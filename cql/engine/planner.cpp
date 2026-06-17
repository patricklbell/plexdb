module cql.engine.planner;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.key;
import cql.engine.schema;
import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;

namespace cql::planner {
    static Optional<U64> find_pk_position(const schema::Table& tbl, const AutoString8& col_name) {
        String8 name(col_name.c_str, col_name.length);
        for (U64 pos = 0; pos < tbl.partition_key_col_indices.length; pos++) {
            if (tbl.cols[tbl.partition_key_col_indices[pos]].name == name) {
                return pos;
            }
        }
        return {};
    }

    static Optional<U64> find_ck_position(const schema::Table& tbl, const AutoString8& col_name) {
        String8 name(col_name.c_str, col_name.length);
        for (U64 pos = 0; pos < tbl.clustering_key_col_indices.length; pos++) {
            if (tbl.cols[tbl.clustering_key_col_indices[pos]].name == name) {
                return pos;
            }
        }
        return {};
    }

    static bool is_null_eval(const Evaluated& eval) {
        if (type_matches_tag<Constant>(eval.value)) {
            return type_matches_tag<Null>(get<Constant>(eval.value).value);
        }
        if (type_matches_tag<ColumnValue>(eval.value)) {
            return type_matches_tag<Null>(get<ColumnValue>(eval.value));
        }
        return false;
    }

    // Apply a single-column range bound to `bounds`. When `direction == DESC`,
    // logical </> map onto byte >/< since DESC values are encoded byte-inverted,
    // so the begin/end roles swap. `partial` marks bounds that cover a prefix
    // shorter than the full composite key (forces prefix-aware comparison).
    static void apply_range_bound(KeyBounds& bounds, Operator op, Sort direction,
                                  DynamicArray<U8> bytes, bool partial) {
        if (direction == Sort::DESC) {
            switch (op) {
                case Operator::lt:
                    op = Operator::gt;
                    break;
                case Operator::le:
                    op = Operator::ge;
                    break;
                case Operator::gt:
                    op = Operator::lt;
                    break;
                case Operator::ge:
                    op = Operator::le;
                    break;
                default:
                    break;
            }
        }
        switch (op) {
            case Operator::lt:
                bounds.end            = move(bytes);
                bounds.has_end        = true;
                bounds.end_inclusive  = false;
                bounds.end_is_partial = partial;
                break;
            case Operator::le:
                bounds.end            = move(bytes);
                bounds.has_end        = true;
                bounds.end_inclusive  = true;
                bounds.end_is_partial = partial;
                break;
            case Operator::gt:
                bounds.begin            = move(bytes);
                bounds.has_begin        = true;
                bounds.begin_inclusive  = false;
                bounds.begin_is_partial = partial;
                break;
            case Operator::ge:
                bounds.begin            = move(bytes);
                bounds.has_begin        = true;
                bounds.begin_inclusive  = true;
                bounds.begin_is_partial = partial;
                break;
            default:
                break;
        }
    }

    // @note Equality values are collected per position so compound keys are only
    // serialized when all positions are filled; partial matches fall to filter.
    // in_seen[i]=true even when the IN list is empty so we can detect zero-row queries.
    struct KeyConstraints {
        DynamicArray<Optional<Evaluated>>     eq_vals;
        DynamicArray<WhereClause::Relation>   eq_rels;
        DynamicArray<DynamicArray<Evaluated>> in_vals;
        DynamicArray<bool>                    in_seen;
    };

    static void init_constraints(KeyConstraints& kc, U64 n) {
        resize(kc.eq_vals, n);
        resize(kc.in_vals, n);
        resize(kc.in_seen, n);
        for (U64 i = 0; i < n; i++) {
            kc.in_seen[i] = false;
        }
    }

    static bool all_positions_eq(const KeyConstraints& kc, U64 n) {
        if (n == 0) {
            return false;
        }
        for (U64 i = 0; i < n; i++) {
            if (!kc.eq_vals[i]) {
                return false;
            }
        }
        return true;
    }

    static bool all_positions_covered(const KeyConstraints& kc, U64 n) {
        for (U64 i = 0; i < n; i++) {
            if (!kc.eq_vals[i] && !kc.in_seen[i]) {
                return false;
            }
        }
        return true;
    }

    static bool any_empty_in(const KeyConstraints& kc, U64 n) {
        for (U64 i = 0; i < n; i++) {
            if (kc.in_seen[i] && kc.in_vals[i].length == 0) {
                return true;
            }
        }
        return false;
    }

    static DynamicArray<Evaluated> drain_eq_combo(KeyConstraints& kc, U64 n) {
        DynamicArray<Evaluated> out;
        for (U64 i = 0; i < n; i++) {
            push_back(out, move(*kc.eq_vals[i]));
        }
        return out;
    }

    template<typename Serialize>
    static void build_cartesian_combos(const KeyConstraints& kc, U64 n, Serialize&& serialize,
                                       DynamicArray<DynamicArray<U8>>& out) {
        DynamicArray<U64> sizes;
        resize(sizes, n);
        U64 total = 1;
        for (U64 i = 0; i < n; i++) {
            sizes[i] = kc.eq_vals[i] ? 1 : kc.in_vals[i].length;
            total *= sizes[i];
        }
        for (U64 idx = 0; idx < total; idx++) {
            DynamicArray<Evaluated> combo;
            U64                     stride = total;
            for (U64 i = 0; i < n; i++) {
                stride /= sizes[i];
                U64 choice = (idx / stride) % sizes[i];
                push_back(combo, kc.eq_vals[i] ? *kc.eq_vals[i] : kc.in_vals[i][choice]);
            }
            push_back(out, serialize(combo));
        }
    }

    Pair<RowLocator, FilterPlan> build_row_locator(const WhereClause& where, const schema::Table& tbl, const EvalContext& ctx) {
        RowLocator locator{};
        FilterPlan filter{};

        U64 n_pk = tbl.partition_key_col_indices.length;
        U64 n_ck = tbl.clustering_key_col_indices.length;

        auto col_is_indexed = [&](U64 ci) -> bool {
            if (!type_matches_tag<type::Basic>(tbl.cols[ci].type.value)) {
                return false;
            }
            for (const auto& idx : tbl.indexes) {
                if (!idx.tombstone && idx.col_idx == ci) {
                    return true;
                }
            }
            return false;
        };
        auto try_capture_index = [&](U64 ci, const Evaluated& eval) {
            if (locator.index_col_idx) {
                return;
            }
            if (is_null_eval(eval)) {
                return;
            }
            if (!col_is_indexed(ci)) {
                return;
            }
            locator.index_col_idx    = ci;
            locator.index_key_prefix = key::make_index_prefix(
                eval, get<type::Basic>(tbl.cols[ci].type.value));
        };

        KeyConstraints pk;
        KeyConstraints ck;
        init_constraints(pk, n_pk);
        init_constraints(ck, n_ck);

        // Multi-column tuple IN: each outer entry is one combination, inner Optional<Evaluated>
        // indexed by CK position (empty if that CK not specified by the tuple).
        DynamicArray<DynamicArray<Optional<Evaluated>>> ck_tuple_in_combos;

        for (const auto& rel : where.relations) {
            visit(rel.value, [&](const auto& r) {
                using T = RemoveCVRef<decltype(r)>;
                if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                    auto pk_pos = find_pk_position(tbl, r.column.identifier);
                    auto ck_pos = find_ck_position(tbl, r.column.identifier);

                    if (static_cast<bool>(pk_pos)) {
                        Evaluated eval = evaluate(r.value, ctx);
                        if (r.operator_ == Operator::eq) {
                            if (!is_null_eval(eval)) {
                                pk.eq_vals[*pk_pos] = eval;
                                push_back(pk.eq_rels, rel);
                                try_capture_index(tbl.partition_key_col_indices[*pk_pos], eval);
                            }
                        } else if (r.operator_ == Operator::in) {
                            // Collect IN values per PK position for multi-partition mutations.
                            // IN (a, b) parses as TupleLiteral; IN [a, b] parses as ListOrVectorLiteral.
                            pk.in_seen[*pk_pos] = true;
                            visit(eval.value, [&](const auto& lv) {
                                using LT = RemoveCVRef<decltype(lv)>;
                                if constexpr (SameAs<LT, ListOrVectorLiteral> || SameAs<LT, TupleLiteral>) {
                                    for (const Term& elem : lv.elements) {
                                        Evaluated ev = evaluate(elem, ctx);
                                        if (!is_null_eval(ev)) {
                                            push_back(pk.in_vals[*pk_pos], ev);
                                        }
                                    }
                                }
                            });
                            // Also add as filter predicate for SELECT (no ALLOW FILTERING needed for PK IN).
                            push_back(filter.predicates, rel);
                        } else if (n_pk == 1 && *pk_pos == 0) {
                            switch (r.operator_) {
                                case Operator::lt:
                                    locator.pk.end           = key::serialize_partition_single(tbl, eval);
                                    locator.pk.has_end       = true;
                                    locator.pk.end_inclusive = false;
                                    break;
                                case Operator::le:
                                    locator.pk.end           = key::serialize_partition_single(tbl, eval);
                                    locator.pk.has_end       = true;
                                    locator.pk.end_inclusive = true;
                                    break;
                                case Operator::gt:
                                    locator.pk.begin           = key::serialize_partition_single(tbl, eval);
                                    locator.pk.has_begin       = true;
                                    locator.pk.begin_inclusive = false;
                                    break;
                                case Operator::ge:
                                    locator.pk.begin           = key::serialize_partition_single(tbl, eval);
                                    locator.pk.has_begin       = true;
                                    locator.pk.begin_inclusive = true;
                                    break;
                                default:
                                    push_back(filter.predicates, rel);
                                    filter.needs_allow_filtering = true;
                                    break;
                            }
                        } else {
                            push_back(filter.predicates, rel);
                            filter.needs_allow_filtering = true;
                        }
                    } else if (static_cast<bool>(ck_pos) && n_ck > 0) {
                        Evaluated eval = evaluate(r.value, ctx);
                        if (r.operator_ == Operator::eq) {
                            if (!is_null_eval(eval)) {
                                ck.eq_vals[*ck_pos] = eval;
                                push_back(ck.eq_rels, rel);
                                try_capture_index(tbl.clustering_key_col_indices[*ck_pos], eval);
                            }
                        } else if (r.operator_ == Operator::in) {
                            ck.in_seen[*ck_pos] = true;
                            visit(eval.value, [&](const auto& lv) {
                                using LT = RemoveCVRef<decltype(lv)>;
                                if constexpr (SameAs<LT, ListOrVectorLiteral> || SameAs<LT, TupleLiteral>) {
                                    for (const Term& elem : lv.elements) {
                                        Evaluated ev = evaluate(elem, ctx);
                                        if (!is_null_eval(ev)) {
                                            push_back(ck.in_vals[*ck_pos], ev);
                                        }
                                    }
                                }
                            });
                            push_back(filter.predicates, rel);
                        } else if (*ck_pos == 0) {
                            Sort dir = tbl.cols[tbl.clustering_key_col_indices[0]].clustering_order;
                            apply_range_bound(locator.ck, r.operator_, dir,
                                              key::serialize_clustering_single(tbl, eval), n_ck > 1);
                            // @note CK range also goes to filter for post-scan evaluation;
                            // does not set needs_allow_filtering — CK filtering is efficient.
                            push_back(filter.predicates, rel);
                        } else {
                            push_back(filter.predicates, rel);
                        }
                    } else {
                        bool covered_by_index = false;
                        if (r.operator_ == Operator::eq) {
                            String8 col_name(r.column.identifier.c_str, r.column.identifier.length);
                            for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                                if (tbl.cols[ci].name == col_name && !tbl.cols[ci].tombstone) {
                                    if (col_is_indexed(ci)) {
                                        Evaluated eval = evaluate(r.value, ctx);
                                        if (!is_null_eval(eval)) {
                                            try_capture_index(ci, eval);
                                            covered_by_index = true;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        push_back(filter.predicates, rel);
                        if (!covered_by_index) {
                            filter.needs_allow_filtering = true;
                        }
                    }
                } else if constexpr (SameAs<T, WhereClause::TupleExpressionRelation>) {
                    if (r.operator_ == Operator::in && r.columns.length > 0) {
                        // (ck1 [, ck2]) IN ((v0a [, v0b]), (v1a [, v1b]))
                        // Only generate ck.in_vals when every column in the tuple is a CK.
                        bool all_ck_in = true;
                        for (U64 i = 0; i < r.columns.length && all_ck_in; i++) {
                            all_ck_in = static_cast<bool>(find_ck_position(tbl, r.columns[i].identifier));
                        }
                        if (all_ck_in) {
                            for (U64 ci = 0; ci < r.columns.length; ci++) {
                                auto ck_pos = find_ck_position(tbl, r.columns[ci].identifier);
                                if (ck_pos) {
                                    ck.in_seen[*ck_pos] = true;
                                }
                            }
                            if (r.columns.length > 1) {
                                // Multi-column: each value-tuple is a pre-paired combination.
                                for (const Term& val_term : r.values) {
                                    DynamicArray<Optional<Evaluated>> combo;
                                    resize(combo, n_ck);
                                    visit(val_term.value, [&](const auto& v) {
                                        using VT = RemoveCVRef<decltype(v)>;
                                        if constexpr (SameAs<VT, TupleLiteral>) {
                                            for (U64 i = 0; i < v.elements.length && i < r.columns.length; i++) {
                                                auto ck_pos = find_ck_position(tbl, r.columns[i].identifier);
                                                if (ck_pos) {
                                                    combo[*ck_pos] = evaluate(v.elements[i], ctx);
                                                }
                                            }
                                        }
                                    });
                                    push_back(ck_tuple_in_combos, move(combo));
                                }
                            } else {
                                // Single-column: feed ck.in_vals for cartesian product.
                                for (const Term& val_term : r.values) {
                                    visit(val_term.value, [&](const auto& v) {
                                        using VT = RemoveCVRef<decltype(v)>;
                                        if constexpr (SameAs<VT, TupleLiteral>) {
                                            auto ck_pos = find_ck_position(tbl, r.columns[0].identifier);
                                            if (ck_pos && v.elements.length > 0) {
                                                push_back(ck.in_vals[*ck_pos], evaluate(v.elements[0], ctx));
                                            }
                                        } else {
                                            auto ck_pos = find_ck_position(tbl, r.columns[0].identifier);
                                            if (ck_pos) {
                                                push_back(ck.in_vals[*ck_pos], evaluate(val_term, ctx));
                                            }
                                        }
                                    });
                                }
                            }
                        }
                        push_back(filter.predicates, rel);
                    } else if (r.operator_ == Operator::eq &&
                               r.columns.length > 0 &&
                               r.columns.length == r.values.length) {
                        // Determine whether all named columns belong to the PK or CK.
                        bool all_pk = (r.columns.length <= n_pk);
                        for (U64 i = 0; i < r.columns.length && all_pk; i++) {
                            all_pk = static_cast<bool>(find_pk_position(tbl, r.columns[i].identifier));
                        }
                        bool all_ck = !all_pk && (r.columns.length <= n_ck);
                        for (U64 i = 0; i < r.columns.length && all_ck; i++) {
                            all_ck = static_cast<bool>(find_ck_position(tbl, r.columns[i].identifier));
                        }

                        if (all_pk) {
                            for (U64 i = 0; i < r.columns.length; i++) {
                                auto pk_pos         = find_pk_position(tbl, r.columns[i].identifier);
                                pk.eq_vals[*pk_pos] = evaluate(r.values[i], ctx);
                            }
                            push_back(pk.eq_rels, rel);
                        } else if (all_ck) {
                            for (U64 i = 0; i < r.columns.length; i++) {
                                auto ck_pos         = find_ck_position(tbl, r.columns[i].identifier);
                                ck.eq_vals[*ck_pos] = evaluate(r.values[i], ctx);
                            }
                            push_back(ck.eq_rels, rel);
                        } else {
                            push_back(filter.predicates, rel);
                            filter.needs_allow_filtering = true;
                        }
                    } else {
                        if (r.columns.length == 1 && r.values.length == 1 && n_ck > 0) {
                            auto ck_pos = find_ck_position(tbl, r.columns[0].identifier);
                            if (ck_pos && *ck_pos == 0) {
                                Evaluated eval = evaluate(r.values[0], ctx);
                                Sort      dir  = tbl.cols[tbl.clustering_key_col_indices[0]].clustering_order;
                                apply_range_bound(locator.ck, r.operator_, dir,
                                                  key::serialize_clustering_single(tbl, eval), n_ck > 1);
                            }
                        }
                        push_back(filter.predicates, rel);
                    }
                } else if constexpr (SameAs<T, WhereClause::TokenRelation>) {
                    push_back(filter.predicates, rel);
                } else {
                    static_assert(!SameAs<T, T>, "missing WHERE clause type");
                }
            });
        }

        auto serialize_pk = [&](const DynamicArray<Evaluated>& combo) {
            return key::serialize_partition(tbl, combo);
        };
        auto serialize_ck = [&](const DynamicArray<Evaluated>& combo) {
            return key::serialize_clustering(tbl, combo);
        };

        if (all_positions_eq(pk, n_pk)) {
            DynamicArray<Evaluated> evals = drain_eq_combo(pk, n_pk);
            locator.pk.begin              = serialize_pk(evals);
            locator.pk.has_begin          = true;
            locator.pk.begin_inclusive    = true;
            locator.pk.is_equality        = true;
        } else if (all_positions_covered(pk, n_pk)) {
            locator.pk.has_in = true;
            build_cartesian_combos(pk, n_pk, serialize_pk, locator.pk.in_values);
            for (const auto& r : pk.eq_rels) {
                push_back(filter.predicates, r);
            }
        } else if (any_empty_in(pk, n_pk)) {
            // @note empty IN list ⇒ zero-row query; has_in with empty in_values short-circuits.
            locator.pk.has_in = true;
        } else {
            for (const auto& r : pk.eq_rels) {
                push_back(filter.predicates, r);
                bool covered = false;
                if (auto rel = visit(r.value, [](const auto& v) -> const WhereClause::ColumnExpressionRelation* {
                        using T = RemoveCVRef<decltype(v)>;
                        if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                            return &v;
                        } else {
                            return nullptr;
                        }
                    })) {
                    String8 col_name(rel->column.identifier.c_str, rel->column.identifier.length);
                    for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                        if (tbl.cols[ci].name == col_name && !tbl.cols[ci].tombstone) {
                            if (col_is_indexed(ci)) {
                                covered = true;
                            }
                            break;
                        }
                    }
                }
                if (!covered) {
                    filter.needs_allow_filtering = true;
                }
            }
        }

        if (n_ck > 0) {
            bool ck_eq_complete = ck.eq_rels.length > 0 && all_positions_eq(ck, n_ck);
            if (ck_eq_complete) {
                DynamicArray<Evaluated> evals = drain_eq_combo(ck, n_ck);
                locator.ck.begin              = serialize_ck(evals);
                locator.ck.has_begin          = true;
                locator.ck.begin_inclusive    = true;
                locator.ck.is_equality        = true;
            } else if (all_positions_covered(ck, n_ck)) {
                locator.ck.has_in = true;
                build_cartesian_combos(ck, n_ck, serialize_ck, locator.ck.in_values);
            } else if (any_empty_in(ck, n_ck)) {
                locator.ck.has_in = true;
            }
            for (const auto& r : ck.eq_rels) {
                push_back(filter.predicates, r);
            }

            // @note each combo is a pre-paired set; positions absent from the tuple fall back to ck.eq_vals.
            for (const auto& combo : ck_tuple_in_combos) {
                bool                    all_covered = true;
                DynamicArray<Evaluated> full_combo;
                for (U64 i = 0; i < n_ck; i++) {
                    if (combo[i]) {
                        push_back(full_combo, *combo[i]);
                    } else if (ck.eq_vals[i]) {
                        push_back(full_combo, *ck.eq_vals[i]);
                    } else {
                        all_covered = false;
                        break;
                    }
                }
                if (all_covered) {
                    push_back(locator.ck.in_values, key::serialize_clustering(tbl, full_combo));
                }
            }
        }

        // @note partial CK prefix EQ (c1=0 on 3-CK table) is not ck_is_equality; convert to prefix range
        if (n_ck > 1 && !locator.ck.is_equality && !locator.ck.has_begin && !locator.ck.has_end &&
            locator.ck.in_values.length == 0 && !locator.ck.has_in) {
            U64 n_prefix = 0;
            for (U64 i = 0; i < n_ck; i++) {
                if (ck.eq_vals[i]) {
                    n_prefix++;
                } else {
                    break;
                }
            }
            if (n_prefix > 0) {
                DynamicArray<U8> prefix;
                if (n_prefix == 1) {
                    prefix = key::serialize_clustering_single(tbl, *ck.eq_vals[0]);
                } else {
                    DynamicArray<Evaluated> prefix_evals;
                    for (U64 i = 0; i < n_prefix; i++) {
                        push_back(prefix_evals, *ck.eq_vals[i]);
                    }
                    prefix = key::serialize_clustering_prefix(tbl, prefix_evals);
                }
                locator.ck.begin            = prefix;
                locator.ck.has_begin        = true;
                locator.ck.begin_inclusive  = true;
                locator.ck.begin_is_partial = true;
                locator.ck.end              = move(prefix);
                locator.ck.has_end          = true;
                locator.ck.end_inclusive    = true;
                locator.ck.end_is_partial   = true;
            }
        }

        // @note CK eq on position N with a smaller-N position missing is filtering inside
        // the partition; a global secondary index on N does not avoid the partition scan.
        bool partition_scoped = locator.pk.is_equality || locator.pk.has_in ||
                                locator.ck.has_begin || locator.ck.has_end;
        if (n_ck > 0 && !locator.ck.is_equality && partition_scoped) {
            bool prefix_broken = false;
            for (U64 i = 0; i < n_ck; i++) {
                bool covered = static_cast<bool>(ck.eq_vals[i]) || ck.in_seen[i];
                if (!covered) {
                    prefix_broken = true;
                } else if (prefix_broken && static_cast<bool>(ck.eq_vals[i])) {
                    filter.needs_allow_filtering = true;
                }
            }
        }

        // @note prefer the partition lookup over the index when both are available.
        if (locator.index_col_idx && (locator.pk.is_equality || locator.pk.has_in)) {
            locator.index_col_idx    = {};
            locator.index_key_prefix = {};
        }

        // Contiguous equality-restricted CK prefix. A single-value IN counts as equality.
        for (U64 i = 0; i < n_ck; i++) {
            if (static_cast<bool>(ck.eq_vals[i])) {
                locator.ck_eq_prefix_len = i + 1;
            } else if (ck.in_seen[i] && ck.in_vals[i].length == 1) {
                locator.ck_eq_prefix_len = i + 1;
            } else {
                break;
            }
        }

        return {move(locator), move(filter)};
    }

    SelectPlan plan_select(const Select& stmt, const schema::Table& tbl, const EvalContext& ctx) {
        SelectPlan plan{};

        if (stmt.where) {
            auto [loc, flt] = build_row_locator(*stmt.where, tbl, ctx);
            plan.locator    = move(loc);
            plan.filter     = move(flt);
        }

        // @note DISTINCT restricts to per-partition projection, so WHERE must address whole
        // partitions only (partition key components or static columns).
        if (stmt.where && stmt.transform && *stmt.transform == Select::Transform::UNIQUE) {
            for (const auto& rel : stmt.where->relations) {
                bool ok = visit(rel.value, [&](const auto& r) -> bool {
                    using T        = RemoveCVRef<decltype(r)>;
                    auto check_col = [&](const AutoString8& ident) -> bool {
                        if (find_pk_position(tbl, ident)) {
                            return true;
                        }
                        String8 name(ident.c_str, ident.length);
                        for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                            if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == name && tbl.cols[ci].is_static) {
                                return true;
                            }
                        }
                        return false;
                    };
                    if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                        return check_col(r.column.identifier);
                    } else if constexpr (SameAs<T, WhereClause::TupleExpressionRelation>) {
                        for (U64 i = 0; i < r.columns.length; i++) {
                            if (!check_col(r.columns[i].identifier)) {
                                return false;
                            }
                        }
                        return true;
                    } else {
                        return true;
                    }
                });
                if (!ok) {
                    plan.result.error = PlanError::DistinctRestrictionInvalid;
                    return plan;
                }
            }
        }

        if (!stmt.allow_filtering && plan.filter.needs_allow_filtering) {
            plan.result.error = PlanError::RequiresAllowFiltering;
            return plan;
        }

        if (stmt.order_by && stmt.order_by->columns.length > 0) {
            // ORDER BY requires the partition key to be fully restricted (eq or IN) so
            // the scan stays within partitions whose iteration we can deterministically
            // order.
            if (!plan.locator.pk.is_equality && !plan.locator.pk.has_in &&
                plan.locator.pk.in_values.length == 0) {
                plan.result.error = PlanError::OrderByOnNonClusteringColumn;
                return plan;
            }
            // @note ORDER BY column[0] may sit anywhere from CK[0] through the last
            // equality-restricted CK position; columns past it must follow CK order
            // positionally.
            String8 first_name(stmt.order_by->columns[0].column.identifier.c_str,
                               stmt.order_by->columns[0].column.identifier.length);
            U64     ck_start = MAX_U64;
            for (U64 p = 0; p < tbl.clustering_key_col_indices.length; p++) {
                if (tbl.cols[tbl.clustering_key_col_indices[p]].name == first_name) {
                    ck_start = p;
                    break;
                }
            }
            if (ck_start == MAX_U64 || ck_start > plan.locator.ck_eq_prefix_len) {
                plan.result.error   = PlanError::OrderByOnNonClusteringColumn;
                plan.result.context = AutoString8(first_name);
                return plan;
            }

            // @note each ORDER BY direction must either match or invert the table's
            // CLUSTERING ORDER, and the choice must be uniform across the list.
            // reverse_clustering is true exactly when every column inverts.
            Optional<bool> opposite_decision;
            for (U64 i = 0; i < stmt.order_by->columns.length; i++) {
                const auto& col_order = stmt.order_by->columns[i];
                String8     name(col_order.column.identifier.c_str, col_order.column.identifier.length);
                U64         ck_pos = ck_start + i;
                if (ck_pos >= tbl.clustering_key_col_indices.length ||
                    tbl.cols[tbl.clustering_key_col_indices[ck_pos]].name != name) {
                    plan.result.error   = PlanError::OrderByOnNonClusteringColumn;
                    plan.result.context = AutoString8(name);
                    return plan;
                }
                Sort tbl_order   = tbl.cols[tbl.clustering_key_col_indices[ck_pos]].clustering_order;
                bool is_opposite = (col_order.sort != tbl_order);
                if (opposite_decision && *opposite_decision != is_opposite) {
                    plan.result.error = PlanError::OrderByOnNonClusteringColumn;
                    return plan;
                }
                opposite_decision = is_opposite;
            }
            plan.locator.reverse_clustering = opposite_decision.has_value() && *opposite_decision;
        }

        for (const auto& sc : stmt.select.clauses) {
            visit(sc.column.value, [&](const auto& sel) {
                using ST = RemoveCVRef<decltype(sel)>;
                if constexpr (SameAs<ST, ColumnName>) {
                    String8 name(sel.identifier.c_str, sel.identifier.length);
                    for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                        if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == name) {
                            push_back(plan.projection.ops, SelectOp{SelectOp::ColumnRef{ci}});
                            break;
                        }
                    }
                } else if constexpr (SameAs<ST, Select::Count>) {
                    push_back(plan.projection.ops, SelectOp{SelectOp::CountStar{}});
                    plan.projection.is_aggregate = true;
                } else {
                    assert_not_implemented("SELECT clause type (function/cast/term) is not implemented");
                }
            });
        }

        return plan;
    }

    static PlanResult validate_mutation_where(const WhereClause& where, const schema::Table& tbl) {
        for (const auto& rel : where.relations) {
            if (!type_matches_tag<WhereClause::ColumnExpressionRelation>(rel.value)) {
                continue;
            }
            const auto& cer    = get<WhereClause::ColumnExpressionRelation>(rel.value);
            auto        pk_pos = find_pk_position(tbl, cer.column.identifier);
            auto        ck_pos = find_ck_position(tbl, cer.column.identifier);

            bool col_exists = false;
            for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == cer.column.identifier) {
                    col_exists = true;
                    break;
                }
            }
            if (!col_exists) {
                return {PlanError::ColumnNotFound, AutoString8(cer.column.identifier)};
            }
            if (!pk_pos && !ck_pos) {
                return {PlanError::NonKeyColumnInMutationWhere, AutoString8(cer.column.identifier)};
            }
            if (static_cast<bool>(pk_pos) &&
                cer.operator_ != Operator::eq && cer.operator_ != Operator::in) {
                AutoString8 op_name;
                switch (cer.operator_) {
                    case Operator::lt:
                        op_name = AutoString8("LT");
                        break;
                    case Operator::gt:
                        op_name = AutoString8("GT");
                        break;
                    case Operator::le:
                        op_name = AutoString8("LTE");
                        break;
                    case Operator::ge:
                        op_name = AutoString8("GTE");
                        break;
                    case Operator::ne:
                        op_name = AutoString8("NEQ");
                        break;
                    case Operator::contains:
                        op_name = AutoString8("CONTAINS");
                        break;
                    case Operator::contains_key:
                        op_name = AutoString8("CONTAINS KEY");
                        break;
                    default:
                        op_name = AutoString8("UNKNOWN");
                        break;
                }
                return {PlanError::NonEqInOnPartitionKeyMutation, move(op_name)};
            }
        }
        return {PlanError::None, {}};
    }

    MutationPlan plan_update(const Update& stmt, const schema::Table& tbl, const EvalContext& ctx) {
        MutationPlan plan{};

        for (const auto& rel : stmt.where.relations) {
            if (type_matches_tag<WhereClause::TokenRelation>(rel.value)) {
                plan.result.error = PlanError::TokenFunctionInMutation;
                return plan;
            }
        }

        if (auto wr = validate_mutation_where(stmt.where, tbl); wr.error != PlanError::None) {
            plan.result = move(wr);
            return plan;
        }

        // Check for duplicate column assignments.
        for (U64 i = 0; i < stmt.assignments.length; i++) {
            for (U64 j = 0; j < i; j++) {
                if (stmt.assignments[i].target.column.identifier == stmt.assignments[j].target.column.identifier) {
                    plan.result.error   = PlanError::DuplicateColumnInMutation;
                    plan.result.context = AutoString8(stmt.assignments[i].target.column.identifier);
                    return plan;
                }
            }
        }

        {
            auto [loc, flt] = build_row_locator(stmt.where, tbl, ctx);
            plan.locator    = move(loc);
            plan.filter     = move(flt);
        }

        if (!plan.locator.pk.is_equality && plan.locator.pk.in_values.length == 0 && !plan.locator.pk.has_in) {
            plan.result.error   = PlanError::MissingPartitionKey;
            plan.result.context = AutoString8(tbl.cols[tbl.partition_key_col_indices[0]].name);
            return plan;
        }

        // Process assignments first to determine if CK is required.
        // Static-only updates don't need the clustering key.
        bool has_non_static_assignment = false;
        for (const auto& assign : stmt.assignments) {
            assert_true_not_implemented(!assign.target.access, "subscript/field access in UPDATE SET is not implemented");

            Optional<U64> col_idx;
            String8       col_name(assign.target.column.identifier.c_str, assign.target.column.identifier.length);
            for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == col_name) {
                    col_idx = ci;
                    break;
                }
            }
            if (!col_idx) {
                plan.result.error   = PlanError::ColumnNotFound;
                plan.result.context = AutoString8(col_name);
                return plan;
            }

            if (type_matches_tag<TOIArithmeticOperation>(assign.value.value)) {
                plan.result.error   = PlanError::CounterOperationOnNonCounter;
                plan.result.context = AutoString8(col_name);
                return plan;
            }
            assert_true_not_implemented(!type_matches_tag<AutoString8>(assign.value.value),
                                        "column reference in UPDATE assignment is not implemented");
            if (!tbl.cols[*col_idx].is_static) {
                has_non_static_assignment = true;
            }
            push_back(plan.spec.updates, ColumnUpdate{*col_idx, evaluate(assign.value, ctx)});
        }

        if (tbl.clustering_key_col_indices.length > 0) {
            bool ck_specified = plan.locator.ck.is_equality || plan.locator.ck.in_values.length > 0 || plan.locator.ck.has_in;
            if (!ck_specified && has_non_static_assignment) {
                plan.result.error   = PlanError::MissingClusteringKey;
                plan.result.context = AutoString8(tbl.cols[tbl.clustering_key_col_indices[0]].name);
                return plan;
            }
            if (ck_specified && !has_non_static_assignment) {
                plan.result.error = PlanError::StaticOnlyUpdateWithCK;
                return plan;
            }
        }

        // Include PK and CK equality values in the spec so apply_mutation can populate
        // key columns when creating a new row (upsert behaviour).
        for (const auto& rel : stmt.where.relations) {
            visit(rel.value, [&](const auto& r) {
                using R = RemoveCVRef<decltype(r)>;
                if constexpr (SameAs<R, WhereClause::ColumnExpressionRelation>) {
                    if (r.operator_ == Operator::eq) {
                        String8 col_name(r.column.identifier.c_str, r.column.identifier.length);
                        for (U64 pk_ci : tbl.partition_key_col_indices) {
                            if (!tbl.cols[pk_ci].tombstone && tbl.cols[pk_ci].name == col_name) {
                                push_back(plan.spec.updates, ColumnUpdate{pk_ci, evaluate(r.value, ctx)});
                                break;
                            }
                        }
                        for (U64 ck_ci : tbl.clustering_key_col_indices) {
                            if (!tbl.cols[ck_ci].tombstone && tbl.cols[ck_ci].name == col_name) {
                                push_back(plan.spec.updates, ColumnUpdate{ck_ci, evaluate(r.value, ctx)});
                                break;
                            }
                        }
                    }
                }
            });
        }

        return plan;
    }

    MutationPlan plan_delete(const Delete& stmt, const schema::Table& tbl, const EvalContext& ctx) {
        MutationPlan plan{};

        for (const auto& rel : stmt.where.relations) {
            if (type_matches_tag<WhereClause::TokenRelation>(rel.value)) {
                plan.result.error = PlanError::TokenFunctionInMutation;
                return plan;
            }
        }

        if (auto wr = validate_mutation_where(stmt.where, tbl); wr.error != PlanError::None) {
            plan.result = move(wr);
            return plan;
        }

        {
            auto [loc, flt] = build_row_locator(stmt.where, tbl, ctx);
            plan.locator    = move(loc);
            plan.filter     = move(flt);
        }

        if (!plan.locator.pk.is_equality && plan.locator.pk.in_values.length == 0 && !plan.locator.pk.has_in) {
            plan.result.error   = PlanError::MissingPartitionKey;
            plan.result.context = AutoString8(tbl.cols[tbl.partition_key_col_indices[0]].name);
            return plan;
        }

        bool ck_specified = plan.locator.ck.is_equality || plan.locator.ck.in_values.length > 0 ||
                            plan.locator.ck.has_in || plan.locator.ck.has_begin || plan.locator.ck.has_end;

        if (stmt.selections.length == 0) {
            plan.spec.is_full_delete = true;
        } else {
            bool is_static_only = true;
            for (const auto& sel : stmt.selections) {
                assert_true_not_implemented(!sel.access, "subscript/field access in DELETE is not implemented");

                Optional<U64> col_idx;
                String8       col_name(sel.column.identifier.c_str, sel.column.identifier.length);
                for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                    if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == col_name) {
                        col_idx = ci;
                        if (!tbl.cols[ci].is_static) {
                            is_static_only = false;
                        }
                        break;
                    }
                }
                if (!col_idx) {
                    plan.result.error   = PlanError::ColumnNotFound;
                    plan.result.context = AutoString8(col_name);
                    return plan;
                }
                push_back(plan.spec.updates, ColumnUpdate{*col_idx, Evaluated{Constant{Null{}}}});
            }

            if (is_static_only && ck_specified) {
                plan.result.error = PlanError::StaticOnlyDeleteWithCK;
                return plan;
            }

            bool has_ck_range = (plan.locator.ck.has_begin || plan.locator.ck.has_end) && !plan.locator.ck.is_equality;
            if (!is_static_only && has_ck_range) {
                plan.result.error = PlanError::RangeDeletionOnSpecificColumns;
                return plan;
            }
        }

        // Skip CK check if the PK IN list is empty — the mutation is guaranteed to be a no-op.
        bool pk_will_be_empty = plan.locator.pk.has_in && plan.locator.pk.in_values.length == 0;
        // Range or full-partition deletes are valid even without CK equality/IN.
        // Missing CK only applies to column-level (non-static) deletes that need a specific row.
        bool needs_ck = !plan.spec.is_full_delete && !pk_will_be_empty;
        if (needs_ck && tbl.clustering_key_col_indices.length > 0 && !ck_specified) {
            bool all_static = true;
            for (const auto& upd : plan.spec.updates) {
                if (upd.col_idx < tbl.cols.length && !tbl.cols[upd.col_idx].is_static) {
                    all_static = false;
                    break;
                }
            }
            if (!all_static) {
                plan.result.error   = PlanError::MissingClusteringKey;
                plan.result.context = AutoString8(tbl.cols[tbl.clustering_key_col_indices[0]].name);
                return plan;
            }
        }

        return plan;
    }

} // namespace cql::planner
