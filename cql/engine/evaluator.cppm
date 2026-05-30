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
        Constant, MapLiteral, SetLiteral, ListOrVectorLiteral, UdtLiteral, TupleLiteral, ColumnValue
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
        if (type_matches_tag<ColumnValue>(term.value)) {
            return {get<ColumnValue>(move(term.value))};
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
        if (!type_matches_tag<cql::Constant>(c.value)) return "@todo"_as;
        auto& con = get<cql::Constant>(c.value);

        if (type_matches_tag<cql::Null>(con.value)) return "null"_as;

        switch (dtype) {
            case cql::BasicType::text:
            case cql::BasicType::ascii:
            case cql::BasicType::varchar:
                return get<AutoString8>(con.value);
            case cql::BasicType::smallint:
                return to_str(static_cast<S64>(static_cast<S16>(get<S64>(con.value))));
            case cql::BasicType::int_:
                return to_str(static_cast<S64>(static_cast<S32>(get<S64>(con.value))));
            case cql::BasicType::counter:
            case cql::BasicType::timestamp:
            case cql::BasicType::bigint:
                return to_str(get<S64>(con.value));
            case cql::BasicType::boolean:
                return get<bool>(con.value) ? "true"_as : "false"_as;
            case cql::BasicType::float_:
                return to_str(static_cast<F32>(get<F64>(con.value)));
            case cql::BasicType::double_:
                return to_str(get<F64>(con.value));
            case cql::BasicType::tinyint:
            case cql::BasicType::date:
            case cql::BasicType::time:
                return to_str(get<S64>(con.value));
            case cql::BasicType::uuid:
            case cql::BasicType::timeuuid: {
                auto& uuid = get<cql::UUID>(con.value);
                const char hx[] = "0123456789abcdef";
                AutoString8 result{36};
                char* o = result.c_str;
                const U8* b = &uuid.value[0];
                for (int i = 0; i < 4;  i++) { o[i*2]    = hx[b[i]>>4];     o[i*2+1]    = hx[b[i]&0xf]; }
                o[8] = '-';
                for (int i = 0; i < 2;  i++) { o[9+i*2]  = hx[b[4+i]>>4];   o[10+i*2]   = hx[b[4+i]&0xf]; }
                o[13] = '-';
                for (int i = 0; i < 2;  i++) { o[14+i*2] = hx[b[6+i]>>4];   o[15+i*2]   = hx[b[6+i]&0xf]; }
                o[18] = '-';
                for (int i = 0; i < 2;  i++) { o[19+i*2] = hx[b[8+i]>>4];   o[20+i*2]   = hx[b[8+i]&0xf]; }
                o[23] = '-';
                for (int i = 0; i < 6;  i++) { o[24+i*2] = hx[b[10+i]>>4];  o[25+i*2]   = hx[b[10+i]&0xf]; }
                return result;
            }
            case cql::BasicType::blob:
            case cql::BasicType::inet:
            case cql::BasicType::varint:
            case cql::BasicType::decimal:
            case cql::BasicType::duration: {
                auto& blob = get<cql::Blob>(con.value);
                const char hx[] = "0123456789abcdef";
                AutoString8 hex{blob.value.length * 2};
                for (U64 i = 0; i < blob.value.length; i++) {
                    hex.c_str[i*2]     = hx[blob.value.ptr[i] >> 4];
                    hex.c_str[i*2 + 1] = hx[blob.value.ptr[i] & 0xf];
                }
                return "0x"_as + hex;
            }
            case cql::BasicType::hex: {
                auto& hex = get<cql::Hex>(con.value);
                const char hx[] = "0123456789abcdef";
                AutoString8 result{hex.value.length * 2};
                for (U64 i = 0; i < hex.value.length; i++) {
                    result.c_str[i*2]     = hx[hex.value.ptr[i] >> 4];
                    result.c_str[i*2 + 1] = hx[hex.value.ptr[i] & 0xf];
                }
                return "0x"_as + result;
            }
        }
        return "unknown"_as;
    }
}
