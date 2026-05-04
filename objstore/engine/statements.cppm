export module objstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.tagged_union;
import plexdb.os.dynamic_tagged_union;

import objstore.engine.types;

using namespace plexdb;

export namespace objstore {
    // ========================================================================
    // term
    // ========================================================================
    struct Constant;
    struct MapLiteral;
    struct SetLiteral;
    struct ListOrVectorLiteral;
    struct UdtLiteral;
    struct TupleLiteral;
    struct FunctionCall;
    struct ArithmeticOperation;
    struct TypeHint;
    struct BindMarker;
    struct Term {
        // @note @warn DO NOT modify without also checking TermWithIdentifier
        ExpandDynamicTaggedUnion<
            TypeList<
                Constant,
                MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral,
                FunctionCall,
                ArithmeticOperation,
                TypeHint,
                BindMarker
            >
        > value;
    };

    // constants
    struct Null {};
    struct UUID {
        static constexpr U64 length = 16_u64;
        Array<U8, length> value;

        bool operator==(const UUID& o) const {
            for (U64 i = 0; i < length; i++)
                if (value[i] != o.value[i]) return false;
            return true;
        }
    };
    struct Hex {
        DynamicArray<U8> value;

        bool operator==(const Hex& o) const {
            if (value.length != o.value.length) return false;
            for (U64 i = 0; i < value.length; i++)
                if (value[i] != o.value[i]) return false;
            return true;
        }
    };
    struct Blob {
        DynamicArray<U8> value;

        bool operator==(const Blob& o) const {
            if (value.length != o.value.length) return false;
            for (U64 i = 0; i < value.length; i++)
                if (value[i] != o.value[i]) return false;
            return true;
        }
    };
    using ConstantTypes = TypeList<AutoString8, S64, bool, F64, Null, UUID, Hex, Blob>;
    struct Constant {
        ExpandTaggedUnion<ConstantTypes> value;
    };

    // literals
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

    // functions
    struct FunctionCall {
        AutoString8 identifier;
        DynamicArray<Term> arguments;
    };

    // arithmetics
    enum ArithmeticOperator {
        plus,
        minus,
        times,
        divide,
    };
    struct UnaryMinusArithmeticOperation {
        Term operand;
    };
    struct BinaryArithmeticOperation {
        Term lhs;
        ArithmeticOperator op;
        Term rhs;
    };
    struct ArithmeticOperation {
        TaggedUnion<UnaryMinusArithmeticOperation, BinaryArithmeticOperation> value;
    };

    // type hinting
    struct TypeHint {
        Type type;
        Term operand;
    };

    // bindings
    struct BindMarker {
        AutoString8 identifier;
    };

    // ========================================================================
    // common
    // ========================================================================
    struct TableName {
        Optional<AutoString8> keyspace_name;
        AutoString8 table_name;
    };
    struct ColumnName {
        AutoString8 identifier;
    };

    using OptionKey = AutoString8;
    using OptionValue = ExpandTaggedUnion<TypeList<AutoString8, Constant, MapLiteral>>;
    using OptionPair = Pair<OptionKey, OptionValue>;
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
        enum class Kind { TIMESTAMP, TTL };
        Kind kind;
        TaggedUnion<S64, BindMarker> value;
    };

    enum class Operator { eq, lt, gt, le, ge, ne, in, contains, contains_key };

    struct SimpleSelection {
        struct Subscript { Term index; };
        struct FieldAccess { AutoString8 field; };
        ColumnName column;
        Optional<TaggedUnion<Subscript, FieldAccess>> access;
    };

    struct Condition {
        SimpleSelection selection;
        Operator op;
        Term value;
    };

    struct IfExists {};
    struct IfConditions {
        DynamicArray<Condition> conditions;
    };
    using IfClause = TaggedUnion<IfExists, IfConditions>;

    struct WhereClause {
        struct ColumnExpressionRelation {
            ColumnName column;
            Operator operator_;
            Term value;
        };
        struct TupleExpressionRelation {
            DynamicArray<ColumnName> columns;
            Operator operator_;
            DynamicArray<Term> values;
        };
        struct TokenRelation {
            DynamicArray<ColumnName> columns;
            Operator operator_;
            Term value;
        };
        struct Relation {
            TaggedUnion<ColumnExpressionRelation, TupleExpressionRelation, TokenRelation> value;
        };
        DynamicArray<Relation> relations;
    };

    struct ColumnDefinition {
        ColumnName name;
        Type type;
        bool _static;
        Optional<ColumnMask> mask;
        bool primary_key;
    };

    // ========================================================================
    // Data definition (DDL)
    // ========================================================================
    struct CreateKeyspace {
        bool if_not_exists;
        AutoString8 name;
        Options options;
    };

    struct UseKeyspace {
        AutoString8 keyspace;
    };

    struct AlterKeyspace {
        bool if_exists;
        AutoString8 keyspace;
        Options options;
    };

    struct DropKeyspace {
        bool if_exists;
        AutoString8 keyspace;
    };

    struct CreateTable {
        struct PartitionKey {
            TaggedUnion<ColumnName, DynamicArray<ColumnName>> column_or_columns;
        };

        using ClusteringColumns = DynamicArray<ColumnName>;
        struct PrimaryKey {
            PartitionKey partition_key;
            ClusteringColumns clustering_columns;
        };

        struct CompactStorage {};
        struct ColumnOrder {
            ColumnName column;
            Sort sort;
        };
        struct ClusteringOrder {
            DynamicArray<ColumnOrder> column_orders;
        };
        struct TableOptions {
            DynamicArray<ExpandTaggedUnion<TypeList<CompactStorage, ClusteringOrder, OptionPair>>> value;
        };

        bool if_not_exists;
        TableName name;
        DynamicArray<ColumnDefinition> column_definitions;
        Optional<PrimaryKey> primary_key;
        TableOptions options;
    };

    struct AlterTable {
        struct AddColumnInstruction {
            bool if_not_exists;
            DynamicArray<ColumnDefinition> column_definitions; // @note static & primary_key are not allowed
        };

        struct DropColumnInstruction {
            bool if_exists;
            DynamicArray<ColumnName> columns;
        };

        struct RenameColumnInstruction {
            bool if_exists;
            DynamicArray<Pair<ColumnName, ColumnName>> old_to_new_columns;
        };

        struct AlterColumnInstruction {
            bool if_exists;
            ColumnName column;
            Optional<ColumnMask> column_mask;
        };

        bool if_exists;
        TableName table;
        TaggedUnion<AddColumnInstruction, DropColumnInstruction, RenameColumnInstruction, AlterColumnInstruction, Options> alter_table_instruction;
    };

    struct DropTable {
        bool if_exists;
        TableName table;
    };

    struct TruncateTable {
        TableName table;
    };

    // ========================================================================
    // Data manipulation (DML)
    // ========================================================================
    struct Insert {
        struct NamesValues {
            DynamicArray<ColumnName> names;
            DynamicArray<Term> values;
        };

        struct JsonClause {
            enum class Default { NUL, UNSET };
            AutoString8 string;
            Default default_; // @note if not in query, defaults to UNSET
        };

        TableName table;
        TaggedUnion<NamesValues, JsonClause> insert_clause;
        bool if_not_exists;
        DynamicArray<UpdateParameter> using_parameters;
    };

    struct TOIArithmeticOperation;
    struct TermWithIdentifiers {
        ExpandDynamicTaggedUnion<
            // @note @warn order MUST match Term to allow direct pointer moving
            TypeList<
                Constant,
                MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral,
                FunctionCall,
                ArithmeticOperation,
                TypeHint,
                BindMarker,
                TOIArithmeticOperation,
                AutoString8
            >
        > value;
    };

    struct TOIUnaryMinus {
        TermWithIdentifiers operand;
    };
    struct TOIBinaryArithmetic {
        TermWithIdentifiers lhs;
        ArithmeticOperator op;
        TermWithIdentifiers rhs;
    };
    struct TOIArithmeticOperation {
        TaggedUnion<TOIUnaryMinus, TOIBinaryArithmetic> value;
    };

    struct Update {
        struct Assignment {
            SimpleSelection target;
            TermWithIdentifiers value;
        };

        TableName table;
        DynamicArray<UpdateParameter> using_parameters;
        DynamicArray<Assignment> assignments;
        WhereClause where;
        Optional<IfClause> if_;
    };

    struct Delete {
        DynamicArray<SimpleSelection> selections;
        TableName table;
        DynamicArray<UpdateParameter> using_parameters;
        WhereClause where;
        Optional<IfClause> if_;
    };

    struct Batch {
        enum class Kind { LOGGED, UNLOGGED, COUNTER };

        struct ModificationStatement {
            TaggedUnion<Insert, Update, Delete> value;
        };

        Kind kind; // @note if not in query, set to LOGGED
        DynamicArray<UpdateParameter> using_parameters;
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
        struct Function;
        struct Count {};
        // @note dynamic to allow recursive definition
        struct Selector {
            DynamicTaggedUnion<ColumnName, Term, Cast, Function, Count> value;
        };
        struct Cast {
            Selector column;
            Type to;
        };
        struct Function {
            AutoString8 function_name;
            DynamicArray<Selector> arguments;
        };
        struct SelectColumn {
            Selector column;
            Optional<AutoString8> as;
        };
        struct SelectClause {
            DynamicArray<SelectColumn> clauses;
        };

        struct GroupByClause {
            DynamicArray<ColumnName> columns;
        };

        struct ColumnOrderBy {
            Sort sort;
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

        Optional<Transform> transform;
        SelectClause select;
        TableName from;
        Optional<WhereClause> where;
        Optional<GroupByClause> group_by;
        Optional<OrderByClause> order_by;
        PerPartitionLimit per_partition_limit;
        Limit limit;
        bool allow_filtering;
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
            Select,
            Insert,
            Update,
            Delete,
            Batch
        > value;
    };
}

export namespace objstore {
    inline U64 hash(const UUID& uuid) {
        return plexdb::hash(plexdb::String8(&uuid.value[0], uuid.length));
    }

    inline U64 hash(const Blob& blob) {
        return plexdb::hash(plexdb::String8(blob.value.ptr, blob.value.length));
    }

    inline U64 hash(const Hex& hex) {
        return plexdb::hash(plexdb::String8(hex.value.ptr, hex.value.length));
    }
}
