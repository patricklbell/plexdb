module cql.engine.evaluator;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;

namespace cql {
    // ========================================================================
    // internal types
    // ========================================================================
    struct FunctionEntry {
        AutoString8 name;
        Evaluated (*fn)(TArrayView<const Evaluated>, const EvalContext&);
    };

    // ========================================================================
    // uuid / time helpers
    // ========================================================================
    static constexpr S64 UUID_EPOCH_OFFSET_100NS = 122192928000000000LL;
    static constexpr S64 MS_PER_DAY              = 86400000LL;

    static U64 unix_ms_to_uuid_timestamp_100ns(S64 unix_ms) {
        return static_cast<U64>(unix_ms) * 10000_u64 + static_cast<U64>(UUID_EPOCH_OFFSET_100NS);
    }

    static S64 uuid_timestamp_100ns_to_unix_ms(U64 ts) {
        return (static_cast<S64>(ts) - UUID_EPOCH_OFFSET_100NS) / 10000LL;
    }

    static UUID make_timeuuid_from_unix_ms(S64 unix_ms, U16 clock_seq, const Array<U8, 6>& node) {
        UUID uuid{};
        U64 timestamp = unix_ms_to_uuid_timestamp_100ns(unix_ms);
        uuid.value[0] = static_cast<U8>((timestamp >> 24) & 0xff_u64);
        uuid.value[1] = static_cast<U8>((timestamp >> 16) & 0xff_u64);
        uuid.value[2] = static_cast<U8>((timestamp >>  8) & 0xff_u64);
        uuid.value[3] = static_cast<U8>( timestamp        & 0xff_u64);
        uuid.value[4] = static_cast<U8>((timestamp >> 40) & 0xff_u64);
        uuid.value[5] = static_cast<U8>((timestamp >> 32) & 0xff_u64);
        uuid.value[6] = static_cast<U8>(0x10_u8 | static_cast<U8>((timestamp >> 56) & 0x0f_u64));
        uuid.value[7] = static_cast<U8>((timestamp >> 48) & 0xff_u64);
        uuid.value[8] = static_cast<U8>(0x80_u8 | static_cast<U8>((clock_seq >> 8) & 0x3f_u16));
        uuid.value[9] = static_cast<U8>(clock_seq & 0xff_u16);
        for (U64 i = 0; i < 6; i++) uuid.value[10 + i] = node.values[i];
        return uuid;
    }

    static S64 timeuuid_to_unix_ms(const UUID& uuid) {
        U64 timestamp = 0;
        timestamp |= static_cast<U64>(uuid.value[0]) << 24;
        timestamp |= static_cast<U64>(uuid.value[1]) << 16;
        timestamp |= static_cast<U64>(uuid.value[2]) <<  8;
        timestamp |= static_cast<U64>(uuid.value[3]);
        timestamp |= static_cast<U64>(uuid.value[4]) << 40;
        timestamp |= static_cast<U64>(uuid.value[5]) << 32;
        timestamp |= static_cast<U64>(uuid.value[6] & 0x0f_u8) << 56;
        timestamp |= static_cast<U64>(uuid.value[7]) << 48;
        return uuid_timestamp_100ns_to_unix_ms(timestamp);
    }

    static UUID random_uuid_v4() {
        UUID uuid{};
        os::random_bytes(&uuid.value[0], UUID::length);
        uuid.value[6] = static_cast<U8>((uuid.value[6] & 0x0f_u8) | 0x40_u8);
        uuid.value[8] = static_cast<U8>((uuid.value[8] & 0x3f_u8) | 0x80_u8);
        return uuid;
    }

    static UUID now_timeuuid() {
        Array<U8, 8> entropy{};
        os::random_bytes(entropy.values, 8);
        Array<U8, 6> node{};
        for (U64 i = 0; i < 6; i++) node.values[i] = entropy.values[i];
        U16 clock_seq = static_cast<U16>(
            static_cast<U16>(entropy.values[6]) | (static_cast<U16>(entropy.values[7]) << 8)
        ) & 0x3fff_u16;
        return make_timeuuid_from_unix_ms(static_cast<S64>(os::unix_ms_now()), clock_seq, node);
    }

    static UUID min_timeuuid(S64 unix_ms) {
        Array<U8, 6> node{};
        return make_timeuuid_from_unix_ms(unix_ms, 0_u16, node);
    }

    static UUID max_timeuuid(S64 unix_ms) {
        Array<U8, 6> node{};
        for (U64 i = 0; i < 6; i++) node.values[i] = 0xff_u8;
        return make_timeuuid_from_unix_ms(unix_ms, 0x3fff_u16, node);
    }

    // ========================================================================
    // function registry
    // ========================================================================
    static DynamicArray<FunctionEntry>& builtin_function_registry() {
        static DynamicArray<FunctionEntry> registry{};
        static bool initialized = false;
        if (initialized) return registry;
        initialized = true;

        auto add = [&](AutoString8 name, Evaluated (*fn)(TArrayView<const Evaluated>, const EvalContext&)) {
            push_back(registry, FunctionEntry{move(name), fn});
        };

        add("uuid"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Constant{random_uuid_v4()}};
        });
        add("now"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Constant{now_timeuuid()}};
        });
        add("currenttimeuuid"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Constant{now_timeuuid()}};
        });
        add("currenttimestamp"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Constant{static_cast<S64>(os::unix_ms_now())}};
        });
        add("currentdate"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Constant{os::unix_days_now()}};
        });
        add("currenttime"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Constant{os::unix_ns_since_midnight_now()}};
        });
        add("todate"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1 || !type_matches_tag<Constant>(args[0].value)) return Evaluated{Constant{Null{}}};
            auto& value = get<Constant>(args[0].value).value;
            if (type_matches_tag<S64>(value))
                return Evaluated{Constant{static_cast<S64>(get<S64>(value) / MS_PER_DAY)}};
            if (type_matches_tag<UUID>(value))
                return Evaluated{Constant{static_cast<S64>(timeuuid_to_unix_ms(get<UUID>(value)) / MS_PER_DAY)}};
            return Evaluated{Constant{Null{}}};
        });
        add("totimestamp"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1 || !type_matches_tag<Constant>(args[0].value)) return Evaluated{Constant{Null{}}};
            auto& value = get<Constant>(args[0].value).value;
            if (type_matches_tag<S64>(value))  return Evaluated{Constant{get<S64>(value)}};
            if (type_matches_tag<UUID>(value))  return Evaluated{Constant{timeuuid_to_unix_ms(get<UUID>(value))}};
            return Evaluated{Constant{Null{}}};
        });
        add("tounixtimestamp"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1 || !type_matches_tag<Constant>(args[0].value)) return Evaluated{Constant{Null{}}};
            auto& value = get<Constant>(args[0].value).value;
            if (type_matches_tag<S64>(value))  return Evaluated{Constant{get<S64>(value)}};
            if (type_matches_tag<UUID>(value))  return Evaluated{Constant{timeuuid_to_unix_ms(get<UUID>(value))}};
            return Evaluated{Constant{Null{}}};
        });
        add("mintimeuuid"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1 || !type_matches_tag<Constant>(args[0].value)
                                 || !type_matches_tag<S64>(get<Constant>(args[0].value).value))
                return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{min_timeuuid(get<S64>(get<Constant>(args[0].value).value))}};
        });
        add("maxtimeuuid"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1 || !type_matches_tag<Constant>(args[0].value)
                                 || !type_matches_tag<S64>(get<Constant>(args[0].value).value))
                return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{max_timeuuid(get<S64>(get<Constant>(args[0].value).value))}};
        });
        add("dateof"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1 || !type_matches_tag<Constant>(args[0].value)
                                 || !type_matches_tag<UUID>(get<Constant>(args[0].value).value))
                return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{timeuuid_to_unix_ms(get<UUID>(get<Constant>(args[0].value).value))}};
        });
        add("unixtimestampof"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1 || !type_matches_tag<Constant>(args[0].value)
                                 || !type_matches_tag<UUID>(get<Constant>(args[0].value).value))
                return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{timeuuid_to_unix_ms(get<UUID>(get<Constant>(args[0].value).value))}};
        });

        return registry;
    }

    static FunctionEntry* find_function(const AutoString8& identifier) {
        AutoString8 name = identifier;
        to_lowercase_inplace(name);
        auto& registry = builtin_function_registry();
        for (U64 i = 0; i < registry.length; i++) {
            if (registry[i].name == name && registry[i].fn != nullptr) return &registry[i];
        }
        return nullptr;
    }

    // ========================================================================
    // bind marker resolution
    // ========================================================================
    static const Constant* lookup_positional_binding(const AutoString8& identifier, const EvalContext& ctx) {
        if (identifier.length == 0) return nullptr;
        if (identifier.c_str[0] == ':') return nullptr;
        U64 index = 0;
        for (U64 i = 0; i < identifier.length; i++) {
            if (identifier.c_str[i] < '0' || identifier.c_str[i] > '9') return nullptr;
            index = index * 10_u64 + static_cast<U64>(identifier.c_str[i] - '0');
        }
        if (index >= ctx.positional_bindings.length) return nullptr;
        return &ctx.positional_bindings[index];
    }

    static const Constant* lookup_named_binding(const AutoString8& identifier, const EvalContext& ctx) {
        if (identifier.length == 0) return nullptr;
        AutoString8 key = identifier;
        if (key.c_str[0] == ':') {
            AutoString8 stripped{key.length - 1};
            for (U64 i = 0; i < stripped.length; i++) stripped.c_str[i] = key.c_str[i + 1];
            key = move(stripped);
        }
        to_lowercase_inplace(key);
        for (U64 i = 0; i < ctx.named_bindings.length; i++) {
            AutoString8 name = ctx.named_bindings[i].first;
            to_lowercase_inplace(name);
            if (name == key) return &ctx.named_bindings[i].second;
        }
        return nullptr;
    }

    static U64 number_bind_markers_in_term(Term& term, U64 idx) {
        if (type_matches_tag<BindMarker>(term.value)) {
            auto& m = get<BindMarker>(term.value);
            if (m.identifier.length == 0) {
                m.identifier = to_str(idx);
                return idx + 1;
            }
            return idx;
        }
        if (type_matches_tag<ArithmeticOperation>(term.value)) {
            auto& arith = get<ArithmeticOperation>(term.value);
            if (type_matches_tag<BinaryArithmeticOperation>(arith.value)) {
                auto& bin = get<BinaryArithmeticOperation>(arith.value);
                idx = number_bind_markers_in_term(*bin.lhs, idx);
                idx = number_bind_markers_in_term(*bin.rhs, idx);
            } else {
                idx = number_bind_markers_in_term(*get<UnaryMinusArithmeticOperation>(arith.value).operand, idx);
            }
        }
        if (type_matches_tag<FunctionCall>(term.value)) {
            auto& call = get<FunctionCall>(term.value);
            for (U64 i = 0; i < call.arguments.length; i++)
                idx = number_bind_markers_in_term(call.arguments[i], idx);
        }
        if (type_matches_tag<TypeHint>(term.value))
            idx = number_bind_markers_in_term(*get<TypeHint>(term.value).operand, idx);
        return idx;
    }

    void number_bind_markers(Statement& stmt) {
        U64 idx = 0;
        visit(stmt.value, [&](auto& s) {
            using T = RemoveCVRef<decltype(s)>;
            if constexpr (SameAs<T, Insert>) {
                if (!type_matches_tag<Insert::NamesValues>(s.insert_clause)) return;
                auto& nv = get<Insert::NamesValues>(s.insert_clause);
                for (U64 i = 0; i < nv.values.length; i++)
                    idx = number_bind_markers_in_term(nv.values[i], idx);
            }
            // @todo Update, Delete, Select WHERE clause
        });
    }

    // ========================================================================
    // term evaluation
    // ========================================================================
    static Evaluated evaluate_term(const Term& term, const EvalContext& ctx);
    static Evaluated evaluate_function_call(const FunctionCall& call, const EvalContext& ctx);

    static Evaluated evaluate_binary_arithmetic(const Constant& lhs, ArithmeticOperator op, const Constant& rhs) {
        if (type_matches_tag<Null>(lhs.value) || type_matches_tag<Null>(rhs.value))
            return Evaluated{Constant{Null{}}};

        if (type_matches_tag<AutoString8>(lhs.value) && type_matches_tag<AutoString8>(rhs.value)) {
            if (op != ArithmeticOperator::plus) return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{get<AutoString8>(lhs.value) + get<AutoString8>(rhs.value)}};
        }

        bool lhs_is_float = type_matches_tag<F64>(lhs.value);
        bool rhs_is_float = type_matches_tag<F64>(rhs.value);
        bool lhs_is_int   = type_matches_tag<S64>(lhs.value);
        bool rhs_is_int   = type_matches_tag<S64>(rhs.value);

        if (lhs_is_float || rhs_is_float) {
            if (op == ArithmeticOperator::mod) return Evaluated{Constant{Null{}}};
            F64 l = lhs_is_float ? get<F64>(lhs.value) : static_cast<F64>(get<S64>(lhs.value));
            F64 r = rhs_is_float ? get<F64>(rhs.value) : static_cast<F64>(get<S64>(rhs.value));
            switch (op) {
                case ArithmeticOperator::plus:   return Evaluated{Constant{l + r}};
                case ArithmeticOperator::minus:  return Evaluated{Constant{l - r}};
                case ArithmeticOperator::times:  return Evaluated{Constant{l * r}};
                case ArithmeticOperator::divide: return Evaluated{Constant{l / r}};
                case ArithmeticOperator::mod:    break;
            }
        }

        if (!lhs_is_int || !rhs_is_int) return Evaluated{Constant{Null{}}};
        S64 l = get<S64>(lhs.value);
        S64 r = get<S64>(rhs.value);
        if ((op == ArithmeticOperator::divide || op == ArithmeticOperator::mod) && r == 0)
            return Evaluated{Constant{Null{}}};
        switch (op) {
            case ArithmeticOperator::plus:   return Evaluated{Constant{l + r}};
            case ArithmeticOperator::minus:  return Evaluated{Constant{l - r}};
            case ArithmeticOperator::times:  return Evaluated{Constant{l * r}};
            case ArithmeticOperator::divide: return Evaluated{Constant{l / r}};
            case ArithmeticOperator::mod:    return Evaluated{Constant{l % r}};
        }
        return Evaluated{Constant{Null{}}};
    }

    static Evaluated evaluate_unary_minus(const Constant& value) {
        if (type_matches_tag<Null>(value.value)) return Evaluated{Constant{Null{}}};
        if (type_matches_tag<S64>(value.value))  return Evaluated{Constant{-get<S64>(value.value)}};
        if (type_matches_tag<F64>(value.value))  return Evaluated{Constant{-get<F64>(value.value)}};
        return Evaluated{Constant{Null{}}};
    }

    static Evaluated evaluate_term(const Term& term, const EvalContext& ctx) {
        if (type_matches_tag<Constant>(term.value))             return {get<Constant>(term.value)};
        if (type_matches_tag<MapLiteral>(term.value))           return {get<MapLiteral>(term.value)};
        if (type_matches_tag<SetLiteral>(term.value))           return {get<SetLiteral>(term.value)};
        if (type_matches_tag<ListOrVectorLiteral>(term.value))  return {get<ListOrVectorLiteral>(term.value)};
        if (type_matches_tag<TupleLiteral>(term.value))         return {get<TupleLiteral>(term.value)};
        if (type_matches_tag<UdtLiteral>(term.value))           return {get<UdtLiteral>(term.value)};
        if (type_matches_tag<BindMarker>(term.value)) {
            auto& marker = get<BindMarker>(term.value);
            if (const Constant* c = lookup_positional_binding(marker.identifier, ctx)) return {*c};
            if (const Constant* c = lookup_named_binding(marker.identifier, ctx))      return {*c};
            return Evaluated{Constant{Null{}}};
        }
        if (type_matches_tag<FunctionCall>(term.value))
            return evaluate_function_call(get<FunctionCall>(term.value), ctx);
        if (type_matches_tag<ArithmeticOperation>(term.value)) {
            auto& arith = get<ArithmeticOperation>(term.value);
            if (type_matches_tag<UnaryMinusArithmeticOperation>(arith.value)) {
                Evaluated operand = evaluate_term(*get<UnaryMinusArithmeticOperation>(arith.value).operand, ctx);
                if (!type_matches_tag<Constant>(operand.value)) return Evaluated{Constant{Null{}}};
                return evaluate_unary_minus(get<Constant>(operand.value));
            }
            auto& binary = get<BinaryArithmeticOperation>(arith.value);
            Evaluated lhs = evaluate_term(*binary.lhs, ctx);
            Evaluated rhs = evaluate_term(*binary.rhs, ctx);
            if (!type_matches_tag<Constant>(lhs.value) || !type_matches_tag<Constant>(rhs.value))
                return Evaluated{Constant{Null{}}};
            return evaluate_binary_arithmetic(get<Constant>(lhs.value), binary.op, get<Constant>(rhs.value));
        }
        if (type_matches_tag<TypeHint>(term.value))
            return evaluate_term(*get<TypeHint>(term.value).operand, ctx);
        return Evaluated{Constant{Null{}}};
    }

    static Evaluated evaluate_function_call(const FunctionCall& call, const EvalContext& ctx) {
        DynamicArray<Evaluated> args{};
        for (U64 i = 0; i < call.arguments.length; i++)
            push_back(args, evaluate_term(call.arguments[i], ctx));
        FunctionEntry* fn = find_function(call.identifier);
        if (fn == nullptr) return Evaluated{Constant{Null{}}};
        return fn->fn(args, ctx);
    }

    Evaluated evaluate(const Term& term, const EvalContext& ctx) {
        return evaluate_term(term, ctx);
    }

    Evaluated evaluate(const Term& term) {
        return evaluate_term(term, EvalContext{});
    }
}

// ============================================================================
// plexdb::to_str for Evaluated
// ============================================================================
namespace plexdb {
    AutoString8 to_str(cql::Evaluated c, cql::type::Basic dtype) {
        if (!type_matches_tag<cql::Constant>(c.value)) return "@todo"_as;
        auto& con = get<cql::Constant>(c.value);

        if (type_matches_tag<cql::Null>(con.value)) return "null"_as;

        switch (dtype) {
            case cql::type::Basic::text:
            case cql::type::Basic::ascii:
            case cql::type::Basic::varchar:
                return get<AutoString8>(con.value);
            case cql::type::Basic::smallint:
                return to_str(static_cast<S64>(static_cast<S16>(get<S64>(con.value))));
            case cql::type::Basic::int_:
                return to_str(static_cast<S64>(static_cast<S32>(get<S64>(con.value))));
            case cql::type::Basic::counter:
            case cql::type::Basic::timestamp:
            case cql::type::Basic::bigint:
                return to_str(get<S64>(con.value));
            case cql::type::Basic::boolean:
                return get<bool>(con.value) ? "true"_as : "false"_as;
            case cql::type::Basic::float_:
                return to_str(static_cast<F32>(get<F64>(con.value)));
            case cql::type::Basic::double_:
                return to_str(get<F64>(con.value));
            case cql::type::Basic::tinyint:
            case cql::type::Basic::date:
            case cql::type::Basic::time:
                return to_str(get<S64>(con.value));
            case cql::type::Basic::uuid:
            case cql::type::Basic::timeuuid: {
                auto& uuid = get<cql::UUID>(con.value);
                const char hx[] = "0123456789abcdef";
                AutoString8 result{36};
                char* o = result.c_str;
                const U8* b = &uuid.value[0];
                for (int i = 0; i < 4; i++) { o[i*2]    = hx[b[i]>>4];    o[i*2+1]    = hx[b[i]&0xf]; }
                o[8] = '-';
                for (int i = 0; i < 2; i++) { o[9+i*2]  = hx[b[4+i]>>4];  o[10+i*2]   = hx[b[4+i]&0xf]; }
                o[13] = '-';
                for (int i = 0; i < 2; i++) { o[14+i*2] = hx[b[6+i]>>4];  o[15+i*2]   = hx[b[6+i]&0xf]; }
                o[18] = '-';
                for (int i = 0; i < 2; i++) { o[19+i*2] = hx[b[8+i]>>4];  o[20+i*2]   = hx[b[8+i]&0xf]; }
                o[23] = '-';
                for (int i = 0; i < 6; i++) { o[24+i*2] = hx[b[10+i]>>4]; o[25+i*2]   = hx[b[10+i]&0xf]; }
                return result;
            }
            case cql::type::Basic::blob:
            case cql::type::Basic::inet:
            case cql::type::Basic::varint:
            case cql::type::Basic::decimal:
            case cql::type::Basic::duration: {
                auto& blob = get<cql::Blob>(con.value);
                const char hx[] = "0123456789abcdef";
                AutoString8 hex{blob.value.length * 2};
                for (U64 i = 0; i < blob.value.length; i++) {
                    hex.c_str[i*2]     = hx[blob.value.ptr[i] >> 4];
                    hex.c_str[i*2 + 1] = hx[blob.value.ptr[i] & 0xf];
                }
                return "0x"_as + hex;
            }
            case cql::type::Basic::hex: {
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
