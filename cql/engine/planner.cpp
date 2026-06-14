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

    Pair<RowLocator, FilterPlan> build_row_locator(const WhereClause& where, const schema::Table& tbl, const EvalContext& ctx) {
        RowLocator locator{};
        FilterPlan filter{};

        U64 n_pk = tbl.partition_key_col_indices.length;
        U64 n_ck = tbl.clustering_key_col_indices.length;

        // @note Equality values are collected per position so compound keys are only
        // serialized when all positions are filled; partial matches fall to filter.
        DynamicArray<Optional<Evaluated>>   pk_eq_vals;
        DynamicArray<WhereClause::Relation> pk_eq_rels;
        resize(pk_eq_vals, n_pk);

        // IN values per PK position. pk_in_seen[i]=true even when IN list is empty.
        DynamicArray<DynamicArray<Evaluated>> pk_in_vals;
        DynamicArray<bool>                    pk_in_seen;
        resize(pk_in_vals, n_pk);
        resize(pk_in_seen, n_pk);
        for (U64 i = 0; i < n_pk; i++) pk_in_seen[i] = false;

        DynamicArray<Optional<Evaluated>>   ck_eq_vals;
        DynamicArray<WhereClause::Relation> ck_eq_rels;
        resize(ck_eq_vals, n_ck);

        DynamicArray<DynamicArray<Evaluated>> ck_in_vals;
        DynamicArray<bool>                    ck_in_seen;
        resize(ck_in_vals, n_ck);
        resize(ck_in_seen, n_ck);
        for (U64 i = 0; i < n_ck; i++) ck_in_seen[i] = false;

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
                                pk_eq_vals[*pk_pos] = eval;
                                push_back(pk_eq_rels, rel);
                            }
                        } else if (r.operator_ == Operator::in) {
                            // Collect IN values per PK position for multi-partition mutations.
                            // IN (a, b) parses as TupleLiteral; IN [a, b] parses as ListOrVectorLiteral.
                            pk_in_seen[*pk_pos] = true;
                            visit(eval.value, [&](const auto& lv) {
                                using LT = RemoveCVRef<decltype(lv)>;
                                if constexpr (SameAs<LT, ListOrVectorLiteral> || SameAs<LT, TupleLiteral>) {
                                    for (const Term& elem : lv.elements) {
                                        Evaluated ev = evaluate(elem, ctx);
                                        if (!is_null_eval(ev)) {
                                            push_back(pk_in_vals[*pk_pos], ev);
                                        }
                                    }
                                }
                            });
                            // Also add as filter predicate for SELECT (no ALLOW FILTERING needed for PK IN).
                            push_back(filter.predicates, rel);
                        } else if (n_pk == 1 && *pk_pos == 0) {
                            switch (r.operator_) {
                                case Operator::lt:
                                    locator.pk_end           = key::serialize_partition_single(tbl, eval);
                                    locator.pk_has_end       = true;
                                    locator.pk_end_inclusive = false;
                                    break;
                                case Operator::le:
                                    locator.pk_end           = key::serialize_partition_single(tbl, eval);
                                    locator.pk_has_end       = true;
                                    locator.pk_end_inclusive = true;
                                    break;
                                case Operator::gt:
                                    locator.pk_begin           = key::serialize_partition_single(tbl, eval);
                                    locator.pk_has_begin       = true;
                                    locator.pk_begin_inclusive = false;
                                    break;
                                case Operator::ge:
                                    locator.pk_begin           = key::serialize_partition_single(tbl, eval);
                                    locator.pk_has_begin       = true;
                                    locator.pk_begin_inclusive = true;
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
                                ck_eq_vals[*ck_pos] = eval;
                                push_back(ck_eq_rels, rel);
                            }
                        } else if (r.operator_ == Operator::in) {
                            ck_in_seen[*ck_pos] = true;
                            visit(eval.value, [&](const auto& lv) {
                                using LT = RemoveCVRef<decltype(lv)>;
                                if constexpr (SameAs<LT, ListOrVectorLiteral> || SameAs<LT, TupleLiteral>) {
                                    for (const Term& elem : lv.elements) {
                                        Evaluated ev = evaluate(elem, ctx);
                                        if (!is_null_eval(ev)) {
                                            push_back(ck_in_vals[*ck_pos], ev);
                                        }
                                    }
                                }
                            });
                            push_back(filter.predicates, rel);
                        } else if (*ck_pos == 0) {
                            bool partial = n_ck > 1;
                            switch (r.operator_) {
                                case Operator::lt:
                                    locator.ck_end           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_end       = true;
                                    locator.ck_end_inclusive = false;
                                    locator.ck_end_is_partial   = partial;
                                    break;
                                case Operator::le:
                                    locator.ck_end           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_end       = true;
                                    locator.ck_end_inclusive = true;
                                    locator.ck_end_is_partial   = partial;
                                    break;
                                case Operator::gt:
                                    locator.ck_begin           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_begin       = true;
                                    locator.ck_begin_inclusive = false;
                                    locator.ck_begin_is_partial = partial;
                                    break;
                                case Operator::ge:
                                    locator.ck_begin           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_begin       = true;
                                    locator.ck_begin_inclusive = true;
                                    locator.ck_begin_is_partial = partial;
                                    break;
                                default:
                                    break;
                            }
                            // @note CK range also goes to filter for post-scan evaluation;
                            // does not set needs_allow_filtering — CK filtering is efficient.
                            push_back(filter.predicates, rel);
                        } else {
                            push_back(filter.predicates, rel);
                        }
                    } else {
                        push_back(filter.predicates, rel);
                        filter.needs_allow_filtering = true;
                    }
                } else if constexpr (SameAs<T, WhereClause::TupleExpressionRelation>) {
                    if (r.operator_ == Operator::in && r.columns.length > 0) {
                        // (ck1 [, ck2]) IN ((v0a [, v0b]), (v1a [, v1b]))
                        // Only generate ck_in_vals when every column in the tuple is a CK.
                        bool all_ck_in = true;
                        for (U64 i = 0; i < r.columns.length && all_ck_in; i++) {
                            all_ck_in = static_cast<bool>(find_ck_position(tbl, r.columns[i].identifier));
                        }
                        if (all_ck_in) {
                            for (U64 ci = 0; ci < r.columns.length; ci++) {
                                auto ck_pos = find_ck_position(tbl, r.columns[ci].identifier);
                                if (ck_pos) ck_in_seen[*ck_pos] = true;
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
                                // Single-column: feed ck_in_vals for cartesian product.
                                for (const Term& val_term : r.values) {
                                    visit(val_term.value, [&](const auto& v) {
                                        using VT = RemoveCVRef<decltype(v)>;
                                        if constexpr (SameAs<VT, TupleLiteral>) {
                                            auto ck_pos = find_ck_position(tbl, r.columns[0].identifier);
                                            if (ck_pos && v.elements.length > 0) {
                                                push_back(ck_in_vals[*ck_pos], evaluate(v.elements[0], ctx));
                                            }
                                        } else {
                                            auto ck_pos = find_ck_position(tbl, r.columns[0].identifier);
                                            if (ck_pos) {
                                                push_back(ck_in_vals[*ck_pos], evaluate(val_term, ctx));
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
                                pk_eq_vals[*pk_pos] = evaluate(r.values[i], ctx);
                            }
                            push_back(pk_eq_rels, rel);
                        } else if (all_ck) {
                            for (U64 i = 0; i < r.columns.length; i++) {
                                auto ck_pos         = find_ck_position(tbl, r.columns[i].identifier);
                                ck_eq_vals[*ck_pos] = evaluate(r.values[i], ctx);
                            }
                            push_back(ck_eq_rels, rel);
                        } else {
                            push_back(filter.predicates, rel);
                            filter.needs_allow_filtering = true;
                        }
                    } else {
                        // IN or non-eq tuple relation: push to filter for post-scan evaluation.
                        push_back(filter.predicates, rel);
                    }
                } else if constexpr (SameAs<T, WhereClause::TokenRelation>) {
                    push_back(filter.predicates, rel);
                } else {
                    static_assert(!SameAs<T, T>, "missing WHERE clause type");
                }
            });
        }

        bool all_pk_eq = n_pk > 0;
        for (U64 i = 0; i < n_pk; i++) {
            if (!pk_eq_vals[i]) {
                all_pk_eq = false;
                break;
            }
        }

        if (all_pk_eq) {
            DynamicArray<Evaluated> evals;
            for (U64 i = 0; i < n_pk; i++) {
                push_back(evals, move(*pk_eq_vals[i]));
            }
            locator.pk_begin           = key::serialize_partition(tbl, evals);
            locator.pk_has_begin       = true;
            locator.pk_begin_inclusive = true;
            locator.pk_is_equality     = true;
        } else {
            // Check if all PK positions have either EQ or IN (even empty IN).
            // If so, generate a cartesian product of pk_in_values for multi-partition support.
            bool all_pk_covered = true;
            for (U64 i = 0; i < n_pk; i++) {
                if (!pk_eq_vals[i] && !pk_in_seen[i]) {
                    all_pk_covered = false;
                    break;
                }
            }
            if (all_pk_covered) {
                locator.pk_has_in = true;
                DynamicArray<U64> sizes;
                resize(sizes, n_pk);
                U64 total = 1;
                for (U64 i = 0; i < n_pk; i++) {
                    sizes[i] = pk_eq_vals[i] ? 1 : pk_in_vals[i].length;
                    total *= sizes[i];
                }
                for (U64 idx = 0; idx < total; idx++) {
                    DynamicArray<Evaluated> combo;
                    U64                     stride = total;
                    for (U64 i = 0; i < n_pk; i++) {
                        stride /= sizes[i];
                        U64 choice = (idx / stride) % sizes[i];
                        push_back(combo, pk_eq_vals[i] ? *pk_eq_vals[i] : pk_in_vals[i][choice]);
                    }
                    push_back(locator.pk_in_values, key::serialize_partition(tbl, combo));
                }
                // Push EQ parts to filter so they are evaluated row-by-row in SELECT.
                for (const auto& r : pk_eq_rels) {
                    push_back(filter.predicates, r);
                }
            } else {
                // If any explicitly-seen IN list is empty, the cartesian product is 0
                // (a no-op), so mark pk_has_in without requiring all columns covered.
                bool any_pk_empty_in = false;
                for (U64 i = 0; i < n_pk; i++) {
                    if (pk_in_seen[i] && pk_in_vals[i].length == 0) {
                        any_pk_empty_in = true;
                        break;
                    }
                }
                if (any_pk_empty_in) {
                    locator.pk_has_in = true;
                } else {
                    for (const auto& r : pk_eq_rels) {
                        push_back(filter.predicates, r);
                        filter.needs_allow_filtering = true;
                    }
                }
            }
        }

        if (n_ck > 0) {
            bool all_ck_eq = ck_eq_rels.length > 0;
            for (U64 i = 0; i < n_ck; i++) {
                if (!ck_eq_vals[i]) {
                    all_ck_eq = false;
                    break;
                }
            }

            if (all_ck_eq) {
                DynamicArray<Evaluated> evals;
                for (U64 i = 0; i < n_ck; i++) {
                    push_back(evals, move(*ck_eq_vals[i]));
                }
                locator.ck_begin           = key::serialize_clustering(tbl, evals);
                locator.ck_has_begin       = true;
                locator.ck_begin_inclusive = true;
                locator.ck_is_equality     = true;
                // @note CK equality also goes to filter for post-scan evaluation;
                // the locator's ck_begin is used for initial positioning only.
                for (const auto& r : ck_eq_rels) {
                    push_back(filter.predicates, r);
                }
            } else {
                // Generate cartesian product of EQ and IN values per CK position.
                // ck_in_seen[i]=true counts as "covered" even when the IN list is empty.
                bool all_ck_covered = true;
                for (U64 i = 0; i < n_ck; i++) {
                    if (!ck_eq_vals[i] && !ck_in_seen[i]) {
                        all_ck_covered = false;
                        break;
                    }
                }
                if (all_ck_covered) {
                    locator.ck_has_in = true;
                    DynamicArray<U64> sizes;
                    resize(sizes, n_ck);
                    U64 total = 1;
                    for (U64 i = 0; i < n_ck; i++) {
                        sizes[i] = ck_eq_vals[i] ? 1 : ck_in_vals[i].length;
                        total *= sizes[i];
                    }
                    for (U64 idx = 0; idx < total; idx++) {
                        DynamicArray<Evaluated> combo;
                        U64                     stride = total;
                        for (U64 i = 0; i < n_ck; i++) {
                            stride /= sizes[i];
                            U64 choice = (idx / stride) % sizes[i];
                            push_back(combo, ck_eq_vals[i] ? *ck_eq_vals[i] : ck_in_vals[i][choice]);
                        }
                        push_back(locator.ck_in_values, key::serialize_clustering(tbl, combo));
                    }
                } else {
                    // If any explicitly-seen IN list is empty, the product is 0 — no-op.
                    for (U64 i = 0; i < n_ck; i++) {
                        if (ck_in_seen[i] && ck_in_vals[i].length == 0) {
                            locator.ck_has_in = true;
                            break;
                        }
                    }
                }
                for (const auto& r : ck_eq_rels) {
                    push_back(filter.predicates, r);
                }
            }

            // Process multi-column tuple IN combinations (from TupleExpressionRelation with > 1 column).
            // Each entry is a pre-paired set of CK values; fill missing positions from ck_eq_vals.
            for (const auto& combo : ck_tuple_in_combos) {
                bool                    all_covered = true;
                DynamicArray<Evaluated> full_combo;
                for (U64 i = 0; i < n_ck; i++) {
                    if (combo[i]) {
                        push_back(full_combo, *combo[i]);
                    } else if (ck_eq_vals[i]) {
                        push_back(full_combo, *ck_eq_vals[i]);
                    } else {
                        all_covered = false;
                        break;
                    }
                }
                if (all_covered) {
                    push_back(locator.ck_in_values, key::serialize_clustering(tbl, full_combo));
                }
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

        if (!stmt.allow_filtering && plan.filter.needs_allow_filtering) {
            plan.result.error = PlanError::RequiresAllowFiltering;
            return plan;
        }

        if (stmt.order_by) {
            for (const auto& col_order : stmt.order_by->columns) {
                String8 name(col_order.column.identifier.c_str, col_order.column.identifier.length);
                bool    is_ck = false;
                for (U64 ck_ci : tbl.clustering_key_col_indices) {
                    if (tbl.cols[ck_ci].name == name) {
                        is_ck = true;
                        break;
                    }
                }
                if (!is_ck) {
                    plan.result.error   = PlanError::OrderByOnNonClusteringColumn;
                    plan.result.context = AutoString8(name);
                    return plan;
                }
                assert_not_implemented("ORDER BY on clustering key requires reverse iterator");
            }
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
                    case Operator::lt:           op_name = AutoString8("LT"); break;
                    case Operator::gt:           op_name = AutoString8("GT"); break;
                    case Operator::le:           op_name = AutoString8("LTE"); break;
                    case Operator::ge:           op_name = AutoString8("GTE"); break;
                    case Operator::ne:           op_name = AutoString8("NEQ"); break;
                    case Operator::contains:     op_name = AutoString8("CONTAINS"); break;
                    case Operator::contains_key: op_name = AutoString8("CONTAINS KEY"); break;
                    default:                     op_name = AutoString8("UNKNOWN"); break;
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

        if (!plan.locator.pk_is_equality && plan.locator.pk_in_values.length == 0 && !plan.locator.pk_has_in) {
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
            bool ck_specified = plan.locator.ck_is_equality || plan.locator.ck_in_values.length > 0 || plan.locator.ck_has_in;
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

        if (!plan.locator.pk_is_equality && plan.locator.pk_in_values.length == 0 && !plan.locator.pk_has_in) {
            plan.result.error   = PlanError::MissingPartitionKey;
            plan.result.context = AutoString8(tbl.cols[tbl.partition_key_col_indices[0]].name);
            return plan;
        }

        bool ck_specified = plan.locator.ck_is_equality || plan.locator.ck_in_values.length > 0 ||
                            plan.locator.ck_has_in || plan.locator.ck_has_begin || plan.locator.ck_has_end;

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
                        if (!tbl.cols[ci].is_static) is_static_only = false;
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
        }

        // Skip CK check if the PK IN list is empty — the mutation is guaranteed to be a no-op.
        bool pk_will_be_empty = plan.locator.pk_has_in && plan.locator.pk_in_values.length == 0;
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
