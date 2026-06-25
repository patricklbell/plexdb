module cql.engine.planner;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.io;
import cql.engine.key;
import cql.engine.schema;
import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;

namespace cql::planner {
    // @note parser lowercases identifiers (see unquoted_identifier), so all matches are lowercase-only.
    struct ConversionTypeName {
        const char* name;
        type::Basic type;
    };
    static constexpr ConversionTypeName kConversionTypeNames[] = {
        {      "int",      type::Basic::int_},
        {   "bigint",    type::Basic::bigint},
        { "smallint",  type::Basic::smallint},
        {  "tinyint",   type::Basic::tinyint},
        {  "counter",   type::Basic::counter},
        {    "float",    type::Basic::float_},
        {   "double",   type::Basic::double_},
        {     "text",      type::Basic::text},
        {    "ascii",     type::Basic::ascii},
        {  "varchar",   type::Basic::varchar},
        {"timestamp", type::Basic::timestamp},
        {     "date",      type::Basic::date},
        {     "time",      type::Basic::time},
        {     "uuid",      type::Basic::uuid},
        { "timeuuid",  type::Basic::timeuuid},
        {  "boolean",   type::Basic::boolean},
        {     "inet",      type::Basic::inet},
        {     "blob",      type::Basic::blob},
    };
    static Optional<type::Basic> typed_conversion_type_from_token(String8 tok) {
        for (const auto& e : kConversionTypeNames) {
            if (e.name == tok) {
                return e.type;
            }
        }
        return {};
    }

    // Parse a function name like `bigintasblob` into (from, to) types — the `<T1>As<T2>` pattern.
    static Optional<Pair<type::Basic, type::Basic>> parse_typed_conversion_name(String8 name) {
        for (U64 i = 1; i + 2 < name.length; i++) {
            if (name.data[i] != 'a' || name.data[i + 1] != 's') {
                continue;
            }
            auto from = typed_conversion_type_from_token(String8{name.data, i});
            auto to   = typed_conversion_type_from_token(String8{name.data + i + 2, name.length - i - 2});
            if (from && to) {
                return Pair<type::Basic, type::Basic>{*from, *to};
            }
        }
        return {};
    }

    static void reverse_conversions_into(DynamicArray<SelectOp::Conversion>& dst, const DynamicArray<SelectOp::Conversion>& src) {
        for (U64 i = src.length; i > 0; i--) {
            push_back(dst, src[i - 1]);
        }
    }

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

    static bool twi_has_column_ref(const TermWithIdentifiers& twi) {
        return visit(twi.value, [](const auto& v) -> bool {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, AutoString8>) {
                return true;
            } else if constexpr (SameAs<T, TOIArithmeticOperation>) {
                return visit(v.value, [](const auto& arith) -> bool {
                    using AT = Decay<decltype(arith)>;
                    if constexpr (SameAs<AT, TOIUnaryMinus>) {
                        return twi_has_column_ref(arith.operand);
                    } else if constexpr (SameAs<AT, TOIBinaryArithmetic>) {
                        return twi_has_column_ref(arith.lhs) || twi_has_column_ref(arith.rhs);
                    } else {
                        return false;
                    }
                });
            } else {
                return false;
            }
        });
    }

    static bool col_is_counter(const schema::Column& col) {
        return type_matches_tag<type::Basic>(col.type.value) && get<type::Basic>(col.type.value) == type::Basic::counter;
    }

    bool table_has_counter(const schema::Table& tbl) {
        for (const auto& col : tbl.cols) {
            if (!col.tombstone && col_is_counter(col)) {
                return true;
            }
        }
        return false;
    }

    static bool col_is_collection(const schema::Column& col) {
        const auto& tv = col.type.value;
        return type_matches_tag<type::List>(tv) || type_matches_tag<type::Set>(tv) || type_matches_tag<type::Map>(tv);
    }

    // @note matches `c OP X` or `X OP c` where `c` is the target column name
    // and the other operand contains no further column references.
    struct CompoundForm {
        ArithmeticOperator         op;
        bool                       col_on_left;
        const TermWithIdentifiers* other;
    };
    static Optional<CompoundForm> match_compound_form(const TermWithIdentifiers& twi, const String8& target_col) {
        if (!type_matches_tag<TOIArithmeticOperation>(twi.value)) {
            return {};
        }
        const auto& arith = get<TOIArithmeticOperation>(twi.value);
        if (!type_matches_tag<TOIBinaryArithmetic>(arith.value)) {
            return {};
        }
        const auto& bin       = get<TOIBinaryArithmetic>(arith.value);
        auto        is_target = [&](const TermWithIdentifiers& side) -> bool {
            if (!type_matches_tag<AutoString8>(side.value)) {
                return false;
            }
            const auto& name = get<AutoString8>(side.value);
            return String8(name.c_str, name.length) == target_col;
        };
        bool lhs_is_target = is_target(bin.lhs);
        bool rhs_is_target = is_target(bin.rhs);
        if (lhs_is_target && !twi_has_column_ref(bin.rhs)) {
            return CompoundForm{bin.op, true, &bin.rhs};
        }
        if (rhs_is_target && !twi_has_column_ref(bin.lhs)) {
            return CompoundForm{bin.op, false, &bin.lhs};
        }
        return {};
    }

    // Cassandra allows counter writes only in the form `c = c + n` or `c = c - n`,
    // where the LHS column reference matches the assignment target and the RHS contains
    // no further column references (constants, bind markers, or arithmetic over those).
    static bool is_counter_increment_form(const TermWithIdentifiers& twi, const String8& target_col) {
        if (!type_matches_tag<TOIArithmeticOperation>(twi.value)) {
            return false;
        }
        const auto& arith = get<TOIArithmeticOperation>(twi.value);
        if (!type_matches_tag<TOIBinaryArithmetic>(arith.value)) {
            return false;
        }
        const auto& bin = get<TOIBinaryArithmetic>(arith.value);
        if (bin.op != ArithmeticOperator::plus && bin.op != ArithmeticOperator::minus) {
            return false;
        }
        if (!type_matches_tag<AutoString8>(bin.lhs.value)) {
            return false;
        }
        const auto& lhs_name = get<AutoString8>(bin.lhs.value);
        if (String8(lhs_name.c_str, lhs_name.length) != target_col) {
            return false;
        }
        return !twi_has_column_ref(bin.rhs);
    }

    static bool term_is_literal_null(const Term& t) {
        return type_matches_tag<Constant>(t.value) && type_matches_tag<Null>(get<Constant>(t.value).value);
    }

    // @note Unset is only meaningful in INSERT/UPDATE assignment positions, where it
    // means "leave this column unchanged". In a WHERE relation it has no meaning, so
    // Cassandra rejects it as Invalid. The check has to descend through tuple/list/set/
    // map literals because `IN (?, ?, ?)` style binds each `?` as a child element.
    static bool term_contains_unset_binding(const Term& term, const EvalContext& ctx) {
        return visit(term.value, [&](const auto& v) -> bool {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, BindMarker> || SameAs<T, Constant>) {
                Evaluated e = evaluate(term, ctx);
                if (!type_matches_tag<Constant>(e.value)) {
                    return false;
                }
                return type_matches_tag<Unset>(get<Constant>(e.value).value);
            } else if constexpr (SameAs<T, ListOrVectorLiteral> || SameAs<T, TupleLiteral>) {
                for (const auto& e : v.elements) {
                    if (term_contains_unset_binding(e, ctx)) {
                        return true;
                    }
                }
                return false;
            } else if constexpr (SameAs<T, SetLiteral>) {
                for (const auto& e : v.keys) {
                    if (term_contains_unset_binding(e, ctx)) {
                        return true;
                    }
                }
                return false;
            } else if constexpr (SameAs<T, MapLiteral>) {
                for (const auto& p : v.key_values) {
                    if (term_contains_unset_binding(p.first, ctx) || term_contains_unset_binding(p.second, ctx)) {
                        return true;
                    }
                }
                return false;
            } else {
                return false;
            }
        });
    }

    static bool where_has_unset_binding(const WhereClause& where, const EvalContext& ctx) {
        for (const auto& rel : where.relations) {
            bool hit = visit(rel.value, [&](const auto& r) -> bool {
                using T = RemoveCVRef<decltype(r)>;
                if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                    return term_contains_unset_binding(r.value, ctx);
                } else if constexpr (SameAs<T, WhereClause::TupleExpressionRelation>) {
                    for (const auto& val : r.values) {
                        if (term_contains_unset_binding(val, ctx)) {
                            return true;
                        }
                    }
                    return false;
                } else if constexpr (SameAs<T, WhereClause::TokenRelation>) {
                    return term_contains_unset_binding(r.value, ctx);
                } else if constexpr (SameAs<T, WhereClause::SubscriptedRelation>) {
                    return term_contains_unset_binding(r.subscript, ctx) || term_contains_unset_binding(r.value, ctx);
                } else {
                    return false;
                }
            });
            if (hit) {
                return true;
            }
        }
        return false;
    }

    // @note Cassandra reports counter-specific message for any predicate that
    // compares a counter column to a literal null — match that behavior.
    static bool where_has_null_counter_predicate(const WhereClause& where, const schema::Table& tbl) {
        for (const auto& rel : where.relations) {
            bool hit = visit(rel.value, [&](const auto& r) -> bool {
                using T = RemoveCVRef<decltype(r)>;
                if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                    if (!term_is_literal_null(r.value)) {
                        return false;
                    }
                    String8 cname(r.column.identifier.c_str, r.column.identifier.length);
                    for (const auto& col : tbl.cols) {
                        if (!col.tombstone && col.name == cname && col_is_counter(col)) {
                            return true;
                        }
                    }
                }
                return false;
            });
            if (hit) {
                return true;
            }
        }
        return false;
    }

    // Apply a single-column range bound to `bounds`. When `direction == DESC`,
    // logical </> map onto byte >/< since DESC values are encoded byte-inverted,
    // so the begin/end roles swap. `partial` marks bounds that cover a prefix
    // shorter than the full composite key (forces prefix-aware comparison).
    static void apply_range_bound(KeyBounds& bounds, Operator op, Sort direction, DynamicArray<U8> bytes, bool partial) {
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
    static void build_cartesian_combos(const KeyConstraints& kc, U64 n, Serialize&& serialize, DynamicArray<DynamicArray<U8>>& out) {
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

    // @note matches the kind a collection-index lookup needs for `col CONTAINS x`
    // (Values for list/set/map) or `col CONTAINS KEY x` (Keys for map). Returns
    // false when no compatible index exists or the operator is wrong for the column.
    static bool try_capture_collection_index(RowLocator& locator, const schema::Table& tbl, U64 ci, Operator op, const Evaluated& eval) {
        if (locator.index_col_idx) {
            return false;
        }
        const auto&           col_type = tbl.cols[ci].type;
        schema::IndexKind     want_kind;
        Optional<type::Basic> elem_basic;
        visit(col_type.value, [&](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::List> || SameAs<T, type::Vector>) {
                if (op == Operator::contains && type_matches_tag<type::Basic>(v.element.value)) {
                    want_kind  = schema::IndexKind::Values;
                    elem_basic = get<type::Basic>(v.element.value);
                }
            } else if constexpr (SameAs<T, type::Set>) {
                if (op == Operator::contains && type_matches_tag<type::Basic>(v.key.value)) {
                    want_kind  = schema::IndexKind::Values;
                    elem_basic = get<type::Basic>(v.key.value);
                }
            } else if constexpr (SameAs<T, type::Map>) {
                if (op == Operator::contains && type_matches_tag<type::Basic>(v.value.value)) {
                    want_kind  = schema::IndexKind::Values;
                    elem_basic = get<type::Basic>(v.value.value);
                } else if (op == Operator::contains_key && type_matches_tag<type::Basic>(v.key.value)) {
                    want_kind  = schema::IndexKind::Keys;
                    elem_basic = get<type::Basic>(v.key.value);
                }
            }
        });
        if (!elem_basic) {
            return false;
        }
        const schema::Index* matched = nullptr;
        for (const auto& idx : tbl.indexes) {
            if (!idx.tombstone && idx.col_idx == ci && idx.kind == want_kind) {
                matched = &idx;
                break;
            }
        }
        if (matched == nullptr) {
            return false;
        }
        locator.index_col_idx    = ci;
        locator.index_key_prefix = key::make_index_prefix(eval, *elem_basic);
        return true;
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
                eval, get<type::Basic>(tbl.cols[ci].type.value)
            );
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
                            apply_range_bound(locator.ck, r.operator_, dir, key::serialize_clustering_single(tbl, eval), n_ck > 1);
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
                        } else if (r.operator_ == Operator::contains || r.operator_ == Operator::contains_key) {
                            String8 col_name(r.column.identifier.c_str, r.column.identifier.length);
                            for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                                if (tbl.cols[ci].name == col_name && !tbl.cols[ci].tombstone) {
                                    Evaluated eval = evaluate(r.value, ctx);
                                    if (!is_null_eval(eval)) {
                                        covered_by_index = try_capture_collection_index(
                                            locator, tbl, ci, r.operator_, eval
                                        );
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
                    } else if (r.operator_ == Operator::eq && tuple_rhs_is_compatible(r)) {
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
                                pk.eq_vals[*pk_pos] = evaluate(tuple_value_at(r, i), ctx);
                            }
                            push_back(pk.eq_rels, rel);
                        } else if (all_ck) {
                            for (U64 i = 0; i < r.columns.length; i++) {
                                auto ck_pos         = find_ck_position(tbl, r.columns[i].identifier);
                                ck.eq_vals[*ck_pos] = evaluate(tuple_value_at(r, i), ctx);
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
                                apply_range_bound(locator.ck, r.operator_, dir, key::serialize_clustering_single(tbl, eval), n_ck > 1);
                            }
                        } else if (is_inequality(r.operator_) && r.columns.length > 1 && tuple_rhs_is_compatible(r) && n_ck > 0) {
                            // Lex compare on a parenthesized clustering-key prefix
                            // requires the LHS to be a prefix CK[0..k-1] and all referenced
                            // CK columns to share one clustering order.
                            bool prefix_ok = (r.columns.length <= n_ck);
                            for (U64 i = 0; i < r.columns.length && prefix_ok; i++) {
                                auto pos  = find_ck_position(tbl, r.columns[i].identifier);
                                prefix_ok = static_cast<bool>(pos) && *pos == i;
                            }
                            if (prefix_ok) {
                                Sort dir      = tbl.cols[tbl.clustering_key_col_indices[0]].clustering_order;
                                bool same_dir = true;
                                for (U64 i = 1; i < r.columns.length; i++) {
                                    if (tbl.cols[tbl.clustering_key_col_indices[i]].clustering_order != dir) {
                                        same_dir = false;
                                        break;
                                    }
                                }
                                if (same_dir) {
                                    DynamicArray<Evaluated> evals;
                                    for (U64 i = 0; i < r.columns.length; i++) {
                                        push_back(evals, evaluate(tuple_value_at(r, i), ctx));
                                    }
                                    auto             prefix_view = TArrayView<const Evaluated, U64>{evals.ptr, evals.length};
                                    DynamicArray<U8> bytes       = key::serialize_clustering_prefix(tbl, prefix_view);
                                    bool             partial     = r.columns.length < n_ck;
                                    apply_range_bound(locator.ck, r.operator_, dir, move(bytes), partial);
                                }
                            }
                        }
                        push_back(filter.predicates, rel);
                    }
                } else if constexpr (SameAs<T, WhereClause::TokenRelation>) {
                    push_back(filter.predicates, rel);
                } else if constexpr (SameAs<T, WhereClause::SubscriptedRelation>) {
                    bool covered_by_index = false;
                    if (r.operator_ == Operator::eq) {
                        String8 col_name(r.column.identifier.c_str, r.column.identifier.length);
                        for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                            if (tbl.cols[ci].name == col_name && !tbl.cols[ci].tombstone) {
                                if (type_matches_tag<type::Map>(tbl.cols[ci].type.value)) {
                                    const auto& m        = get<type::Map>(tbl.cols[ci].type.value);
                                    Evaluated   key_eval = evaluate(r.subscript, ctx);
                                    Evaluated   val_eval = evaluate(r.value, ctx);
                                    if (!is_null_eval(key_eval) && !is_null_eval(val_eval) && type_matches_tag<type::Basic>(m.key.value) && type_matches_tag<type::Basic>(m.value.value)) {
                                        for (const auto& idx : tbl.indexes) {
                                            if (!idx.tombstone && idx.col_idx == ci && idx.kind == schema::IndexKind::Entries && !locator.index_col_idx) {
                                                locator.index_col_idx    = ci;
                                                locator.index_key_prefix = key::make_index_prefix_entries(key_eval, get<type::Basic>(m.key.value), val_eval, get<type::Basic>(m.value.value));
                                                covered_by_index         = true;
                                                break;
                                            }
                                        }
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
        if (n_ck > 1 && !locator.ck.is_equality && !locator.ck.has_begin && !locator.ck.has_end && locator.ck.in_values.length == 0 && !locator.ck.has_in) {
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
        bool partition_scoped = locator.pk.is_equality || locator.pk.has_in || locator.ck.has_begin || locator.ck.has_end;
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

        if (stmt.where && where_has_unset_binding(*stmt.where, ctx)) {
            plan.result.error = PlanError::UnsetValueInWhere;
            return plan;
        }

        if (stmt.where && where_has_null_counter_predicate(*stmt.where, tbl)) {
            plan.result.error = PlanError::NullValueForCounter;
            return plan;
        }

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
            if (!plan.locator.pk.is_equality && !plan.locator.pk.has_in && plan.locator.pk.in_values.length == 0) {
                plan.result.error = PlanError::OrderByOnNonClusteringColumn;
                return plan;
            }
            // @note ORDER BY column[0] may sit anywhere from CK[0] through the last
            // equality-restricted CK position; columns past it must follow CK order
            // positionally.
            String8 first_name(stmt.order_by->columns[0].column.identifier.c_str, stmt.order_by->columns[0].column.identifier.length);
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
                if (ck_pos >= tbl.clustering_key_col_indices.length || tbl.cols[tbl.clustering_key_col_indices[ck_pos]].name != name) {
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

        // Lower a Function selector for `ttl(c)` or `writetime(c)` into the matching SelectOp.
        // Returns false (with plan.result.error set) when the argument shape or column kind is invalid.
        auto resolve_meta_selector = [&](const Select::Function& fn, bool is_ttl) -> bool {
            PlanError   shape_error = is_ttl ? PlanError::InvalidTtlArgument : PlanError::InvalidWritetimeArgument;
            const char* fn_label    = is_ttl ? "ttl" : "writetime";
            if (fn.arguments.length != 1 || !type_matches_tag<ColumnName>(fn.arguments[0].value)) {
                plan.result.error   = shape_error;
                plan.result.context = AutoString8(fn_label) + AutoString8(" requires a single column argument");
                return false;
            }
            const auto& cn = get<ColumnName>(fn.arguments[0].value);
            String8     name(cn.identifier.c_str, cn.identifier.length);
            for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                if (tbl.cols[ci].tombstone || tbl.cols[ci].name != name) {
                    continue;
                }
                if (tbl.cols[ci].key_kind != schema::KeyKind::None) {
                    plan.result.error   = shape_error;
                    plan.result.context = AutoString8("Cannot use selection function ") + AutoString8(fn_label) + AutoString8(" on PRIMARY KEY part ") + AutoString8(cn.identifier);
                    return false;
                }
                if (col_is_collection(tbl.cols[ci])) {
                    plan.result.error   = shape_error;
                    plan.result.context = AutoString8("Cannot use selection function ") + AutoString8(fn_label) + AutoString8(" on collection column ") + AutoString8(cn.identifier);
                    return false;
                }
                push_back(plan.projection.ops, is_ttl ? SelectOp{SelectOp::TtlOf{ci}, {}} : SelectOp{SelectOp::WritetimeOf{ci}, {}});
                return true;
            }
            plan.result.error   = PlanError::ColumnNotFound;
            plan.result.context = AutoString8(cn.identifier);
            return false;
        };

        for (const auto& sc : stmt.select.clauses) {
            bool ok = visit(sc.column.value, [&](const auto& sel) -> bool {
                using ST = RemoveCVRef<decltype(sel)>;
                if constexpr (SameAs<ST, ColumnName>) {
                    String8 name(sel.identifier.c_str, sel.identifier.length);
                    for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                        if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == name) {
                            push_back(plan.projection.ops, SelectOp{SelectOp::ColumnRef{ci}, {}});
                            break;
                        }
                    }
                    return true;
                } else if constexpr (SameAs<ST, Select::Count>) {
                    push_back(plan.projection.ops, SelectOp{SelectOp::CountStar{}, {}});
                    plan.projection.is_aggregate = true;
                    return true;
                } else if constexpr (SameAs<ST, Select::Function>) {
                    String8 fname(sel.function_name.c_str, sel.function_name.length);
                    if (fname == "ttl" || fname == "TTL") {
                        return resolve_meta_selector(sel, true);
                    }
                    if (fname == "writetime" || fname == "WRITETIME") {
                        return resolve_meta_selector(sel, false);
                    }
                    // @note Recursively unwraps nested `<T>As<U>` calls into a conversion stack; the
                    // innermost argument is a ColumnName, ttl(c), or writetime(c).
                    if (auto from_to = parse_typed_conversion_name(fname)) {
                        DynamicArray<SelectOp::Conversion> conversions;
                        const Select::Function*            inner_fn = &sel;
                        type::Basic                        cur_from = from_to->first;
                        type::Basic                        cur_to   = from_to->second;
                        while (true) {
                            push_back(conversions, SelectOp::Conversion{cur_from, cur_to});
                            if (inner_fn->arguments.length != 1) {
                                plan.result.error   = PlanError::ColumnNotFound;
                                plan.result.context = AutoString8("Type conversion function requires a single argument");
                                return false;
                            }
                            const Select::Selector& arg = inner_fn->arguments[0];
                            if (type_matches_tag<ColumnName>(arg.value)) {
                                const auto& cn = get<ColumnName>(arg.value);
                                String8     name(cn.identifier.c_str, cn.identifier.length);
                                for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                                    if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == name) {
                                        if (!type_matches_tag<type::Basic>(tbl.cols[ci].type.value)) {
                                            plan.result.error   = PlanError::ColumnNotFound;
                                            plan.result.context = AutoString8("Type conversion requires a basic-typed column");
                                            return false;
                                        }
                                        if (get<type::Basic>(tbl.cols[ci].type.value) != cur_from) {
                                            plan.result.error   = PlanError::ColumnNotFound;
                                            plan.result.context = AutoString8("Type conversion argument type does not match column type");
                                            return false;
                                        }
                                        SelectOp op{SelectOp::ColumnRef{ci}, {}};
                                        reverse_conversions_into(op.conversions, conversions);
                                        push_back(plan.projection.ops, move(op));
                                        return true;
                                    }
                                }
                                plan.result.error   = PlanError::ColumnNotFound;
                                plan.result.context = AutoString8(cn.identifier);
                                return false;
                            }
                            if (type_matches_tag<Select::Function>(arg.value)) {
                                const auto& fc = get<Select::Function>(arg.value);
                                String8     inner_name(fc.function_name.c_str, fc.function_name.length);
                                if (inner_name == "ttl" || inner_name == "writetime") {
                                    bool        is_ttl_inner = inner_name == "ttl";
                                    type::Basic meta_type    = is_ttl_inner ? type::Basic::int_ : type::Basic::bigint;
                                    if (cur_from != meta_type) {
                                        plan.result.error   = PlanError::ColumnNotFound;
                                        plan.result.context = AutoString8("Type conversion argument type does not match inner function result type");
                                        return false;
                                    }
                                    U64 ops_before = plan.projection.ops.length;
                                    if (!resolve_meta_selector(fc, is_ttl_inner)) {
                                        return false;
                                    }
                                    if (plan.projection.ops.length != ops_before + 1) {
                                        plan.result.error = PlanError::ColumnNotFound;
                                        return false;
                                    }
                                    auto& last_op = plan.projection.ops[plan.projection.ops.length - 1];
                                    reverse_conversions_into(last_op.conversions, conversions);
                                    return true;
                                }
                                if (auto nested = parse_typed_conversion_name(inner_name)) {
                                    if (cur_from != nested->second) {
                                        plan.result.error   = PlanError::ColumnNotFound;
                                        plan.result.context = AutoString8("Type conversion argument type does not match inner conversion result type");
                                        return false;
                                    }
                                    cur_from = nested->first;
                                    cur_to   = nested->second;
                                    inner_fn = &fc;
                                    continue;
                                }
                            }
                            plan.result.error   = PlanError::ColumnNotFound;
                            plan.result.context = AutoString8("Unsupported argument for type conversion function");
                            return false;
                        }
                    }
                    plan.result.error   = PlanError::ColumnNotFound;
                    plan.result.context = AutoString8("Unknown function ") + AutoString8(sel.function_name);
                    return false;
                } else {
                    assert_not_implemented("SELECT clause type (cast) is not implemented");
                    return true;
                }
            });
            if (!ok) {
                return plan;
            }
        }

        auto add_needed = [&](U64 ci) {
            for (U64 i = 0; i < plan.projection.needed_cols.length; i++) {
                if (plan.projection.needed_cols[i] == ci) {
                    return;
                }
            }
            push_back(plan.projection.needed_cols, ci);
        };
        for (const auto& op : plan.projection.ops) {
            if (type_matches_tag<SelectOp::ColumnRef>(op.value)) {
                add_needed(get<SelectOp::ColumnRef>(op.value).col_idx);
            } else if (type_matches_tag<SelectOp::TtlOf>(op.value)) {
                add_needed(get<SelectOp::TtlOf>(op.value).col_idx);
            } else if (type_matches_tag<SelectOp::WritetimeOf>(op.value)) {
                add_needed(get<SelectOp::WritetimeOf>(op.value).col_idx);
            }
        }
        auto add_needed_by_name = [&](const AutoString8& ident) {
            String8 name(ident.c_str, ident.length);
            for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                if (!tbl.cols[ci].tombstone && tbl.cols[ci].name == name) {
                    add_needed(ci);
                    return;
                }
            }
        };
        for (const auto& rel : plan.filter.predicates) {
            visit(rel.value, [&](const auto& r) {
                using T = RemoveCVRef<decltype(r)>;
                if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                    add_needed_by_name(r.column.identifier);
                } else if constexpr (SameAs<T, WhereClause::TupleExpressionRelation>) {
                    for (U64 i = 0; i < r.columns.length; i++) {
                        add_needed_by_name(r.columns[i].identifier);
                    }
                } else if constexpr (SameAs<T, WhereClause::SubscriptedRelation>) {
                    add_needed_by_name(r.column.identifier);
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
            if (static_cast<bool>(pk_pos) && cer.operator_ != Operator::eq && cer.operator_ != Operator::in) {
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

        if (where_has_unset_binding(stmt.where, ctx)) {
            plan.result.error = PlanError::UnsetValueInWhere;
            return plan;
        }

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

            const schema::Column& col           = tbl.cols[*col_idx];
            bool                  is_counter    = col_is_counter(col);
            bool                  is_collection = col_is_collection(col);

            if (assign.target.access) {
                if (!is_collection) {
                    plan.result.error   = PlanError::InvalidSubscriptTarget;
                    plan.result.context = AutoString8("Element-based access is only supported on collection columns: ") + col_name;
                    return plan;
                }
                if (type_matches_tag<type::Set>(col.type.value)) {
                    plan.result.error   = PlanError::InvalidSubscriptTarget;
                    plan.result.context = AutoString8("Subscript-based assignment is not supported on sets: ") + col_name;
                    return plan;
                }
                if (type_matches_tag<SimpleSelection::FieldAccess>(*assign.target.access)) {
                    plan.result.error   = PlanError::InvalidSubscriptTarget;
                    plan.result.context = AutoString8("Field access on non-UDT column: ") + col_name;
                    return plan;
                }
                if (twi_has_column_ref(assign.value)) {
                    plan.result.error   = PlanError::InvalidCollectionMutation;
                    plan.result.context = AutoString8("Element-based assignment must not reference other columns: ") + col_name;
                    return plan;
                }
                const auto&     sub_term = get<SimpleSelection::Subscript>(*assign.target.access);
                CollectionPatch patch;
                patch.op    = CollectionPatch::Op::SubscriptSet;
                patch.key   = evaluate(sub_term.index, ctx);
                patch.value = evaluate(assign.value, ctx);
                if (type_matches_tag<Constant>(patch.key.value) && type_matches_tag<Unset>(get<Constant>(patch.key.value).value)) {
                    plan.result.error   = PlanError::UnsetSubscriptValue;
                    plan.result.context = AutoString8(col_name);
                    return plan;
                }
                if (type_matches_tag<Constant>(patch.value.value) && type_matches_tag<Unset>(get<Constant>(patch.value.value).value)) {
                    continue;
                }
                if (!col.is_static) {
                    has_non_static_assignment = true;
                }
                push_back(plan.spec.updates, ColumnUpdate{*col_idx, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{move(patch)}});
                continue;
            }

            if (auto cf = match_compound_form(assign.value, col_name); cf && !is_counter) {
                if (!is_collection) {
                    plan.result.error   = PlanError::CounterOperationOnNonCounter;
                    plan.result.context = AutoString8(col_name);
                    return plan;
                }
                bool            is_list = type_matches_tag<type::List>(col.type.value);
                bool            is_map  = type_matches_tag<type::Map>(col.type.value);
                CollectionPatch patch;
                if (cf->op == ArithmeticOperator::plus) {
                    // @note `X + s` / `X + m` is equivalent to append for set/map; only list distinguishes prepend.
                    if (cf->col_on_left || !is_list) {
                        patch.op = CollectionPatch::Op::Append;
                    } else {
                        patch.op = CollectionPatch::Op::Prepend;
                    }
                } else if (cf->op == ArithmeticOperator::minus) {
                    if (!cf->col_on_left) {
                        plan.result.error   = PlanError::InvalidCollectionMutation;
                        plan.result.context = AutoString8("Subtraction with a collection on the right is not supported: ") + col_name;
                        return plan;
                    }
                    patch.op = CollectionPatch::Op::Subtract;
                } else {
                    plan.result.error   = PlanError::InvalidCollectionMutation;
                    plan.result.context = AutoString8("Only + and - are valid compound operators on collections: ") + col_name;
                    return plan;
                }
                patch.value = evaluate(*cf->other, ctx);
                if (type_matches_tag<Constant>(patch.value.value) && type_matches_tag<Unset>(get<Constant>(patch.value.value).value)) {
                    continue;
                }
                // @note `Map - X` expects `set<map.key>` on the RHS; all other compound forms take the column's own type.
                bool rhs_ok = (is_map && patch.op == CollectionPatch::Op::Subtract)
                                ? io::can_cast_write_evaluated_as_column_value(patch.value, type::create_set(get<type::Map>(col.type.value).key))
                                : io::can_cast_write_evaluated_as_column_value(patch.value, col.type);
                if (!rhs_ok) {
                    plan.result.error   = PlanError::InvalidCollectionMutation;
                    plan.result.context = AutoString8("Incompatible right-hand side collection for ") + col_name;
                    return plan;
                }
                if (!col.is_static) {
                    has_non_static_assignment = true;
                }
                push_back(plan.spec.updates, ColumnUpdate{*col_idx, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{move(patch)}});
                continue;
            }

            bool has_col_ref = twi_has_column_ref(assign.value);
            if (has_col_ref && !is_counter) {
                plan.result.error   = PlanError::CounterOperationOnNonCounter;
                plan.result.context = AutoString8(col_name);
                return plan;
            }
            if (is_counter && !is_counter_increment_form(assign.value, col_name)) {
                plan.result.error   = PlanError::CounterAssignmentNotIncrement;
                plan.result.context = AutoString8(col_name);
                return plan;
            }
            if (!col.is_static) {
                has_non_static_assignment = true;
            }
            if (has_col_ref) {
                push_back(plan.spec.updates, ColumnUpdate{*col_idx, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{assign.value}});
            } else {
                push_back(plan.spec.updates, ColumnUpdate{*col_idx, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{evaluate(assign.value, ctx)}});
            }
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
                                push_back(plan.spec.updates, ColumnUpdate{pk_ci, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{evaluate(r.value, ctx)}});
                                break;
                            }
                        }
                        for (U64 ck_ci : tbl.clustering_key_col_indices) {
                            if (!tbl.cols[ck_ci].tombstone && tbl.cols[ck_ci].name == col_name) {
                                push_back(plan.spec.updates, ColumnUpdate{ck_ci, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{evaluate(r.value, ctx)}});
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

        if (where_has_unset_binding(stmt.where, ctx)) {
            plan.result.error = PlanError::UnsetValueInWhere;
            return plan;
        }

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

        bool ck_specified = plan.locator.ck.is_equality || plan.locator.ck.in_values.length > 0 || plan.locator.ck.has_in || plan.locator.ck.has_begin || plan.locator.ck.has_end;

        if (stmt.selections.length == 0) {
            plan.spec.is_full_delete = true;
        } else {
            bool is_static_only = true;
            for (const auto& sel : stmt.selections) {
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
                if (tbl.cols[*col_idx].key_kind != schema::KeyKind::None) {
                    plan.result.error   = PlanError::InvalidSubscriptTarget;
                    plan.result.context = AutoString8("Invalid deletion of primary key part ") + AutoString8(col_name);
                    return plan;
                }
                const schema::Column& col = tbl.cols[*col_idx];
                if (sel.access) {
                    if (!col_is_collection(col)) {
                        plan.result.error   = PlanError::InvalidSubscriptTarget;
                        plan.result.context = AutoString8("Element-based access is only supported on collection columns: ") + col_name;
                        return plan;
                    }
                    if (type_matches_tag<SimpleSelection::FieldAccess>(*sel.access)) {
                        plan.result.error   = PlanError::InvalidSubscriptTarget;
                        plan.result.context = AutoString8("Field access on non-UDT column: ") + col_name;
                        return plan;
                    }
                    const auto&     sub_term = get<SimpleSelection::Subscript>(*sel.access);
                    CollectionPatch patch;
                    patch.op  = CollectionPatch::Op::SubscriptDelete;
                    patch.key = evaluate(sub_term.index, ctx);
                    if (type_matches_tag<Constant>(patch.key.value) && type_matches_tag<Unset>(get<Constant>(patch.key.value).value)) {
                        // @note Cassandra rejects UNSET map key but silently no-ops UNSET list index.
                        if (type_matches_tag<type::List>(col.type.value)) {
                            continue;
                        }
                        plan.result.error   = PlanError::UnsetSubscriptValue;
                        plan.result.context = AutoString8(col_name);
                        return plan;
                    }
                    push_back(plan.spec.updates, ColumnUpdate{*col_idx, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{move(patch)}});
                    continue;
                }
                push_back(plan.spec.updates, ColumnUpdate{*col_idx, TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch>{Evaluated{Constant{Null{}}}}});
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

}
