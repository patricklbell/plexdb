export module objstore.engine.evaluator;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os.containers;
import plexdb.os.dynamic_tagged_union;

import objstore.engine.statements;
import objstore.engine.types;

using namespace plexdb;

export namespace objstore {
    using EvaluatedTypes = TypeList<
        Constant, MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral
    >;
    struct Evaluated {
        // @note @warn DO NOT modify without also checking TermWithIdentifier
        ExpandDynamicTaggedUnion<EvaluatedTypes> value;
    };

    // @warn suggested to move term to avoid copy
    Evaluated evaluate(Term term) {
        assert_true_not_implemented(type_matches_tag<Constant>(term.value), "non-constant term evaluation is not implemented");
        return {get<Constant>(term.value)};
    }
}

export namespace plexdb {
    U64 hash(const objstore::Constant& constant) {
        return visit(constant.value, [](const auto& cv) -> U64 {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, S64>) return hash(static_cast<U64>(cv));
            else if constexpr (SameAs<T, AutoString8>) return hash(cv);
            assert_not_implemented("hash for constant value type is not implemented");
            return 0_u64;
        });
    }
    U64 hash(const objstore::Evaluated& evaluated) {
        return visit(evaluated.value, [](const auto& cv) -> U64 {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T,objstore::Constant>) return hash(cv);
            assert_not_implemented("hash for evaluated value type is not implemented");
            return 0_u64;
        });
    }

    AutoString8 to_str(objstore::Evaluated c, objstore::BasicType dtype) {
        switch (dtype) {
            case objstore::BasicType::text:{
                return get<AutoString8>(c.value);
            }break;
            case objstore::BasicType::smallint:{
                return to_str(get<S16>(c.value));
            }break;
            case objstore::BasicType::int_:{
                return to_str(get<S32>(c.value));
            }break;
            case objstore::BasicType::counter:
            case objstore::BasicType::timestamp:
            case objstore::BasicType::bigint:{
                return to_str(get<S64>(c.value));
            }break;
            case objstore::BasicType::boolean:{
                return static_cast<bool>(get<U8>(c.value)) ? "true"_as : "false"_as;
            }break;
            case objstore::BasicType::float_:{
                return to_str(get<F32>(c.value));
            }break;
            case objstore::BasicType::double_:{
                return to_str(get<F64>(c.value));
            }break;
            case objstore::BasicType::uuid:
            case objstore::BasicType::ascii:
            case objstore::BasicType::blob:
            case objstore::BasicType::date:
            case objstore::BasicType::decimal:
            case objstore::BasicType::duration:
            case objstore::BasicType::inet:
            case objstore::BasicType::time:
            case objstore::BasicType::timeuuid:
            case objstore::BasicType::tinyint:
            case objstore::BasicType::varchar:
            case objstore::BasicType::varint:
            case objstore::BasicType::vector:
            case objstore::BasicType::hex:
                return "@todo"_as;
        }
        return "unknown"_as;
    }
}