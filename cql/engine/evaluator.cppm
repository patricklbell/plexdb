export module cql.engine.evaluator;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;

export namespace cql {
    using EvaluatedTypes = TypeList<
        Constant, MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral
    >;
    struct Evaluated {
        // @note @warn DO NOT modify without also checking TermWithIdentifier
        ExpandTaggedUnion<EvaluatedTypes> value;
    };

    // @warn suggested to move term to avoid copy
    Evaluated evaluate(Term term) {
        if (type_matches_tag<Constant>(term.value)) {
            return {get<Constant>(term.value)};
        }
        if (type_matches_tag<MapLiteral>(term.value)) {
            return {get<MapLiteral>(move(term.value))};
        }
        if (type_matches_tag<SetLiteral>(term.value)) {
            return {get<SetLiteral>(move(term.value))};
        }
        if (type_matches_tag<ListOrVectorLiteral>(term.value)) {
            return {get<ListOrVectorLiteral>(move(term.value))};
        }
        if (type_matches_tag<TupleLiteral>(term.value)) {
            return {get<TupleLiteral>(move(term.value))};
        }
        if (type_matches_tag<UdtLiteral>(term.value)) {
            return {get<UdtLiteral>(move(term.value))};
        }
        if (type_matches_tag<BindMarker>(term.value)) {
            // unresolved bind marker evaluates to null
            // @todo check this is valid
            return {Constant{.value = Null{}}};
        }
        // @todo implement function calls, arithmetic, type hints
        assert_true_not_implemented(false, "non-constant/non-literal term evaluation is not implemented");
        return {Constant{.value = Null{}}};
    }
}

export namespace plexdb {
    AutoString8 to_str(cql::Evaluated c, cql::BasicType dtype) {
        switch (dtype) {
            case cql::BasicType::text:{
                return get<AutoString8>(c.value);
            }break;
            case cql::BasicType::smallint:{
                return to_str(get<S16>(c.value));
            }break;
            case cql::BasicType::int_:{
                return to_str(get<S32>(c.value));
            }break;
            case cql::BasicType::counter:
            case cql::BasicType::timestamp:
            case cql::BasicType::bigint:{
                return to_str(get<S64>(c.value));
            }break;
            case cql::BasicType::boolean:{
                return static_cast<bool>(get<U8>(c.value)) ? "true"_as : "false"_as;
            }break;
            case cql::BasicType::float_:{
                return to_str(get<F32>(c.value));
            }break;
            case cql::BasicType::double_:{
                return to_str(get<F64>(c.value));
            }break;
            case cql::BasicType::uuid:
            case cql::BasicType::ascii:
            case cql::BasicType::blob:
            case cql::BasicType::date:
            case cql::BasicType::decimal:
            case cql::BasicType::duration:
            case cql::BasicType::inet:
            case cql::BasicType::time:
            case cql::BasicType::timeuuid:
            case cql::BasicType::tinyint:
            case cql::BasicType::varchar:
            case cql::BasicType::varint:
            case cql::BasicType::hex:
                return "@todo"_as;
        }
        return "unknown"_as;
    }
}
