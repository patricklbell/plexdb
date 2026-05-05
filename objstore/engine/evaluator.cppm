export module objstore.engine.evaluator;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import objstore.engine.statements;
import objstore.engine.types;

using namespace plexdb;

export namespace objstore {
    using EvaluatedTypes = TypeList<
        Constant, MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral
    >;
    struct Evaluated {
        // @note @warn DO NOT modify without also checking TermWithIdentifier
        ExpandAutoTaggedUnion<EvaluatedTypes> value;
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

export namespace objstore {
    U64 hash(const Constant& constant) {
        return visit(constant.value, [](const auto& cv) -> U64 {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, S64>) return plexdb::hash(static_cast<U64>(cv));
            else if constexpr (SameAs<T, AutoString8>) return plexdb::hash(cv);
            else if constexpr (SameAs<T, bool>) return plexdb::hash(static_cast<U64>(cv));
            else if constexpr (SameAs<T, F64>) {
                U64 bits;
                os::memory_copy(&bits, &cv, sizeof(bits));
                return plexdb::hash(bits);
            }
            else if constexpr (SameAs<T, UUID>) {
                return plexdb::hash(plexdb::String8(&cv.value[0], cv.length));
            }
            else if constexpr (SameAs<T, Null>) return 0_u64;
            else if constexpr (SameAs<T, Hex>) {
                return plexdb::hash(plexdb::String8(cv.value.ptr, cv.value.length));
            }
            else if constexpr (SameAs<T, Blob>) {
                return plexdb::hash(plexdb::String8(cv.value.ptr, cv.value.length));
            }
            else { static_assert(!SameAs<T,T>, "missing hash for constant value type"); }
            return 0_u64;
        });
    }
    U64 hash(const Evaluated& evaluated) {
        return visit(evaluated.value, [](const auto& cv) -> U64 {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T,Constant>) return hash(cv);
            assert_not_implemented("hash for evaluated value type is not implemented");
            return 0_u64;
        });
    }
}

export namespace plexdb {
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
