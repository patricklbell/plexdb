export module cql.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import cql.engine.types;
import cql.engine.column_value;

using namespace plexdb;

export namespace cql {
    // ========================================================================
    // Forward declarations for Term
    // ========================================================================
    struct MapLiteral;
    struct SetLiteral;
    struct ListOrVectorLiteral;
    struct UdtLiteral;
    struct TupleLiteral;
    struct FunctionCall;
    struct ArithmeticOperation;
    struct TypeHint;
    struct TOIArithmeticOperation;

    // ========================================================================
    // constants
    // ========================================================================
    using LiteralTypes = TypeList<AutoString8, S64, bool, F64, Null, UUID, Hex, Blob, Duration, Unset>;
    struct Literal {
        ExpandTaggedUnion<LiteralTypes> value;
    };

    struct BindMarker {
        AutoString8 identifier;
    };

    // ========================================================================
    // Term
    // ========================================================================
    struct Term {
        HybridTaggedUnion<
            TypeList<Literal, BindMarker>,
            TypeList<
                MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral,
                FunctionCall, ArithmeticOperation, TypeHint, ColumnValue>>
            value;
    };

    struct TermWithIdentifiers {
        TermWithIdentifiers() = default;

        template<typename T>
            requires(!SameAs<Decay<T>, Term> && !SameAs<Decay<T>, TermWithIdentifiers>)
        explicit TermWithIdentifiers(T&& v)
            : value(forward<T>(v)) {
        }

        // this custom constructor unwraps a term -> twi, used to unify the parsing
        // @warn requires that type index needs to match term
        explicit TermWithIdentifiers(Term&& t) noexcept {
            value.index = t.value.index;
            if (t.value.index < decltype(t.value)::static_count) {
                visit(t.value, [&](auto& v) {
                    using T = Decay<decltype(v)>;
                    new (&value.storage) T(move(v));
                    v.~T();
                });
            } else {
                value.ptr   = t.value.ptr;
                t.value.ptr = nullptr;
            }
            t.value.index = decltype(t.value)::invalid_index;
        }

        HybridTaggedUnion<
            TypeList<Literal, BindMarker>,
            TypeList<
                MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral,
                FunctionCall, ArithmeticOperation, TypeHint,
                TOIArithmeticOperation, AutoString8>>
            value;
    };

    // ========================================================================
    // literals
    // ========================================================================
    struct MapLiteral {
        DynamicArray<Pair<Term, Term>> key_values;
    };
    struct SetLiteral {
        DynamicArray<Term> keys;
    };
    struct ListOrVectorLiteral {
        DynamicArray<Term> elements;
    };
    struct UdtLiteral {
        DynamicArray<Pair<AutoString8, Term>> identifier_values;
    };
    struct TupleLiteral {
        DynamicArray<Term> elements;
    };

    // ========================================================================
    // functions
    // ========================================================================
    struct FunctionCall {
        AutoString8        identifier;
        DynamicArray<Term> arguments;
    };

    // ========================================================================
    // arithmetic
    // ========================================================================
    enum ArithmeticOperator {
        plus,
        minus,
        times,
        divide,
        mod,
    };

    struct UnaryMinusArithmeticOperation {
        Term operand;
    };
    struct BinaryArithmeticOperation {
        Term               lhs;
        ArithmeticOperator op;
        Term               rhs;
    };
    struct ArithmeticOperation {
        TaggedUnion<UnaryMinusArithmeticOperation, BinaryArithmeticOperation> value;
    };

    // ========================================================================
    // type hinting
    // ========================================================================
    struct TypeHint {
        type::ast::Type type;
        Term            operand;
    };

    // ========================================================================
    // TOI types (TermWithIdentifiers variant types)
    // ========================================================================
    struct TOIUnaryMinus {
        TermWithIdentifiers operand;
    };
    struct TOIBinaryArithmetic {
        TermWithIdentifiers lhs;
        ArithmeticOperator  op;
        TermWithIdentifiers rhs;
    };
    struct TOIArithmeticOperation {
        TaggedUnion<TOIUnaryMinus, TOIBinaryArithmetic> value;
    };

    // ========================================================================
    // common
    // ========================================================================
    struct TableName {
        Optional<AutoString8> keyspace_name;
        AutoString8           table_name;
    };
    struct ColumnName {
        AutoString8 identifier;
    };

    using OptionKey   = AutoString8;
    using OptionValue = ExpandTaggedUnion<TypeList<AutoString8, Literal, MapLiteral>>;
    using OptionPair  = Pair<OptionKey, OptionValue>;
    struct Options {
        DynamicArray<OptionPair> identifier_values;
    };

    enum class Sort : U8 {
        ASC,
        DESC
    };

    struct ColumnMask {
        Optional<FunctionCall> mask_function;
    };

    struct UpdateParameter {
        enum class Kind {
            TIMESTAMP,
            TTL
        };
        Kind                         kind;
        TaggedUnion<S64, BindMarker> value;
    };

    enum class Operator {
        eq,
        lt,
        gt,
        le,
        ge,
        ne,
        in,
        contains,
        contains_key
    };

    struct SimpleSelection {
        struct Subscript {
            Term index;
        };
        struct FieldAccess {
            AutoString8 field;
        };
        ColumnName                                    column;
        Optional<TaggedUnion<Subscript, FieldAccess>> access;
    };

    struct Condition {
        SimpleSelection selection;
        Operator        op;
        Term            value;
    };

    struct IfExists {};
    struct IfConditions {
        DynamicArray<Condition> conditions;
    };
    using IfClause = TaggedUnion<IfExists, IfConditions>;

    struct WhereClause {
        struct ColumnExpressionRelation {
            ColumnName column;
            Operator   operator_;
            Term       value;
        };
        struct TupleExpressionRelation {
            DynamicArray<ColumnName> columns;
            Operator                 operator_;
            DynamicArray<Term>       values;
        };
        struct TokenRelation {
            DynamicArray<ColumnName> columns;
            Operator                 operator_;
            Term                     value;
        };
        struct SubscriptedRelation {
            ColumnName column;
            Term       subscript;
            Operator   operator_;
            Term       value;
        };
        struct Relation {
            TaggedUnion<ColumnExpressionRelation, TupleExpressionRelation, TokenRelation, SubscriptedRelation> value;
        };
        DynamicArray<Relation> relations;
    };

    inline bool is_inequality(Operator op) {
        return op == Operator::lt || op == Operator::le || op == Operator::gt || op == Operator::ge;
    }

    inline bool tuple_rhs_is_single_value(const WhereClause::TupleExpressionRelation& r) {
        return r.values.length == 1
            && r.columns.length > 1
            && type_matches_tag<TupleLiteral>(r.values[0].value)
            && get<TupleLiteral>(r.values[0].value).elements.length == r.columns.length;
    }

    inline bool tuple_rhs_is_compatible(const WhereClause::TupleExpressionRelation& r) {
        return (r.columns.length > 0 && r.columns.length == r.values.length) || tuple_rhs_is_single_value(r);
    }

    inline const Term& tuple_value_at(const WhereClause::TupleExpressionRelation& r, U64 i) {
        if (r.values.length == r.columns.length) {
            return r.values[i];
        }
        return get<TupleLiteral>(r.values[0].value).elements[i];
    }

    template<class Sel>
    inline auto* try_subscript_index_term(Sel& sel) {
        using R = decltype(&get<SimpleSelection::Subscript>(*sel.access).index);
        if (!sel.access || !type_matches_tag<SimpleSelection::Subscript>(*sel.access)) {
            return R{nullptr};
        }
        return &get<SimpleSelection::Subscript>(*sel.access).index;
    }

    struct ColumnDefinition {
        ColumnName           name;
        type::ast::Type      type;
        bool                 _static;
        Optional<ColumnMask> mask;
        bool                 primary_key;
    };

    // ========================================================================
    // Data definition (DDL)
    // ========================================================================
    struct CreateKeyspace {
        bool        if_not_exists;
        AutoString8 name;
        Options     options;
    };

    struct UseKeyspace {
        AutoString8 keyspace;
    };

    struct AlterKeyspace {
        bool        if_exists;
        AutoString8 keyspace;
        Options     options;
    };

    struct DropKeyspace {
        bool        if_exists;
        AutoString8 keyspace;
    };

    struct CreateTable {
        struct PartitionKey {
            TaggedUnion<ColumnName, DynamicArray<ColumnName>> column_or_columns;
        };

        using ClusteringColumns = DynamicArray<ColumnName>;
        struct PrimaryKey {
            PartitionKey      partition_key;
            ClusteringColumns clustering_columns;
        };

        struct CompactStorage {};
        struct ColumnOrder {
            ColumnName column;
            Sort       sort;
        };
        struct ClusteringOrder {
            DynamicArray<ColumnOrder> column_orders;
        };
        struct TableOptions {
            DynamicArray<ExpandTaggedUnion<TypeList<CompactStorage, ClusteringOrder, OptionPair>>> value;
        };

        bool                           if_not_exists;
        TableName                      name;
        DynamicArray<ColumnDefinition> column_definitions;
        Optional<PrimaryKey>           primary_key;
        TableOptions                   options;
    };

    struct AlterTable {
        struct AddColumnInstruction {
            bool                           if_not_exists;
            DynamicArray<ColumnDefinition> column_definitions; // @note static & primary_key are not allowed
        };

        struct DropColumnInstruction {
            bool                     if_exists;
            DynamicArray<ColumnName> columns;
        };

        struct RenameColumnInstruction {
            bool                                       if_exists;
            DynamicArray<Pair<ColumnName, ColumnName>> old_to_new_columns;
        };

        struct AlterColumnInstruction {
            bool                 if_exists;
            ColumnName           column;
            Optional<ColumnMask> column_mask;
        };

        bool                                                                                                               if_exists;
        TableName                                                                                                          table;
        TaggedUnion<AddColumnInstruction, DropColumnInstruction, RenameColumnInstruction, AlterColumnInstruction, Options> alter_table_instruction;
    };

    struct DropTable {
        bool      if_exists;
        TableName table;
    };

    struct TruncateTable {
        TableName table;
    };

    struct CreateIndex {
        bool                  custom;
        bool                  if_not_exists;
        Optional<AutoString8> index_name;
        TableName             table;
        AutoString8           column_name;
        // @note set when target uses `values(col)`, `keys(col)`, or `entries(col)`.
        Optional<AutoString8> selector;
    };

    struct DropIndex {
        bool      if_exists;
        TableName index_name; // table_name field holds the index name; keyspace_name is optional
    };

    struct CreateType {
        bool                           if_not_exists;
        TableName                      name;
        DynamicArray<ColumnDefinition> fields;
    };

    struct DropType {
        bool      if_exists;
        TableName name;
    };

    struct AlterType {
        struct AddFieldInstruction {
            DynamicArray<ColumnDefinition> fields;
        };
        struct RenameFieldInstruction {
            DynamicArray<Pair<ColumnName, ColumnName>> old_to_new_fields;
        };
        TableName                                                name;
        TaggedUnion<AddFieldInstruction, RenameFieldInstruction> instruction;
    };

    // ========================================================================
    // Data manipulation (DML)
    // ========================================================================
    struct Insert {
        struct NamesValues {
            DynamicArray<ColumnName> names;
            DynamicArray<Term>       values;
        };

        struct JsonClause {
            enum class Default {
                NUL,
                UNSET
            };
            AutoString8 string;
            Default     default_; // @note if not in query, defaults to UNSET
        };

        TableName                            table;
        TaggedUnion<NamesValues, JsonClause> insert_clause;
        bool                                 if_not_exists;
        DynamicArray<UpdateParameter>        using_parameters;
    };

    struct Update {
        struct Assignment {
            SimpleSelection     target;
            TermWithIdentifiers value;
        };

        TableName                     table;
        DynamicArray<UpdateParameter> using_parameters;
        DynamicArray<Assignment>      assignments;
        WhereClause                   where;
        Optional<IfClause>            if_;
    };

    struct Delete {
        DynamicArray<SimpleSelection> selections;
        TableName                     table;
        DynamicArray<UpdateParameter> using_parameters;
        WhereClause                   where;
        Optional<IfClause>            if_;
    };

    struct Batch {
        enum class Kind {
            LOGGED,
            UNLOGGED,
            COUNTER
        };

        struct ModificationStatement {
            TaggedUnion<Insert, Update, Delete> value;
        };

        Kind                                kind; // @note if not in query, set to LOGGED
        DynamicArray<UpdateParameter>       using_parameters;
        DynamicArray<ModificationStatement> statements;
    };

    // ========================================================================
    // Queries
    // ========================================================================
    struct Select {
        enum class Transform {
            UNIQUE,
            JSON,
        };

        struct Cast;
        struct Selector; // forward-declare for Function (DynamicArray<Selector> needs only a pointer)
        struct Function {
            AutoString8            function_name;
            DynamicArray<Selector> arguments;
        };
        struct Count {};
        // Static types (ColumnName, Term, Function, Count) are stored inline.
        // Cast is dynamic: it directly embeds Selector, so its size depends on Selector's size.
        struct Selector {
            HybridTaggedUnion<TypeList<ColumnName, Term, Function, Count>, TypeList<Cast>> value;
        };
        struct Cast {
            Selector        column;
            type::ast::Type to;
        };
        struct SelectColumn {
            Selector              column;
            Optional<AutoString8> as;
        };
        struct SelectClause {
            DynamicArray<SelectColumn> clauses;
        };

        struct GroupByClause {
            DynamicArray<ColumnName> columns;
        };

        struct ColumnOrderBy {
            Sort       sort;
            ColumnName column;
        };
        struct OrderByClause {
            DynamicArray<ColumnOrderBy> columns;
        };

        struct Limit {
            TaggedUnion<S64, BindMarker> value; // @note optional
        };
        struct PerPartitionLimit {
            TaggedUnion<S64, BindMarker> value; // @note optional
        };

        Optional<Transform>     transform;
        SelectClause            select;
        TableName               from;
        Optional<WhereClause>   where;
        Optional<GroupByClause> group_by;
        Optional<OrderByClause> order_by;
        PerPartitionLimit       per_partition_limit;
        Limit                   limit;
        bool                    allow_filtering;
    };

    // ========================================================================
    // statement
    // ========================================================================
    struct Statement {
        TaggedUnion<
            CreateKeyspace,
            UseKeyspace,
            AlterKeyspace,
            DropKeyspace,
            CreateTable,
            AlterTable,
            DropTable,
            TruncateTable,
            CreateIndex,
            DropIndex,
            CreateType,
            AlterType,
            DropType,
            Select,
            Insert,
            Update,
            Delete,
            Batch>
            value;
    };
}
