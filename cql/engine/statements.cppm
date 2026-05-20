export module cql.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import cql.engine.types;

using namespace plexdb;

export namespace cql {
    // ========================================================================
    // Forward declarations (needed by literal/arithmetic types)
    // ========================================================================
    struct Term;
    struct TermWithIdentifiers;

    // ========================================================================
    // Box<T> — owns a heap-allocated T*
    // Method bodies are defined after T is complete (as explicit specialisations).
    // ========================================================================
    template<typename T>
    struct Box {
        T* ptr = nullptr;

        Box() = default;

        explicit Box(T&& val);
        Box(const Box& o);
        Box(Box&& o) noexcept;
        Box& operator=(const Box& o);
        Box& operator=(Box&& o) noexcept;
        ~Box();

        T& operator*() { return *ptr; }
        const T& operator*() const { return *ptr; }
        T* operator->() { return ptr; }
        const T* operator->() const { return ptr; }
    };

    // ========================================================================
    // constants
    // ========================================================================
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

    struct Inet {
        bool is_v6;
        union {
            Array<U8, 4>  v4;
            Array<U8, 16> v6;
        };
        bool operator==(const Inet& o) const {
            if (is_v6 != o.is_v6) return false;
            if (is_v6) {
                for (int i = 0; i < 16; i++) if (v6[i] != o.v6[i]) return false;
            } else {
                for (int i = 0; i < 4;  i++) if (v4[i] != o.v4[i]) return false;
            }
            return true;
        }
    };

    struct VarInt {
        bool negative = false;
        DynamicArray<U8> magnitude;  // big-endian bytes
        bool operator==(const VarInt& o) const {
            if (negative != o.negative || magnitude.length != o.magnitude.length) return false;
            for (U64 i = 0; i < magnitude.length; i++) if (magnitude[i] != o.magnitude[i]) return false;
            return true;
        }
    };

    struct Decimal {
        S32 scale = 0;
        VarInt unscaled;
        bool operator==(const Decimal& o) const {
            return scale == o.scale && unscaled == o.unscaled;
        }
    };

    struct Duration {
        S32 months = 0;
        S32 days   = 0;
        S64 nanoseconds = 0;
        bool operator==(const Duration& o) const {
            return months == o.months && days == o.days && nanoseconds == o.nanoseconds;
        }
    };

    using ConstantTypes = TypeList<AutoString8, S64, bool, F64, Null, UUID, Hex, Blob>;
    struct Constant {
        ExpandTaggedUnion<ConstantTypes> value;
    };

    // ========================================================================
    // literals (use DynamicArray<Term> - ok with forward-declared Term since
    // DynamicArray only stores T* internally)
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
        AutoString8 identifier;
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
    };

    struct UnaryMinusArithmeticOperation {
        Box<Term> operand;
        explicit UnaryMinusArithmeticOperation(Term&& t);
        UnaryMinusArithmeticOperation() = default;
    };
    struct BinaryArithmeticOperation {
        Box<Term> lhs;
        ArithmeticOperator op;
        Box<Term> rhs;
        BinaryArithmeticOperation(Term&& l, ArithmeticOperator o, Term&& r);
        BinaryArithmeticOperation() = default;
    };
    struct ArithmeticOperation {
        TaggedUnion<UnaryMinusArithmeticOperation, BinaryArithmeticOperation> value;
    };

    // ========================================================================
    // type hinting
    // ========================================================================
    struct TypeHint {
        Type type;
        Box<Term> operand;
        TypeHint(Type t, Term&& o);
        TypeHint() = default;
    };

    // ========================================================================
    // bindings
    // ========================================================================
    struct BindMarker {
        AutoString8 identifier;
    };

    // ========================================================================
    // Term — defined after all variant types are complete
    // ========================================================================
    struct Term {
        ExpandTaggedUnion<
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

    // ========================================================================
    // Box<Term> method bodies (now that Term is complete)
    // ========================================================================
    template<> inline Box<Term>::Box(Term&& val) {
        ptr = reinterpret_cast<Term*>(os::allocate(sizeof(Term)));
        new (ptr) Term(move(val));
    }
    template<> inline Box<Term>::Box(const Box<Term>& o) {
        if (o.ptr) {
            ptr = reinterpret_cast<Term*>(os::allocate(sizeof(Term)));
            new (ptr) Term(*o.ptr);
        }
    }
    template<> inline Box<Term>::Box(Box<Term>&& o) noexcept {
        ptr = o.ptr;
        o.ptr = nullptr;
    }
    template<> inline Box<Term>& Box<Term>::operator=(const Box<Term>& o) {
        if (this == &o) return *this;
        if (ptr) { ptr->~Term(); os::deallocate(ptr); ptr = nullptr; }
        if (o.ptr) {
            ptr = reinterpret_cast<Term*>(os::allocate(sizeof(Term)));
            new (ptr) Term(*o.ptr);
        }
        return *this;
    }
    template<> inline Box<Term>& Box<Term>::operator=(Box<Term>&& o) noexcept {
        if (this == &o) return *this;
        if (ptr) { ptr->~Term(); os::deallocate(ptr); ptr = nullptr; }
        ptr = o.ptr;
        o.ptr = nullptr;
        return *this;
    }
    template<> inline Box<Term>::~Box() {
        if (ptr) { ptr->~Term(); os::deallocate(ptr); ptr = nullptr; }
    }

    // ========================================================================
    // ArithmeticOperation / TypeHint constructors (now that Term is complete)
    // ========================================================================
    inline UnaryMinusArithmeticOperation::UnaryMinusArithmeticOperation(Term&& t)
        : operand(move(t)) {}

    inline BinaryArithmeticOperation::BinaryArithmeticOperation(Term&& l, ArithmeticOperator o, Term&& r)
        : lhs(move(l)), op(o), rhs(move(r)) {}

    inline TypeHint::TypeHint(Type t, Term&& o)
        : type(t), operand(move(o)) {}

    // ========================================================================
    // TOI types (TermWithIdentifiers variant types)
    // ========================================================================
    struct TOIUnaryMinus {
        Box<TermWithIdentifiers> operand;
        explicit TOIUnaryMinus(TermWithIdentifiers&& t);
        TOIUnaryMinus() = default;
    };
    struct TOIBinaryArithmetic {
        Box<TermWithIdentifiers> lhs;
        ArithmeticOperator op;
        Box<TermWithIdentifiers> rhs;
        TOIBinaryArithmetic(TermWithIdentifiers&& l, ArithmeticOperator o, TermWithIdentifiers&& r);
        TOIBinaryArithmetic() = default;
    };
    struct TOIArithmeticOperation {
        TaggedUnion<TOIUnaryMinus, TOIBinaryArithmetic> value;
    };

    // ========================================================================
    // TermWithIdentifiers — defined after TOI types
    // ========================================================================
    struct TermWithIdentifiers {
        ExpandTaggedUnion<
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

    // ========================================================================
    // Box<TermWithIdentifiers> method bodies
    // ========================================================================
    template<> inline Box<TermWithIdentifiers>::Box(TermWithIdentifiers&& val) {
        ptr = reinterpret_cast<TermWithIdentifiers*>(os::allocate(sizeof(TermWithIdentifiers)));
        new (ptr) TermWithIdentifiers(move(val));
    }
    template<> inline Box<TermWithIdentifiers>::Box(const Box<TermWithIdentifiers>& o) {
        if (o.ptr) {
            ptr = reinterpret_cast<TermWithIdentifiers*>(os::allocate(sizeof(TermWithIdentifiers)));
            new (ptr) TermWithIdentifiers(*o.ptr);
        }
    }
    template<> inline Box<TermWithIdentifiers>::Box(Box<TermWithIdentifiers>&& o) noexcept {
        ptr = o.ptr;
        o.ptr = nullptr;
    }
    template<> inline Box<TermWithIdentifiers>& Box<TermWithIdentifiers>::operator=(const Box<TermWithIdentifiers>& o) {
        if (this == &o) return *this;
        if (ptr) { ptr->~TermWithIdentifiers(); os::deallocate(ptr); ptr = nullptr; }
        if (o.ptr) {
            ptr = reinterpret_cast<TermWithIdentifiers*>(os::allocate(sizeof(TermWithIdentifiers)));
            new (ptr) TermWithIdentifiers(*o.ptr);
        }
        return *this;
    }
    template<> inline Box<TermWithIdentifiers>& Box<TermWithIdentifiers>::operator=(Box<TermWithIdentifiers>&& o) noexcept {
        if (this == &o) return *this;
        if (ptr) { ptr->~TermWithIdentifiers(); os::deallocate(ptr); ptr = nullptr; }
        ptr = o.ptr;
        o.ptr = nullptr;
        return *this;
    }
    template<> inline Box<TermWithIdentifiers>::~Box() {
        if (ptr) { ptr->~TermWithIdentifiers(); os::deallocate(ptr); ptr = nullptr; }
    }

    // ========================================================================
    // TOI constructors (now that TermWithIdentifiers is complete)
    // ========================================================================
    inline TOIUnaryMinus::TOIUnaryMinus(TermWithIdentifiers&& t)
        : operand(move(t)) {}

    inline TOIBinaryArithmetic::TOIBinaryArithmetic(TermWithIdentifiers&& l, ArithmeticOperator o, TermWithIdentifiers&& r)
        : lhs(move(l)), op(o), rhs(move(r)) {}

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
        struct Selector;  // forward-declare for Function (DynamicArray<Selector> needs only a pointer)
        struct Function {
            AutoString8 function_name;
            DynamicArray<Selector> arguments;
        };
        struct Count {};
        // Static types (ColumnName, Term, Function, Count) are stored inline.
        // Cast is dynamic: it directly embeds Selector, so its size depends on Selector's size.
        struct Selector {
            HybridTaggedUnion<TypeList<ColumnName, Term, Function, Count>, TypeList<Cast>> value;
        };
        struct Cast {
            Selector column;
            Type to;
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

export namespace cql {
    inline U64 hash(const UUID& uuid) {
        return plexdb::hash(plexdb::String8(&uuid.value[0], uuid.length));
    }

    inline U64 hash(const Blob& blob) {
        return plexdb::hash(plexdb::String8(blob.value.ptr, blob.value.length));
    }

    inline U64 hash(const Hex& hex) {
        return plexdb::hash(plexdb::String8(hex.value.ptr, hex.value.length));
    }

    inline U64 hash(const Inet& inet) {
        if (inet.is_v6)
            return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&inet.v6[0]), 16));
        else
            return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&inet.v4[0]), 4));
    }

    inline U64 hash(const VarInt& vi) {
        U64 h = plexdb::hash(plexdb::String8(vi.magnitude.ptr, vi.magnitude.length));
        h ^= vi.negative ? 0x1ULL : 0x0ULL;
        return h;
    }

    inline U64 hash(const Decimal& d) {
        U64 h = hash(d.unscaled);
        U8 scale_bytes[4];
        os::memory_copy(scale_bytes, &d.scale, 4);
        h ^= plexdb::hash(plexdb::String8(scale_bytes, 4));
        return h;
    }

    inline U64 hash(const Duration& dur) {
        U8 buf[16];
        os::memory_copy(buf,     &dur.months,      4);
        os::memory_copy(buf + 4, &dur.days,        4);
        os::memory_copy(buf + 8, &dur.nanoseconds, 8);
        return plexdb::hash(plexdb::String8(buf, 16));
    }
}
