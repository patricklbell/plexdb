export module cql.engine.planner;

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

export namespace cql::planner {
    struct RowLocator {
        DynamicArray<U8> pk_begin{};
        DynamicArray<U8> pk_end{};
        bool             pk_has_begin       = false;
        bool             pk_has_end         = false;
        bool             pk_begin_inclusive = true;
        bool             pk_end_inclusive   = true;
        bool             pk_is_equality     = false;

        // Non-empty when WHERE uses IN on the partition key.
        // Each entry is a fully serialized partition key to mutate.
        DynamicArray<DynamicArray<U8>> pk_in_values{};

        // Non-empty when WHERE uses IN on the clustering key.
        // Each entry is a fully serialized clustering key to mutate.
        DynamicArray<DynamicArray<U8>> ck_in_values{};

        DynamicArray<U8> ck_begin{};
        DynamicArray<U8> ck_end{};
        bool             ck_has_begin       = false;
        bool             ck_has_end         = false;
        bool             ck_begin_inclusive = true;
        bool             ck_end_inclusive   = true;
        bool             ck_is_equality     = false;

        bool pk_has_in = false;
        bool ck_has_in = false;

        // @note set when bound covers only first CK column of a multi-column key; composite-encoded, requires prefix comparison
        bool ck_begin_is_partial = false;
        bool ck_end_is_partial   = false;

        bool reverse_partitions = false;
        bool reverse_clustering = false;

        // @note number of clustering positions, starting from 0, restricted by equality (or
        // single-value IN). ORDER BY may begin at the first non-equality CK position.
        U64 ck_eq_prefix_len = 0;

        // Set when a WHERE equality predicate matches an indexed non-key column.
        Optional<U64>    index_col_idx;    // column index in tbl.cols
        DynamicArray<U8> index_key_prefix; // encoded column value for index prefix scan
    };

    struct FilterPlan {
        DynamicArray<WhereClause::Relation> predicates;
        bool                                needs_allow_filtering = false;
    };

    struct SelectOp {
        struct ColumnRef {
            U64 col_idx;
        };
        struct CountStar {};
        TaggedUnion<ColumnRef, CountStar> value;
    };

    struct ProjectionPlan {
        DynamicArray<SelectOp> ops;
        bool                   is_aggregate = false;
    };

    // ── Mutation spec (UPDATE / DELETE / INSERT) ──────────────────────────────
    struct ColumnUpdate {
        U64       col_idx;
        Evaluated new_value; // Null means clear this column
    };

    struct MutationSpec {
        DynamicArray<ColumnUpdate> updates;
        bool                       is_full_delete = false;
    };

    enum class PlanError : U8 {
        None,
        RequiresAllowFiltering,
        MissingPartitionKey,
        MissingClusteringKey,
        OrderByOnNonClusteringColumn,
        ColumnNotFound,
        TypeMismatch,
        StaticOnlyUpdateWithCK,
        StaticOnlyDeleteWithCK,
        RangeDeletionOnSpecificColumns,
        TokenFunctionInMutation,
        DuplicateColumnInMutation,
        NonKeyColumnInMutationWhere,
        NonEqInOnPartitionKeyMutation,
        CounterOperationOnNonCounter,
        DistinctRestrictionInvalid,
    };

    struct PlanResult {
        PlanError   error   = PlanError::None;
        AutoString8 context = {};
    };

    struct SelectPlan {
        RowLocator     locator;
        FilterPlan     filter;
        ProjectionPlan projection;
        PlanResult     result;
    };

    struct MutationPlan {
        RowLocator   locator;
        FilterPlan   filter;
        MutationSpec spec;
        PlanResult   result;
    };

    // ── Planner functions ─────────────────────────────────────────────────────
    // @note CK predicates go to filter.predicates but never set needs_allow_filtering.
    // Non-key predicates go to filter.predicates and set needs_allow_filtering.
    Pair<RowLocator, FilterPlan> build_row_locator(const WhereClause& where, const schema::Table& tbl, const EvalContext& ctx);

    SelectPlan   plan_select(const Select& stmt, const schema::Table& tbl, const EvalContext& ctx);
    MutationPlan plan_update(const Update& stmt, const schema::Table& tbl, const EvalContext& ctx);
    MutationPlan plan_delete(const Delete& stmt, const schema::Table& tbl, const EvalContext& ctx);
}
