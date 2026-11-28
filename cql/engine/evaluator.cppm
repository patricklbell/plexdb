export module cql.engine.evaluator;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.column_value;
import cql.engine.schema;
import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;
using namespace cql::column_types;

export namespace cql {
    struct EvalContext {
        TArrayView<const Literal>                    positional_bindings{};
        TArrayView<const Pair<AutoString8, Literal>> named_bindings{};
        const schema::Table*                         table      = nullptr;
        const ColumnValue*                           row_values = nullptr; // array of length table->cols.length
    };

    using EvaluatedTypes = TypeList<
        Literal, MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral, ColumnValue>;

    struct Evaluated {
        // @note @warn DO NOT modify without also checking TermWithIdentifier
        ExpandTaggedUnion<EvaluatedTypes> value;
    };

    Evaluated evaluate(const Term& term, const EvalContext& ctx);
    Evaluated evaluate(const Term& term);
    Evaluated evaluate(const TermWithIdentifiers& twi, const EvalContext& ctx);

    bool evaluate_where(TArrayView<const WhereClause::Relation> predicates, const EvalContext& ctx);

    // Cassandra's ordering for a value of `type`: scalars by their type's comparator,
    // lists/vectors/tuples/UDTs field by field (null first), sets by sorted elements and maps by
    // sorted (key, value) entries.
    S32 compare_typed_values(const ColumnValue& a, const ColumnValue& b, const type::Type& type);

    // Set elements / map entries in Cassandra's order (by element / key).
    DynamicArray<const NestedColumnValue*>                                           sorted_set_elements(const DynamicSet<NestedColumnValue>& set, const type::Type& element_type);
    DynamicArray<const Pair<NestedColumnValue, NestedColumnValue>*> sorted_map_entries(const DynamicMap<NestedColumnValue, NestedColumnValue>& map, const type::Type& key_type);

    // Invoke a built-in function by name (case-insensitive) with pre-evaluated
    // arguments. Returns Evaluated{Literal{Null{}}} when no registered function
    // matches `name`. The lookup is the same registry the term evaluator uses
    // for FunctionCall terms, so SELECT-projection sites can route generic
    // function calls through it.
    Evaluated call_registered_function(String8 name, TArrayView<const Evaluated> args, const EvalContext& ctx);
    bool      registered_function_exists(String8 name);

    // Extract the outermost TypeHint's asserted basic type from a Term or
    // TermWithIdentifiers. Empty when the outermost node is not a TypeHint or
    // its asserted type is not a basic type.
    Optional<type::Basic> outer_type_hint_basic(const Term& term);
    Optional<type::Basic> outer_type_hint_basic(const TermWithIdentifiers& twi);

}
