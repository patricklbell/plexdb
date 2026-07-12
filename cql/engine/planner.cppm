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
    // Shared bound description for partition and clustering keys, generic over the BTree's
    // key representation: T = S64 (the Murmur3 token) for the partition BTree's
    // FixedKeyPolicy<S64>, T = DynamicArray<U8> (composite-encoded bytes) for the clustering
    // BTree's VarlenKeyPolicy. `is_partial` is meaningful only for composite clustering keys
    // where the bound covers a prefix shorter than the full CK column count.
    template<typename T>
    struct BoundsT {
        T    begin{};
        T    end{};
        bool has_begin        = false;
        bool has_end          = false;
        bool begin_inclusive  = true;
        bool end_inclusive    = true;
        bool is_equality      = false;
        bool begin_is_partial = false;
        bool end_is_partial   = false;
        bool has_in           = false;
        // Non-empty when WHERE uses IN; each entry is a fully serialized key.
        DynamicArray<T> in_values{};
    };
    using TokenBounds = BoundsT<S64>;              // Murmur3 token
    using KeyBounds   = BoundsT<DynamicArray<U8>>; // encoded bytes

    struct RowLocator {
        TokenBounds pk;
        KeyBounds   ck;

        // @note needed for mutations since token doesn't encode PK
        DynamicArray<Evaluated> pk_evals;

        bool reverse_clustering = false;

        // @note number of clustering positions, starting from 0, restricted by equality (or
        // single-value IN). ORDER BY may begin at the first non-equality CK position.
        U64 ck_eq_prefix_len = 0;

        // Set when a WHERE equality predicate matches an indexed non-key column.
        Optional<U64>    index_col_idx;    // column index in tbl.cols
        DynamicArray<U8> index_key_prefix; // encoded column value for index prefix scan
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
        CounterAssignmentNotIncrement,
        NullValueForCounter,
        DistinctRestrictionInvalid,
        InvalidCollectionMutation,
        InvalidSubscriptTarget,
        UnsetSubscriptValue,
        UnsetValueInWhere,
        InvalidTtlArgument,
        InvalidWritetimeArgument,
        // @note these errors set context to the full message
        ClusteringRestrictedAfterNonEq,
        ClusteringRestrictedWithoutPrefix,
    };

    struct FilterPlan {
        DynamicArray<WhereClause::Relation> predicates;
        bool                                needs_allow_filtering = false;

        // @note populated when the CK restriction chain is broken in a way Cassandra rejects
        PlanError   chain_violation         = PlanError::None;
        AutoString8 chain_violation_message = {};
    };

    struct SelectOp {
        struct ColumnRef {
            U64 col_idx;
        };
        struct CountStar {};
        // @note column must be a non-key, non-static regular column.
        struct TtlOf {
            U64 col_idx;
        };
        struct WritetimeOf {
            U64 col_idx;
        };
        struct Token {
            DynamicArray<U64> pk_col_indices;
        };
        // @note value is a column index or a pre-evaluated literal
        struct FuncCallArg {
            TaggedUnion<U64, Literal> value;
        };
        struct FuncCall {
            AutoString8               name; // lowercased
            DynamicArray<FuncCallArg> args;
            type::Basic               return_type;
        };
        struct Conversion {
            type::Basic from;
            type::Basic to;
        };
        TaggedUnion<ColumnRef, CountStar, TtlOf, WritetimeOf, Token, FuncCall> value;
        DynamicArray<Conversion>                                               conversions;
    };

    struct ProjectionPlan {
        DynamicArray<SelectOp> ops;
        // @note de-duplicated set of columns the executor must materialize per row
        DynamicArray<U64> needed_cols;
        bool              is_aggregate = false;
    };

    // @note `key` is the list index or map key for Subscript*; `value` is the
    // RHS collection for compound ops and the scalar element for SubscriptSet.
    struct CollectionPatch {
        enum class Op : U8 {
            Append,
            Prepend,
            Subtract,
            SubscriptSet,
            SubscriptDelete,
        };
        Op        op;
        Evaluated key{};
        Evaluated value{};
    };

    struct ColumnUpdate {
        U64                                                          col_idx;
        TaggedUnion<Evaluated, TermWithIdentifiers, CollectionPatch> new_value;
    };

    struct MutationSpec {
        DynamicArray<ColumnUpdate> updates;
        bool                       is_full_delete = false;
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

    bool table_has_counter(const schema::Table& tbl);
}
