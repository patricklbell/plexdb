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

    // True when a value asserted as type `from` can be written to a column of
    // type `to`. Cassandra rules: same type accepts; ascii widens to text /
    // varchar; text and varchar interconvert; everything else strict.
    bool type_compatible_for_assignment(type::Basic from, type::Basic to);

    void number_bind_markers(Statement& stmt);
}

export namespace plexdb {
    AutoString8 to_str(cql::Evaluated c, cql::type::Basic dtype);
}
