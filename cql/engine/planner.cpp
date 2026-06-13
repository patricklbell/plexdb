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

        DynamicArray<Optional<Evaluated>>   ck_eq_vals;
        DynamicArray<WhereClause::Relation> ck_eq_rels;
        resize(ck_eq_vals, n_ck);

        for (const auto& rel : where.relations) {
            visit(rel.value, [&](const auto& r) {
                using T = RemoveCVRef<decltype(r)>;
                if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                    auto pk_pos = find_pk_position(tbl, r.column.identifier);
                    auto ck_pos = find_ck_position(tbl, r.column.identifier);

                    if (static_cast<bool>(pk_pos)) {
                        Evaluated eval = evaluate(r.value, ctx);
                        if (r.operator_ == Operator::eq) {
                            pk_eq_vals[*pk_pos] = eval;
                            push_back(pk_eq_rels, rel);
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
                            ck_eq_vals[*ck_pos] = eval;
                            push_back(ck_eq_rels, rel);
                        } else if (*ck_pos == 0) {
                            switch (r.operator_) {
                                case Operator::lt:
                                    locator.ck_end           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_end       = true;
                                    locator.ck_end_inclusive = false;
                                    break;
                                case Operator::le:
                                    locator.ck_end           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_end       = true;
                                    locator.ck_end_inclusive = true;
                                    break;
                                case Operator::gt:
                                    locator.ck_begin           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_begin       = true;
                                    locator.ck_begin_inclusive = false;
                                    break;
                                case Operator::ge:
                                    locator.ck_begin           = key::serialize_clustering_single(tbl, eval);
                                    locator.ck_has_begin       = true;
                                    locator.ck_begin_inclusive = true;
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
                    assert_not_implemented("tuple expression relations");
                } else if constexpr (SameAs<T, WhereClause::TokenRelation>) {
                    assert_not_implemented("token relations");
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
            for (const auto& r : pk_eq_rels) {
                push_back(filter.predicates, r);
                filter.needs_allow_filtering = true;
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
                for (const auto& r : ck_eq_rels) {
                    push_back(filter.predicates, r);
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

    MutationPlan plan_update(const Update& stmt, const schema::Table& tbl, const EvalContext& ctx) {
        MutationPlan plan{};

        {
            auto [loc, flt] = build_row_locator(stmt.where, tbl, ctx);
            plan.locator    = move(loc);
            plan.filter     = move(flt);
        }

        if (!plan.locator.pk_is_equality) {
            plan.result.error = PlanError::MissingPartitionKey;
            return plan;
        }

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

            // @todo counter columns: col = col + n (TermWithIdentifiers with column ref)
            if (!type_matches_tag<Constant>(assign.value.value) && !type_matches_tag<BindMarker>(assign.value.value)) {
                assert_not_implemented("non-constant/non-bind UPDATE assignment (counter columns)");
            }

            Term rhs_term;
            if (type_matches_tag<Constant>(assign.value.value)) {
                rhs_term.value = get<Constant>(assign.value.value);
            } else {
                rhs_term.value = get<BindMarker>(assign.value.value);
            }
            push_back(plan.spec.updates, ColumnUpdate{*col_idx, evaluate(move(rhs_term), ctx)});
        }

        return plan;
    }

    MutationPlan plan_delete(const Delete& stmt, const schema::Table& tbl, const EvalContext& ctx) {
        MutationPlan plan{};

        {
            auto [loc, flt] = build_row_locator(stmt.where, tbl, ctx);
            plan.locator    = move(loc);
            plan.filter     = move(flt);
        }

        if (!plan.locator.pk_is_equality) {
            plan.result.error = PlanError::MissingPartitionKey;
            return plan;
        }

        if (stmt.selections.length == 0) {
            plan.spec.is_full_delete = true;
        } else {
            for (const auto& sel : stmt.selections) {
                assert_true_not_implemented(!sel.access, "subscript/field access in DELETE is not implemented");

                Optional<U64> col_idx;
                String8       col_name(sel.column.identifier.c_str, sel.column.identifier.length);
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
                push_back(plan.spec.updates, ColumnUpdate{*col_idx, Evaluated{Constant{Null{}}}});
            }
        }

        return plan;
    }

} // namespace cql::planner
