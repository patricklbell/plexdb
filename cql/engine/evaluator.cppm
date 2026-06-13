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
        TArrayView<const Constant>                    positional_bindings{};
        TArrayView<const Pair<AutoString8, Constant>> named_bindings{};
        const schema::Table*                          table      = nullptr;
        const ColumnValue*                            row_values = nullptr; // array of length table->cols.length
    };

    using EvaluatedTypes = TypeList<
        Constant, MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral, ColumnValue>;

    struct Evaluated {
        // @note @warn DO NOT modify without also checking TermWithIdentifier
        ExpandTaggedUnion<EvaluatedTypes> value;
    };

    Evaluated evaluate(const Term& term, const EvalContext& ctx);
    Evaluated evaluate(const Term& term);
    Evaluated evaluate(const TermWithIdentifiers& twi, const EvalContext& ctx);

    bool evaluate_where(TArrayView<const WhereClause::Relation> predicates, const EvalContext& ctx);

    void number_bind_markers(Statement& stmt);
}

export namespace plexdb {
    AutoString8 to_str(cql::Evaluated c, cql::type::Basic dtype);
}
