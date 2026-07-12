module;
#include <string.h>

module cql.engine.evaluator;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.column_value;
import cql.engine.io.evaluator;
import cql.engine.key;
import cql.engine.schema;
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

    static UUID make_timeuuid_from_ts_100ns(U64 timestamp, U16 clock_seq, const Array<U8, 6>& node) {
        UUID uuid{};
        uuid.value[0] = static_cast<U8>((timestamp >> 24) & 0xff_u64);
        uuid.value[1] = static_cast<U8>((timestamp >> 16) & 0xff_u64);
        uuid.value[2] = static_cast<U8>((timestamp >> 8) & 0xff_u64);
        uuid.value[3] = static_cast<U8>(timestamp & 0xff_u64);
        uuid.value[4] = static_cast<U8>((timestamp >> 40) & 0xff_u64);
        uuid.value[5] = static_cast<U8>((timestamp >> 32) & 0xff_u64);
        uuid.value[6] = static_cast<U8>(0x10_u8 | static_cast<U8>((timestamp >> 56) & 0x0f_u64));
        uuid.value[7] = static_cast<U8>((timestamp >> 48) & 0xff_u64);
        uuid.value[8] = static_cast<U8>(0x80_u8 | static_cast<U8>((clock_seq >> 8) & 0x3f_u16));
        uuid.value[9] = static_cast<U8>(clock_seq & 0xff_u16);
        for (U64 i = 0; i < 6; i++) {
            uuid.value[10 + i] = node.values[i];
        }
        return uuid;
    }

    static UUID make_timeuuid_from_unix_ms(S64 unix_ms, U16 clock_seq, const Array<U8, 6>& node) {
        return make_timeuuid_from_ts_100ns(unix_ms_to_uuid_timestamp_100ns(unix_ms), clock_seq, node);
    }

    static S64 timeuuid_to_unix_ms(const UUID& uuid) {
        U64 timestamp = 0;
        timestamp |= static_cast<U64>(uuid.value[0]) << 24;
        timestamp |= static_cast<U64>(uuid.value[1]) << 16;
        timestamp |= static_cast<U64>(uuid.value[2]) << 8;
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
        // Stable per-host node id (RFC 4122): every timeuuid this process emits shares it, so they
        // are attributable to this node and collision-free against a peer's.
        Array<U8, 6> node = os::get_node_id();

        Array<U8, 2> cs_entropy{};
        os::random_bytes(cs_entropy.values, 2);
        U16 clock_seq = static_cast<U16>(
                            static_cast<U16>(cs_entropy.values[0]) | (static_cast<U16>(cs_entropy.values[1]) << 8)
                        )
                      & 0x3fff_u16;
        // Cassandra guarantees consecutive now() values are strictly increasing (UUIDGen's per-tick
        // counter). Our clock is only millisecond-resolution, so two calls in the same millisecond
        // would otherwise share a timestamp and be ordered by the random clock_seq — making
        // `b < now()` a coin flip. Guard with a monotonic 100ns counter so each now() is strictly
        // greater than the last.
        //
        // @warn Single-node ordering only. The node id above makes timeuuids globally attributable
        // and collision-free, but strict cross-node *ordering* of now() would still need
        // synchronised clocks or a coordinator — `last_ts_100ns` orders calls within THIS process
        // only. Guard this the same way as USING TIMESTAMP (assert_true(engine.single_node, ...) in
        // engine.cpp) once the single_node flag reaches the function-evaluation path.
        static U64 last_ts_100ns = 0;
        U64        ts_100ns      = unix_ms_to_uuid_timestamp_100ns(static_cast<S64>(os::unix_ms_now()));
        if (ts_100ns <= last_ts_100ns) {
            ts_100ns = last_ts_100ns + 1;
        }
        last_ts_100ns = ts_100ns;
        return make_timeuuid_from_ts_100ns(ts_100ns, clock_seq, node);
    }

    static UUID min_timeuuid(S64 unix_ms) {
        Array<U8, 6> node{};
        return make_timeuuid_from_unix_ms(unix_ms, 0_u16, node);
    }

    static UUID max_timeuuid(S64 unix_ms) {
        Array<U8, 6> node{};
        for (U64 i = 0; i < 6; i++) {
            node.values[i] = 0xff_u8;
        }
        return make_timeuuid_from_unix_ms(unix_ms, 0x3fff_u16, node);
    }

    // ========================================================================
    // value extraction helpers
    // ========================================================================

    // Extract a typed value from the inner union of an Evaluated (searches both Literal.value
    // and ColumnValue). Returns nullptr if not found.
    template<typename T>
    static const T* as_value(const Evaluated& e) {
        const T* result = nullptr;
        visit(e.value, [&](const auto& top) {
            using TT = Decay<decltype(top)>;
            if constexpr (SameAs<TT, Literal>) {
                visit(top.value, [&](const auto& v) {
                    if constexpr (SameAs<Decay<decltype(v)>, T>) {
                        result = &v;
                    }
                });
            } else if constexpr (SameAs<TT, ColumnValue>) {
                visit(top, [&](const auto& v) {
                    if constexpr (SameAs<Decay<decltype(v)>, T>) {
                        result = &v;
                    }
                });
            }
        });
        return result;
    }

    // Look up a column by name and return its value as Evaluated.
    static Evaluated lookup_column_value(const AutoString8& col_name, const EvalContext& ctx) {
        if (!ctx.table || !ctx.row_values) {
            return Evaluated{Literal{Null{}}};
        }
        Optional<U64> ci = schema::find_column(*ctx.table, col_name);
        return ci ? Evaluated{ctx.row_values[*ci]} : Evaluated{Literal{Null{}}};
    }

    // ========================================================================
    // function registry
    // ========================================================================
    static DynamicArray<FunctionEntry>& builtin_function_registry() {
        static DynamicArray<FunctionEntry> registry{};
        static bool                        initialized = false;
        if (initialized) {
            return registry;
        }
        initialized = true;

        auto add = [&](AutoString8 name, Evaluated (*fn)(TArrayView<const Evaluated>, const EvalContext&)) {
            push_back(registry, FunctionEntry{move(name), fn});
        };

        add("uuid"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Literal{random_uuid_v4()}};
        });
        add("now"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Literal{now_timeuuid()}};
        });
        add("currenttimeuuid"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Literal{now_timeuuid()}};
        });
        add("currenttimestamp"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Literal{static_cast<S64>(os::unix_ms_now())}};
        });
        add("currentdate"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Literal{os::unix_days_now()}};
        });
        add("currenttime"_as, [](TArrayView<const Evaluated>, const EvalContext&) -> Evaluated {
            return Evaluated{Literal{os::unix_ns_since_midnight_now()}};
        });
        add("todate"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            if (const S64* ts = as_value<S64>(args[0])) {
                return Evaluated{Literal{static_cast<S64>(*ts / MS_PER_DAY)}};
            }
            if (const UUID* uuid = as_value<UUID>(args[0])) {
                return Evaluated{Literal{static_cast<S64>(timeuuid_to_unix_ms(*uuid) / MS_PER_DAY)}};
            }
            return Evaluated{Literal{Null{}}};
        });
        add("totimestamp"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            if (const S64* ts = as_value<S64>(args[0])) {
                return Evaluated{Literal{*ts}};
            }
            if (const UUID* uuid = as_value<UUID>(args[0])) {
                return Evaluated{Literal{timeuuid_to_unix_ms(*uuid)}};
            }
            return Evaluated{Literal{Null{}}};
        });
        add("tounixtimestamp"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            if (const S64* ts = as_value<S64>(args[0])) {
                return Evaluated{Literal{*ts}};
            }
            if (const UUID* uuid = as_value<UUID>(args[0])) {
                return Evaluated{Literal{timeuuid_to_unix_ms(*uuid)}};
            }
            return Evaluated{Literal{Null{}}};
        });
        add("mintimeuuid"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const S64* ts = as_value<S64>(args[0]);
            if (!ts) {
                return Evaluated{Literal{Null{}}};
            }
            return Evaluated{Literal{min_timeuuid(*ts)}};
        });
        add("maxtimeuuid"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const S64* ts = as_value<S64>(args[0]);
            if (!ts) {
                return Evaluated{Literal{Null{}}};
            }
            return Evaluated{Literal{max_timeuuid(*ts)}};
        });
        add("dateof"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const UUID* uuid = as_value<UUID>(args[0]);
            if (!uuid) {
                return Evaluated{Literal{Null{}}};
            }
            return Evaluated{Literal{timeuuid_to_unix_ms(*uuid)}};
        });
        add("unixtimestampof"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const UUID* uuid = as_value<UUID>(args[0]);
            if (!uuid) {
                return Evaluated{Literal{Null{}}};
            }
            return Evaluated{Literal{timeuuid_to_unix_ms(*uuid)}};
        });

        add("textasblob"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const AutoString8* s = as_value<AutoString8>(args[0]);
            if (!s) {
                return Evaluated{Literal{Null{}}};
            }
            Blob b{};
            resize(b.value, s->length);
            if (s->length > 0) {
                memcpy(b.value.ptr, s->c_str, s->length);
            }
            return Evaluated{Literal{move(b)}};
        });
        add("blobastext"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            if (const Blob* b = as_value<Blob>(args[0])) {
                AutoString8 s(reinterpret_cast<const U8*>(b->value.ptr), b->value.length);
                return Evaluated{Literal{move(s)}};
            }
            if (const Hex* h = as_value<Hex>(args[0])) {
                AutoString8 s(reinterpret_cast<const U8*>(h->value.ptr), h->value.length);
                return Evaluated{Literal{move(s)}};
            }
            return Evaluated{Literal{Null{}}};
        });
        add("intasblob"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const S64* v = as_value<S64>(args[0]);
            if (!v) {
                return Evaluated{Literal{Null{}}};
            }
            Blob b{};
            resize(b.value, 4_u64);
            U32 be = __builtin_bswap32(static_cast<U32>(*v));
            memcpy(b.value.ptr, &be, 4);
            return Evaluated{Literal{move(b)}};
        });
        add("blobasint"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const Blob* b = as_value<Blob>(args[0]);
            if (!b || b->value.length != 4) {
                return Evaluated{Literal{Null{}}};
            }
            U32 be;
            memcpy(&be, b->value.ptr, 4);
            return Evaluated{Literal{S64(static_cast<S32>(__builtin_bswap32(be)))}};
        });
        add("bigintasblob"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const S64* v = as_value<S64>(args[0]);
            if (!v) {
                return Evaluated{Literal{Null{}}};
            }
            Blob b{};
            resize(b.value, 8_u64);
            U64 be = __builtin_bswap64(static_cast<U64>(*v));
            memcpy(b.value.ptr, &be, 8);
            return Evaluated{Literal{move(b)}};
        });
        add("blobasbigint"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) {
                return Evaluated{Literal{Null{}}};
            }
            const Blob* b = as_value<Blob>(args[0]);
            if (!b || b->value.length != 8) {
                return Evaluated{Literal{Null{}}};
            }
            U64 be;
            memcpy(&be, b->value.ptr, 8);
            return Evaluated{Literal{S64(__builtin_bswap64(be))}};
        });

        add("token"_as, [](TArrayView<const Evaluated> args, const EvalContext& ctx) -> Evaluated {
            if (!ctx.table || args.length != ctx.table->partition_key_col_indices.length) {
                return Evaluated{Literal{Null{}}};
            }
            return Evaluated{Literal{key::compute_partition_token_from_evals(*ctx.table, args)}};
        });

        return registry;
    }

    static FunctionEntry* find_function(const AutoString8& identifier) {
        AutoString8 name = identifier;
        to_lowercase_inplace(name);
        auto& registry = builtin_function_registry();
        for (U64 i = 0; i < registry.length; i++) {
            if (registry[i].name == name && registry[i].fn != nullptr) {
                return &registry[i];
            }
        }
        return nullptr;
    }

    Evaluated call_registered_function(String8 name, TArrayView<const Evaluated> args, const EvalContext& ctx) {
        FunctionEntry* fn = find_function(AutoString8(name));
        if (fn == nullptr) {
            return Evaluated{Literal{Null{}}};
        }
        return fn->fn(args, ctx);
    }

    bool registered_function_exists(String8 name) {
        return find_function(AutoString8(name)) != nullptr;
    }

    template<typename T>
    static Optional<type::Basic> outer_type_hint_basic_impl(const T& term) {
        return visit(term.value, [](const auto& v) -> Optional<type::Basic> {
            using TT = Decay<decltype(v)>;
            if constexpr (SameAs<TT, TypeHint>) {
                if (type_matches_tag<type::Basic>(v.type.value)) {
                    return Optional<type::Basic>{get<type::Basic>(v.type.value)};
                }
            }
            return Optional<type::Basic>{};
        });
    }

    Optional<type::Basic> outer_type_hint_basic(const Term& term) {
        return outer_type_hint_basic_impl(term);
    }
    Optional<type::Basic> outer_type_hint_basic(const TermWithIdentifiers& twi) {
        return outer_type_hint_basic_impl(twi);
    }

    // ========================================================================
    // bind marker resolution
    // ========================================================================
    static const Literal* lookup_positional_binding(const AutoString8& identifier, const EvalContext& ctx) {
        if (identifier.length == 0) {
            return nullptr;
        }
        if (identifier.c_str[0] == ':') {
            return nullptr;
        }
        U64 index = 0;
        for (U64 i = 0; i < identifier.length; i++) {
            if (identifier.c_str[i] < '0' || identifier.c_str[i] > '9') {
                return nullptr;
            }
            index = index * 10_u64 + static_cast<U64>(identifier.c_str[i] - '0');
        }
        if (index >= ctx.positional_bindings.length) {
            return nullptr;
        }
        return &ctx.positional_bindings[index];
    }

    static const Literal* lookup_named_binding(const AutoString8& identifier, const EvalContext& ctx) {
        if (identifier.length == 0) {
            return nullptr;
        }
        AutoString8 key = identifier;
        if (key.c_str[0] == ':') {
            AutoString8 stripped{key.length - 1};
            for (U64 i = 0; i < stripped.length; i++) {
                stripped.c_str[i] = key.c_str[i + 1];
            }
            key = move(stripped);
        }
        to_lowercase_inplace(key);
        for (U64 i = 0; i < ctx.named_bindings.length; i++) {
            AutoString8 name = ctx.named_bindings[i].first;
            to_lowercase_inplace(name);
            if (name == key) {
                return &ctx.named_bindings[i].second;
            }
        }
        return nullptr;
    }

    // ========================================================================
    // term evaluation
    // ========================================================================
    static Evaluated evaluate_term(const Term& term, const EvalContext& ctx);
    static Evaluated evaluate_toi(const TermWithIdentifiers& twi, const EvalContext& ctx);
    static Evaluated evaluate_function_call(const FunctionCall& call, const EvalContext& ctx);
    static bool      as_s64(const Evaluated& e, S64& out);
    static bool      as_f64(const Evaluated& e, F64& out);
    static bool      eval_is_null(const Evaluated& e);

    static bool eval_is_float_like(const Evaluated& e) {
        return as_value<F64>(e) != nullptr || as_value<F32>(e) != nullptr;
    }

    static bool eval_is_unset(const Evaluated& e) {
        return as_value<Unset>(e) != nullptr;
    }

    static Evaluated evaluate_binary_arithmetic(const Evaluated& lhs, ArithmeticOperator op, const Evaluated& rhs) {
        if (eval_is_unset(lhs) || eval_is_unset(rhs)) {
            return Evaluated{Literal{Unset{}}};
        }
        if (eval_is_null(lhs) || eval_is_null(rhs)) {
            return Evaluated{Literal{Null{}}};
        }

        const AutoString8* ls = as_value<AutoString8>(lhs);
        const AutoString8* rs = as_value<AutoString8>(rhs);
        if (ls && rs) {
            if (op != ArithmeticOperator::plus) {
                return Evaluated{Literal{Null{}}};
            }
            return Evaluated{Literal{*ls + *rs}};
        }

        if (eval_is_float_like(lhs) || eval_is_float_like(rhs)) {
            if (op == ArithmeticOperator::mod) {
                return Evaluated{Literal{Null{}}};
            }
            F64 l, r;
            if (!as_f64(lhs, l) || !as_f64(rhs, r)) {
                return Evaluated{Literal{Null{}}};
            }
            switch (op) {
                case ArithmeticOperator::plus:
                    return Evaluated{Literal{l + r}};
                case ArithmeticOperator::minus:
                    return Evaluated{Literal{l - r}};
                case ArithmeticOperator::times:
                    return Evaluated{Literal{l * r}};
                case ArithmeticOperator::divide:
                    return Evaluated{Literal{l / r}};
                case ArithmeticOperator::mod:
                    break;
            }
            return Evaluated{Literal{Null{}}};
        }

        S64 l, r;
        if (!as_s64(lhs, l) || !as_s64(rhs, r)) {
            return Evaluated{Literal{Null{}}};
        }
        if ((op == ArithmeticOperator::divide || op == ArithmeticOperator::mod) && r == 0) {
            return Evaluated{Literal{Null{}}};
        }
        switch (op) {
            case ArithmeticOperator::plus:
                return Evaluated{Literal{l + r}};
            case ArithmeticOperator::minus:
                return Evaluated{Literal{l - r}};
            case ArithmeticOperator::times:
                return Evaluated{Literal{l * r}};
            case ArithmeticOperator::divide:
                return Evaluated{Literal{l / r}};
            case ArithmeticOperator::mod:
                return Evaluated{Literal{l % r}};
        }
        return Evaluated{Literal{Null{}}};
    }

    static Evaluated evaluate_unary_minus(const Evaluated& value) {
        if (eval_is_unset(value)) {
            return Evaluated{Literal{Unset{}}};
        }
        if (eval_is_null(value)) {
            return Evaluated{Literal{Null{}}};
        }
        if (eval_is_float_like(value)) {
            F64 v;
            if (!as_f64(value, v)) {
                return Evaluated{Literal{Null{}}};
            }
            return Evaluated{Literal{-v}};
        }
        S64 v;
        if (!as_s64(value, v)) {
            return Evaluated{Literal{Null{}}};
        }
        return Evaluated{Literal{-v}};
    }

    // Shared handler for all term variants that are identical between Term and TermWithIdentifiers.
    // Delegates arithmetic/function-call recursion through evaluate_term (TWI operands are always Term).
    template<typename V>
    static Evaluated evaluate_term_content(const V& v, const EvalContext& ctx) {
        using T = Decay<V>;
        if constexpr (SameAs<T, Literal>) {
            return {v};
        } else if constexpr (SameAs<T, BindMarker>) {
            if (const Literal* c = lookup_positional_binding(v.identifier, ctx)) {
                return {*c};
            }
            if (const Literal* c = lookup_named_binding(v.identifier, ctx)) {
                return {*c};
            }
            return Evaluated{Literal{Null{}}};
        } else if constexpr (SameAs<T, MapLiteral> || SameAs<T, SetLiteral> || SameAs<T, ListOrVectorLiteral> || SameAs<T, UdtLiteral> || SameAs<T, TupleLiteral>) {
            return {v};
        } else if constexpr (SameAs<T, FunctionCall>) {
            return evaluate_function_call(v, ctx);
        } else if constexpr (SameAs<T, ArithmeticOperation>) {
            return visit(v.value, [&](const auto& arith_v) -> Evaluated {
                using AT = Decay<decltype(arith_v)>;
                if constexpr (SameAs<AT, UnaryMinusArithmeticOperation>) {
                    return evaluate_unary_minus(evaluate_term(arith_v.operand, ctx));
                } else if constexpr (SameAs<AT, BinaryArithmeticOperation>) {
                    return evaluate_binary_arithmetic(evaluate_term(arith_v.lhs, ctx), arith_v.op, evaluate_term(arith_v.rhs, ctx));
                } else {
                    static_assert(!SameAs<AT, AT>, "missing ArithmeticOperation case");
                    return Evaluated{Literal{Null{}}};
                }
            });
        } else if constexpr (SameAs<T, TypeHint>) {
            return evaluate_term(v.operand, ctx);
        } else if constexpr (SameAs<T, ColumnValue>) {
            return Evaluated{v};
        } else {
            static_assert(!SameAs<T, T>, "evaluate_term_content missing Term variant case");
            return Evaluated{Literal{Null{}}};
        }
    }

    static Evaluated evaluate_term(const Term& term, const EvalContext& ctx) {
        return visit(term.value, [&](const auto& v) -> Evaluated {
            return evaluate_term_content(v, ctx);
        });
    }

    static Evaluated evaluate_function_call(const FunctionCall& call, const EvalContext& ctx) {
        DynamicArray<Evaluated> args{};
        for (U64 i = 0; i < call.arguments.length; i++) {
            push_back(args, evaluate_term(call.arguments[i], ctx));
        }
        FunctionEntry* fn = find_function(call.identifier);
        if (fn == nullptr) {
            return Evaluated{Literal{Null{}}};
        }
        return fn->fn(args, ctx);
    }

    static Evaluated evaluate_toi(const TermWithIdentifiers& twi, const EvalContext& ctx) {
        return visit(twi.value, [&](const auto& v) -> Evaluated {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, AutoString8>) {
                return lookup_column_value(v, ctx);
            } else if constexpr (SameAs<T, TOIArithmeticOperation>) {
                return visit(v.value, [&](const auto& arith_v) -> Evaluated {
                    using AT = Decay<decltype(arith_v)>;
                    if constexpr (SameAs<AT, TOIUnaryMinus>) {
                        return evaluate_unary_minus(evaluate_toi(arith_v.operand, ctx));
                    } else if constexpr (SameAs<AT, TOIBinaryArithmetic>) {
                        return evaluate_binary_arithmetic(evaluate_toi(arith_v.lhs, ctx), arith_v.op, evaluate_toi(arith_v.rhs, ctx));
                    } else {
                        static_assert(!SameAs<AT, AT>, "missing TOIArithmeticOperation case");
                        return Evaluated{Literal{Null{}}};
                    }
                });
            } else {
                return evaluate_term_content(v, ctx);
            }
        });
    }

    Evaluated evaluate(const Term& term, const EvalContext& ctx) {
        return evaluate_term(term, ctx);
    }
    Evaluated evaluate(const Term& term) {
        return evaluate_term(term, EvalContext{});
    }
    Evaluated evaluate(const TermWithIdentifiers& twi, const EvalContext& ctx) {
        return evaluate_toi(twi, ctx);
    }

    // ========================================================================
    // predicate helpers
    // ========================================================================
    static bool as_s64(const Evaluated& e, S64& out) {
        bool found = false;
        visit(e.value, [&](const auto& top) {
            using T = Decay<decltype(top)>;
            if constexpr (SameAs<T, Literal>) {
                visit(top.value, [&](const auto& v) {
                    using V = Decay<decltype(v)>;
                    if constexpr (SameAs<V, S64>) {
                        out   = v;
                        found = true;
                    } else if constexpr (SameAs<V, bool>) {
                        out   = v ? 1 : 0;
                        found = true;
                    }
                });
            } else if constexpr (SameAs<T, ColumnValue>) {
                visit(top, [&](const auto& cv) {
                    using V = Decay<decltype(cv)>;
                    if constexpr (SameAs<V, S64>) {
                        out   = cv;
                        found = true;
                    } else if constexpr (SameAs<V, S32>) {
                        out   = S64(cv);
                        found = true;
                    } else if constexpr (SameAs<V, S16>) {
                        out   = S64(cv);
                        found = true;
                    } else if constexpr (SameAs<V, U8>) {
                        out   = S64(cv);
                        found = true;
                    }
                });
            }
        });
        return found;
    }

    static bool as_f64(const Evaluated& e, F64& out) {
        bool found = false;
        visit(e.value, [&](const auto& top) {
            using T = Decay<decltype(top)>;
            if constexpr (SameAs<T, Literal>) {
                visit(top.value, [&](const auto& v) {
                    using V = Decay<decltype(v)>;
                    if constexpr (SameAs<V, F64>) {
                        out   = v;
                        found = true;
                    } else if constexpr (SameAs<V, S64>) {
                        out   = static_cast<F64>(v);
                        found = true;
                    }
                });
            } else if constexpr (SameAs<T, ColumnValue>) {
                visit(top, [&](const auto& cv) {
                    using V = Decay<decltype(cv)>;
                    if constexpr (SameAs<V, F64>) {
                        out   = cv;
                        found = true;
                    } else if constexpr (SameAs<V, F32>) {
                        out   = F64(cv);
                        found = true;
                    } else if constexpr (SameAs<V, S64>) {
                        out   = static_cast<F64>(cv);
                        found = true;
                    } else if constexpr (SameAs<V, S32>) {
                        out   = static_cast<F64>(cv);
                        found = true;
                    }
                });
            }
        });
        return found;
    }

    static bool eval_is_null(const Evaluated& e) {
        return as_value<Null>(e) != nullptr;
    }

    static S64 compare_evaluated(const Evaluated& lhs, const Evaluated& rhs, bool& comparable) {
        comparable = true;
        if (eval_is_null(lhs) || eval_is_null(rhs)) {
            comparable = false;
            return 0;
        }

        S64 l_int, r_int;
        if (as_s64(lhs, l_int) && as_s64(rhs, r_int)) {
            return l_int < r_int ? -1 : (l_int > r_int ? 1 : 0);
        }

        F64 l_flt, r_flt;
        if (as_f64(lhs, l_flt) && as_f64(rhs, r_flt)) {
            return l_flt < r_flt ? -1 : (l_flt > r_flt ? 1 : 0);
        }

        const AutoString8* ls = as_value<AutoString8>(lhs);
        const AutoString8* rs = as_value<AutoString8>(rhs);
        if (ls && rs) {
            U64 min_len = ls->length < rs->length ? ls->length : rs->length;
            for (U64 i = 0; i < min_len; i++) {
                if (ls->c_str[i] != rs->c_str[i]) {
                    return static_cast<U8>(ls->c_str[i]) < static_cast<U8>(rs->c_str[i]) ? -1 : 1;
                }
            }
            return ls->length < rs->length ? -1 : (ls->length > rs->length ? 1 : 0);
        }

        const UUID* lu = as_value<UUID>(lhs);
        const UUID* ru = as_value<UUID>(rhs);
        if (lu && ru) {
            for (U64 i = 0; i < UUID::length; i++) {
                if (lu->value[i] != ru->value[i]) {
                    return lu->value[i] < ru->value[i] ? -1 : 1;
                }
            }
            return 0;
        }

        comparable = false;
        return 0;
    }

    // @todo `contains` / `contains_key` residual evaluation: return false today so unindexed
    static bool apply_operator(S64 cmp, Operator op) {
        switch (op) {
            case Operator::eq:
                return cmp == 0;
            case Operator::ne:
                return cmp != 0;
            case Operator::lt:
                return cmp < 0;
            case Operator::le:
                return cmp <= 0;
            case Operator::gt:
                return cmp > 0;
            case Operator::ge:
                return cmp >= 0;
            case Operator::in:
                assert_not_implemented("apply_operator reached with IN; should be dispatched earlier");
                return false;
            case Operator::contains:
            case Operator::contains_key:
                return false;
        }
        assert_true(false, "unhandled Operator in apply_operator");
        return false;
    }

    // @note CONTAINS scans element values; CONTAINS KEY scans only the key half of a map.
    static bool evaluate_contains(const Evaluated& lhs, const Evaluated& rhs, bool match_keys) {
        if (!type_matches_tag<ColumnValue>(lhs.value)) {
            return false;
        }
        const ColumnValue& cv = get<ColumnValue>(lhs.value);
        return visit(cv, [&](const auto& col) -> bool {
            using T = RemoveCVRef<decltype(col)>;
            if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                if (match_keys) {
                    return false; // CONTAINS KEY is invalid on list/vector
                }
                for (const auto& e : col) {
                    bool comparable = false;
                    S64  cmp        = compare_evaluated(Evaluated{e.value}, rhs, comparable);
                    if (comparable && cmp == 0) {
                        return true;
                    }
                }
                return false;
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                if (match_keys) {
                    return false;
                }
                for (auto it = col.begin(); it != col.end(); ++it) {
                    bool comparable = false;
                    S64  cmp        = compare_evaluated(Evaluated{(*it).value}, rhs, comparable);
                    if (comparable && cmp == 0) {
                        return true;
                    }
                }
                return false;
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                for (auto it = col.begin(); it != col.end(); ++it) {
                    const NestedColumnValue& side       = match_keys ? (*it).first : (*it).second;
                    bool                     comparable = false;
                    S64                      cmp        = compare_evaluated(Evaluated{side.value}, rhs, comparable);
                    if (comparable && cmp == 0) {
                        return true;
                    }
                }
                return false;
            } else {
                return false; // CONTAINS on non-collection column type — caller already rejected at plan time.
            }
        });
    }

    static bool evaluate_column_relation(const WhereClause::ColumnExpressionRelation& cer, const EvalContext& ctx) {
        Optional<U64> col_ci = ctx.table ? schema::find_column(*ctx.table, cer.column.identifier) : Optional<U64>{};
        Evaluated     lhs    = (col_ci && ctx.row_values) ? Evaluated{ctx.row_values[*col_ci]} : Evaluated{Literal{Null{}}};

        if (cer.operator_ == Operator::contains || cer.operator_ == Operator::contains_key) {
            if (eval_is_null(lhs)) {
                return false;
            }
            Evaluated rhs = evaluate_term(cer.value, ctx);
            if (eval_is_null(rhs)) {
                return false;
            }
            return evaluate_contains(lhs, rhs, cer.operator_ == Operator::contains_key);
        }

        if (cer.operator_ == Operator::in) {
            // Check membership: rhs is a TupleLiteral or ListOrVectorLiteral.
            Evaluated rhs     = evaluate_term(cer.value, ctx);
            bool      matched = visit(rhs.value, [&](const auto& list) -> bool {
                using LT = RemoveCVRef<decltype(list)>;
                if constexpr (SameAs<LT, TupleLiteral> || SameAs<LT, ListOrVectorLiteral>) {
                    for (const Term& elem : list.elements) {
                        Evaluated elem_val = evaluate_term(elem, ctx);
                        bool      comparable;
                        S64       cmp = compare_evaluated(lhs, elem_val, comparable);
                        if (comparable && cmp == 0) {
                            return true;
                        }
                    }
                    return false;
                }
                // Single bound value (e.g. IN ?)
                bool comparable;
                S64  cmp = compare_evaluated(lhs, rhs, comparable);
                return comparable && cmp == 0;
            });
            return matched;
        }

        Evaluated rhs = evaluate_term(cer.value, ctx);

        // Type-aware path: reuses compare_column_value (Cassandra's AbstractType.compare per
        // dtype — the same rules clustering/index keys sort by) rather than re-implementing
        // per-type comparison here. This gets uuid/timeuuid/varint/decimal ordering right,
        // which the untyped fallback below cannot. Only engages when the rhs term resolves
        // cleanly to the column's own dtype; anything it can't resolve (implicit numeric
        // widening, cross-type literals, etc.) falls through unchanged.
        if (type_matches_tag<ColumnValue>(lhs.value)) {
            const ColumnValue& lhs_cv = get<ColumnValue>(lhs.value);
            if (!type_matches_tag<Null>(lhs_cv) && type_matches_tag<type::Basic>(ctx.table->cols[*col_ci].type.value)) {
                type::Basic           dtype = get<type::Basic>(ctx.table->cols[*col_ci].type.value);
                Optional<ColumnValue> rhs_cv;
                if (type_matches_tag<Literal>(rhs.value)) {
                    rhs_cv = io::resolve_literal_scalar(get<Literal>(rhs.value), type::Type{dtype});
                } else if (type_matches_tag<ColumnValue>(rhs.value)) {
                    rhs_cv = get<ColumnValue>(rhs.value);
                }
                if (rhs_cv.has_value() && !type_matches_tag<Null>(*rhs_cv)) {
                    S32 cmp = compare_column_value(lhs_cv, *rhs_cv, dtype);
                    return apply_operator(cmp, cer.operator_);
                }
            }
        }

        bool comparable;
        S64  cmp = compare_evaluated(lhs, rhs, comparable);
        if (!comparable) {
            return false;
        }
        return apply_operator(cmp, cer.operator_);
    }

    bool evaluate_where(TArrayView<const WhereClause::Relation> predicates, const EvalContext& ctx) {
        for (U64 i = 0; i < predicates.length; i++) {
            bool passes = visit(predicates[i].value, [&](const auto& value) -> bool {
                using T = Decay<decltype(value)>;
                if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                    return evaluate_column_relation(value, ctx);
                } else if constexpr (SameAs<T, WhereClause::TupleExpressionRelation>) {
                    if (value.operator_ == Operator::in) {
                        // (col1 [, col2]) IN ((v0a [, v0b]), (v1a [, v1b]))
                        // Each value-tuple in value.values must be compared against row columns.
                        for (const Term& val_term : value.values) {
                            bool tuple_matches = visit(val_term.value, [&](const auto& v) -> bool {
                                using VT = RemoveCVRef<decltype(v)>;
                                if constexpr (SameAs<VT, TupleLiteral>) {
                                    for (U64 ci = 0; ci < v.elements.length && ci < value.columns.length; ci++) {
                                        Evaluated lhs  = lookup_column_value(value.columns[ci].identifier, ctx);
                                        Evaluated rhs  = evaluate_term(v.elements[ci], ctx);
                                        bool      comp = false;
                                        if (compare_evaluated(lhs, rhs, comp) != 0 || !comp) {
                                            return false;
                                        }
                                    }
                                    return true;
                                } else {
                                    if (value.columns.length == 0) {
                                        return false;
                                    }
                                    Evaluated lhs  = lookup_column_value(value.columns[0].identifier, ctx);
                                    Evaluated rhs  = evaluate_term(val_term, ctx);
                                    bool      comp = false;
                                    return compare_evaluated(lhs, rhs, comp) == 0 && comp;
                                }
                            });
                            if (tuple_matches) {
                                return true;
                            }
                        }
                        return false;
                    }
                    if (!tuple_rhs_is_compatible(value)) {
                        return true;
                    }
                    if (value.operator_ == Operator::eq) {
                        for (U64 ci = 0; ci < value.columns.length; ci++) {
                            Evaluated lhs  = lookup_column_value(value.columns[ci].identifier, ctx);
                            Evaluated rhs  = evaluate_term(tuple_value_at(value, ci), ctx);
                            bool      comp = false;
                            if (compare_evaluated(lhs, rhs, comp) != 0 || !comp) {
                                return false;
                            }
                        }
                        return true;
                    }
                    if (is_inequality(value.operator_)) {
                        for (U64 ci = 0; ci < value.columns.length; ci++) {
                            Evaluated lhs        = lookup_column_value(value.columns[ci].identifier, ctx);
                            Evaluated rhs        = evaluate_term(tuple_value_at(value, ci), ctx);
                            bool      comparable = false;
                            S64       cmp        = compare_evaluated(lhs, rhs, comparable);
                            if (!comparable) {
                                return false;
                            }
                            if (cmp == 0) {
                                continue;
                            }
                            return apply_operator(cmp, value.operator_);
                        }
                        // all components equal: pass on le/ge, fail on lt/gt
                        return value.operator_ == Operator::le || value.operator_ == Operator::ge;
                    }
                    return true;
                } else if constexpr (SameAs<T, WhereClause::TokenRelation>) {
                    if (!ctx.table || !ctx.row_values) {
                        return true;
                    }
                    TArrayView<const ColumnValue, U64> row_view{ctx.row_values, ctx.table->cols.length};
                    S64                                lhs_tok = key::compute_partition_token(*ctx.table, row_view);
                    Evaluated                          lhs{Literal{lhs_tok}};
                    Evaluated                          rhs        = evaluate_term(value.value, ctx);
                    bool                               comparable = false;
                    S64                                cmp        = compare_evaluated(lhs, rhs, comparable);
                    if (!comparable) {
                        return false;
                    }
                    return apply_operator(cmp, value.operator_);
                } else if constexpr (SameAs<T, WhereClause::SubscriptedRelation>) {
                    // Look up the map column, fetch m[k], compare to v.
                    Evaluated map_eval = lookup_column_value(value.column.identifier, ctx);
                    if (!type_matches_tag<ColumnValue>(map_eval.value)) {
                        return false;
                    }
                    const ColumnValue& cv = get<ColumnValue>(map_eval.value);
                    if (!type_matches_tag<DynamicMap<NestedColumnValue, NestedColumnValue>>(cv)) {
                        return false;
                    }
                    const auto& dm       = get<DynamicMap<NestedColumnValue, NestedColumnValue>>(cv);
                    Evaluated   key_eval = evaluate_term(value.subscript, ctx);
                    Evaluated   rhs      = evaluate_term(value.value, ctx);
                    for (auto it = dm.begin(); it != dm.end(); ++it) {
                        const auto& entry     = *it;
                        Evaluated   entry_key = Evaluated{ColumnValue{entry.first.value}};
                        bool        key_comp  = false;
                        if (compare_evaluated(entry_key, key_eval, key_comp) == 0 && key_comp) {
                            Evaluated entry_val = Evaluated{ColumnValue{entry.second.value}};
                            bool      cmp_ok    = false;
                            S64       cmp       = compare_evaluated(entry_val, rhs, cmp_ok);
                            if (!cmp_ok) {
                                return false;
                            }
                            return apply_operator(cmp, value.operator_);
                        }
                    }
                    return false;
                } else {
                    static_assert(!SameAs<T, T>, "missing WHERE relation type");
                    return false;
                }
            });
            if (!passes) {
                return false;
            }
        }
        return true;
    }

    // ========================================================================
    // bind marker numbering
    // ========================================================================
    static U64 number_bind_markers_in_term(Term& term, U64 idx) {
        visit(term.value, [&](auto& v) {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, BindMarker>) {
                if (v.identifier.length == 0) {
                    v.identifier = to_str(idx++);
                }
            } else if constexpr (SameAs<T, ArithmeticOperation>) {
                visit(v.value, [&](auto& arith_v) {
                    using AT = Decay<decltype(arith_v)>;
                    if constexpr (SameAs<AT, BinaryArithmeticOperation>) {
                        idx = number_bind_markers_in_term(arith_v.lhs, idx);
                        idx = number_bind_markers_in_term(arith_v.rhs, idx);
                    } else if constexpr (SameAs<AT, UnaryMinusArithmeticOperation>) {
                        idx = number_bind_markers_in_term(arith_v.operand, idx);
                    }
                });
            } else if constexpr (SameAs<T, FunctionCall>) {
                for (U64 i = 0; i < v.arguments.length; i++) {
                    idx = number_bind_markers_in_term(v.arguments[i], idx);
                }
            } else if constexpr (SameAs<T, TypeHint>) {
                idx = number_bind_markers_in_term(v.operand, idx);
            }
        });
        return idx;
    }

    void number_bind_markers(Statement& stmt) {
        U64 idx = 0;
        visit(stmt.value, [&](auto& s) {
            using T = RemoveCVRef<decltype(s)>;
            if constexpr (SameAs<T, Insert>) {
                visit(s.insert_clause, [&](auto& clause) {
                    using CT = RemoveCVRef<decltype(clause)>;
                    if constexpr (SameAs<CT, Insert::NamesValues>) {
                        for (U64 i = 0; i < clause.values.length; i++) {
                            idx = number_bind_markers_in_term(clause.values[i], idx);
                        }
                    }
                });
            }
            // @todo Update, Delete, Select WHERE clause
        });
    }
}
