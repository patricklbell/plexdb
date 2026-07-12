module cql.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import cql.engine.types;
import cql.engine.column_value;

using namespace plexdb;

namespace cql {
    bool is_inequality(Operator op) {
        return op == Operator::lt || op == Operator::le || op == Operator::gt || op == Operator::ge;
    }

    bool tuple_rhs_is_single_value(const WhereClause::TupleExpressionRelation& r) {
        return r.values.length == 1
            && r.columns.length > 1
            && type_matches_tag<TupleLiteral>(r.values[0].value)
            && get<TupleLiteral>(r.values[0].value).elements.length == r.columns.length;
    }

    bool tuple_rhs_is_compatible(const WhereClause::TupleExpressionRelation& r) {
        return (r.columns.length > 0 && r.columns.length == r.values.length) || tuple_rhs_is_single_value(r);
    }

    const Term& tuple_value_at(const WhereClause::TupleExpressionRelation& r, U64 i) {
        if (r.values.length == r.columns.length) {
            return r.values[i];
        }
        return get<TupleLiteral>(r.values[0].value).elements[i];
    }
}
