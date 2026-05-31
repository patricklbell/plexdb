module cql.engine.evaluator;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

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
    // value extraction helpers
    // ========================================================================

    // Extract a typed value from the inner union of an Evaluated (searches both Constant.value
    // and ColumnValue). Returns nullptr if not found.
    template<typename T>
    static const T* as_value(const Evaluated& e) {
        const T* result = nullptr;
        visit(e.value, [&](const auto& top) {
            using TT = Decay<decltype(top)>;
            if constexpr (SameAs<TT, Constant>) {
                visit(top.value, [&](const auto& v) {
                    if constexpr (SameAs<Decay<decltype(v)>, T>) result = &v;
                });
            } else if constexpr (SameAs<TT, ColumnValue>) {
                visit(top, [&](const auto& v) {
                    if constexpr (SameAs<Decay<decltype(v)>, T>) result = &v;
                });
            }
        });
        return result;
    }

    // Extract top-level Constant from Evaluated.
    static const Constant* get_if_constant(const Evaluated& e) {
        return visit(e.value, [](const auto& v) -> const Constant* {
            if constexpr (SameAs<Decay<decltype(v)>, Constant>) return &v;
            else return nullptr;
        });
    }

    // Look up a column by name and return its value as Evaluated.
    static Evaluated lookup_column_value(const AutoString8& col_name, const EvalContext& ctx) {
        if (!ctx.table || !ctx.row_values) return Evaluated{Constant{Null{}}};
        for (U64 ci = 0; ci < ctx.table->cols.length; ci++) {
            if (ctx.table->cols[ci].name == String8(col_name.c_str, col_name.length))
                return Evaluated{ctx.row_values[ci]};
        }
        return Evaluated{Constant{Null{}}};
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
            if (args.length != 1) return Evaluated{Constant{Null{}}};
            if (const S64* ts = as_value<S64>(args[0]))
                return Evaluated{Constant{static_cast<S64>(*ts / MS_PER_DAY)}};
            if (const UUID* uuid = as_value<UUID>(args[0]))
                return Evaluated{Constant{static_cast<S64>(timeuuid_to_unix_ms(*uuid) / MS_PER_DAY)}};
            return Evaluated{Constant{Null{}}};
        });
        add("totimestamp"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) return Evaluated{Constant{Null{}}};
            if (const S64* ts = as_value<S64>(args[0]))  return Evaluated{Constant{*ts}};
            if (const UUID* uuid = as_value<UUID>(args[0])) return Evaluated{Constant{timeuuid_to_unix_ms(*uuid)}};
            return Evaluated{Constant{Null{}}};
        });
        add("tounixtimestamp"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) return Evaluated{Constant{Null{}}};
            if (const S64* ts = as_value<S64>(args[0]))  return Evaluated{Constant{*ts}};
            if (const UUID* uuid = as_value<UUID>(args[0])) return Evaluated{Constant{timeuuid_to_unix_ms(*uuid)}};
            return Evaluated{Constant{Null{}}};
        });
        add("mintimeuuid"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) return Evaluated{Constant{Null{}}};
            const S64* ts = as_value<S64>(args[0]);
            if (!ts) return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{min_timeuuid(*ts)}};
        });
        add("maxtimeuuid"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) return Evaluated{Constant{Null{}}};
            const S64* ts = as_value<S64>(args[0]);
            if (!ts) return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{max_timeuuid(*ts)}};
        });
        add("dateof"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) return Evaluated{Constant{Null{}}};
            const UUID* uuid = as_value<UUID>(args[0]);
            if (!uuid) return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{timeuuid_to_unix_ms(*uuid)}};
        });
        add("unixtimestampof"_as, [](TArrayView<const Evaluated> args, const EvalContext&) -> Evaluated {
            if (args.length != 1) return Evaluated{Constant{Null{}}};
            const UUID* uuid = as_value<UUID>(args[0]);
            if (!uuid) return Evaluated{Constant{Null{}}};
            return Evaluated{Constant{timeuuid_to_unix_ms(*uuid)}};
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

    // ========================================================================
    // term evaluation
    // ========================================================================
    static Evaluated evaluate_term(const Term& term, const EvalContext& ctx);
    static Evaluated evaluate_toi(const TermWithIdentifiers& twi, const EvalContext& ctx);
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

    // Shared handler for all term variants that are identical between Term and TermWithIdentifiers.
    // Delegates arithmetic/function-call recursion through evaluate_term (TWI operands are always Term).
    template<typename V>
    static Evaluated evaluate_term_content(const V& v, const EvalContext& ctx) {
        using T = Decay<V>;
        if constexpr (SameAs<T, Constant>) {
            return {v};
        } else if constexpr (SameAs<T, BindMarker>) {
            if (const Constant* c = lookup_positional_binding(v.identifier, ctx)) return {*c};
            if (const Constant* c = lookup_named_binding(v.identifier, ctx))      return {*c};
            return Evaluated{Constant{Null{}}};
        } else if constexpr (SameAs<T, MapLiteral>  || SameAs<T, SetLiteral>    ||
                             SameAs<T, ListOrVectorLiteral> || SameAs<T, UdtLiteral> ||
                             SameAs<T, TupleLiteral>) {
            return {v};
        } else if constexpr (SameAs<T, FunctionCall>) {
            return evaluate_function_call(v, ctx);
        } else if constexpr (SameAs<T, ArithmeticOperation>) {
            return visit(v.value, [&](const auto& arith_v) -> Evaluated {
                using AT = Decay<decltype(arith_v)>;
                if constexpr (SameAs<AT, UnaryMinusArithmeticOperation>) {
                    Evaluated operand = evaluate_term(arith_v.operand, ctx);
                    const Constant* c = get_if_constant(operand);
                    if (!c) return Evaluated{Constant{Null{}}};
                    return evaluate_unary_minus(*c);
                } else if constexpr (SameAs<AT, BinaryArithmeticOperation>) {
                    Evaluated lhs = evaluate_term(arith_v.lhs, ctx);
                    Evaluated rhs = evaluate_term(arith_v.rhs, ctx);
                    const Constant* lc = get_if_constant(lhs);
                    const Constant* rc = get_if_constant(rhs);
                    if (!lc || !rc) return Evaluated{Constant{Null{}}};
                    return evaluate_binary_arithmetic(*lc, arith_v.op, *rc);
                } else {
                    static_assert(!SameAs<AT, AT>, "missing ArithmeticOperation case");
                    return Evaluated{Constant{Null{}}};
                }
            });
        } else if constexpr (SameAs<T, TypeHint>) {
            return evaluate_term(v.operand, ctx);
        } else {
            return Evaluated{Constant{Null{}}};
        }
    }

    static Evaluated evaluate_term(const Term& term, const EvalContext& ctx) {
        return visit(term.value, [&](const auto& v) -> Evaluated {
            return evaluate_term_content(v, ctx);
        });
    }

    static Evaluated evaluate_function_call(const FunctionCall& call, const EvalContext& ctx) {
        DynamicArray<Evaluated> args{};
        for (U64 i = 0; i < call.arguments.length; i++)
            push_back(args, evaluate_term(call.arguments[i], ctx));
        FunctionEntry* fn = find_function(call.identifier);
        if (fn == nullptr) return Evaluated{Constant{Null{}}};
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
                        Evaluated operand = evaluate_toi(arith_v.operand, ctx);
                        const Constant* c = get_if_constant(operand);
                        if (!c) return Evaluated{Constant{Null{}}};
                        return evaluate_unary_minus(*c);
                    } else if constexpr (SameAs<AT, TOIBinaryArithmetic>) {
                        Evaluated lhs = evaluate_toi(arith_v.lhs, ctx);
                        Evaluated rhs = evaluate_toi(arith_v.rhs, ctx);
                        const Constant* lc = get_if_constant(lhs);
                        const Constant* rc = get_if_constant(rhs);
                        if (!lc || !rc) return Evaluated{Constant{Null{}}};
                        return evaluate_binary_arithmetic(*lc, arith_v.op, *rc);
                    } else {
                        static_assert(!SameAs<AT, AT>, "missing TOIArithmeticOperation case");
                        return Evaluated{Constant{Null{}}};
                    }
                });
            } else {
                return evaluate_term_content(v, ctx);
            }
        });
    }

    Evaluated evaluate(const Term& term, const EvalContext& ctx) { return evaluate_term(term, ctx); }
    Evaluated evaluate(const Term& term)                          { return evaluate_term(term, EvalContext{}); }
    Evaluated evaluate(const TermWithIdentifiers& twi, const EvalContext& ctx) { return evaluate_toi(twi, ctx); }

    // ========================================================================
    // predicate helpers
    // ========================================================================
    static bool as_s64(const Evaluated& e, S64& out) {
        bool found = false;
        visit(e.value, [&](const auto& top) {
            using T = Decay<decltype(top)>;
            if constexpr (SameAs<T, Constant>) {
                visit(top.value, [&](const auto& v) {
                    using V = Decay<decltype(v)>;
                    if constexpr (SameAs<V, S64>)  { out = v; found = true; }
                    else if constexpr (SameAs<V, bool>) { out = v ? 1 : 0; found = true; }
                });
            } else if constexpr (SameAs<T, ColumnValue>) {
                visit(top, [&](const auto& cv) {
                    using V = Decay<decltype(cv)>;
                    if constexpr (SameAs<V, S64>) { out = cv; found = true; }
                    else if constexpr (SameAs<V, S32>) { out = S64(cv); found = true; }
                    else if constexpr (SameAs<V, S16>) { out = S64(cv); found = true; }
                    else if constexpr (SameAs<V, U8>)  { out = S64(cv); found = true; }
                });
            }
        });
        return found;
    }

    static bool as_f64(const Evaluated& e, F64& out) {
        bool found = false;
        visit(e.value, [&](const auto& top) {
            using T = Decay<decltype(top)>;
            if constexpr (SameAs<T, Constant>) {
                visit(top.value, [&](const auto& v) {
                    using V = Decay<decltype(v)>;
                    if constexpr (SameAs<V, F64>) { out = v; found = true; }
                    else if constexpr (SameAs<V, S64>) { out = static_cast<F64>(v); found = true; }
                });
            } else if constexpr (SameAs<T, ColumnValue>) {
                visit(top, [&](const auto& cv) {
                    using V = Decay<decltype(cv)>;
                    if constexpr (SameAs<V, F64>) { out = cv; found = true; }
                    else if constexpr (SameAs<V, F32>) { out = F64(cv); found = true; }
                    else if constexpr (SameAs<V, S64>) { out = static_cast<F64>(cv); found = true; }
                    else if constexpr (SameAs<V, S32>) { out = static_cast<F64>(cv); found = true; }
                });
            }
        });
        return found;
    }

    static bool eval_is_null(const Evaluated& e) { return as_value<Null>(e) != nullptr; }

    static S64 compare_evaluated(const Evaluated& lhs, const Evaluated& rhs, bool& comparable) {
        comparable = true;
        if (eval_is_null(lhs) || eval_is_null(rhs)) { comparable = false; return 0; }

        S64 l_int, r_int;
        if (as_s64(lhs, l_int) && as_s64(rhs, r_int))
            return l_int < r_int ? -1 : (l_int > r_int ? 1 : 0);

        F64 l_flt, r_flt;
        if (as_f64(lhs, l_flt) && as_f64(rhs, r_flt))
            return l_flt < r_flt ? -1 : (l_flt > r_flt ? 1 : 0);

        const AutoString8* ls = as_value<AutoString8>(lhs);
        const AutoString8* rs = as_value<AutoString8>(rhs);
        if (ls && rs) {
            U64 min_len = ls->length < rs->length ? ls->length : rs->length;
            for (U64 i = 0; i < min_len; i++) {
                if (ls->c_str[i] != rs->c_str[i])
                    return static_cast<U8>(ls->c_str[i]) < static_cast<U8>(rs->c_str[i]) ? -1 : 1;
            }
            return ls->length < rs->length ? -1 : (ls->length > rs->length ? 1 : 0);
        }

        const UUID* lu = as_value<UUID>(lhs);
        const UUID* ru = as_value<UUID>(rhs);
        if (lu && ru) {
            for (U64 i = 0; i < UUID::length; i++) {
                if (lu->value[i] != ru->value[i])
                    return lu->value[i] < ru->value[i] ? -1 : 1;
            }
            return 0;
        }

        comparable = false;
        return 0;
    }

    static bool apply_operator(S64 cmp, Operator op) {
        switch (op) {
            case Operator::eq: return cmp == 0;
            case Operator::ne: return cmp != 0;
            case Operator::lt: return cmp < 0;
            case Operator::le: return cmp <= 0;
            case Operator::gt: return cmp > 0;
            case Operator::ge: return cmp >= 0;
            default: return false;
        }
    }

    static bool evaluate_column_relation(const WhereClause::ColumnExpressionRelation& cer, const EvalContext& ctx) {
        Evaluated lhs = lookup_column_value(cer.column.identifier, ctx);
        Evaluated rhs = evaluate_term(cer.value, ctx);
        bool comparable;
        S64 cmp = compare_evaluated(lhs, rhs, comparable);
        if (!comparable) return false;
        return apply_operator(cmp, cer.operator_);
    }

    bool evaluate_where(TArrayView<const WhereClause::Relation> predicates, const EvalContext& ctx) {
        for (U64 i = 0; i < predicates.length; i++) {
            bool passes = visit(predicates[i].value, [&](const auto& value) -> bool {
                using T = Decay<decltype(value)>;
                if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>)
                    return evaluate_column_relation(value, ctx);
                assert_not_implemented("TupleExpressionRelation and TokenRelation in WHERE are not implemented");
                return false;
            });
            if (!passes) return false;
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
                if (v.identifier.length == 0) v.identifier = to_str(idx++);
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
                for (U64 i = 0; i < v.arguments.length; i++)
                    idx = number_bind_markers_in_term(v.arguments[i], idx);
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
                        for (U64 i = 0; i < clause.values.length; i++)
                            idx = number_bind_markers_in_term(clause.values[i], idx);
                    }
                });
            }
            // @todo Update, Delete, Select WHERE clause
        });
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
