module;
#include <plexdb/support/tracy/tracy.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/dsl/expression.hpp>
#include <lexy/error.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/input_location.hpp>

#include <plexdb/support/lexy/macros.h>

module cql.parsers;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import plexdb.support.lexy.error_callback;

import cql.log;

namespace cql::parsers {
    namespace grammar {
        namespace dsl = lexy::dsl;

        // ====================================================================
        // sink helpers
        // ====================================================================
        template<typename T>
        struct _dyn_arr_sink {
            using return_type = DynamicArray<T>;
            DynamicArray<T> _arr;
            void            operator()(T&& val) {
                push_back(_arr, move(val));
            }
            return_type finish() && {
                return move(_arr);
            }
        };

        template<typename T>
        struct as_dyn_arr_t {
            constexpr auto sink() const {
                return _dyn_arr_sink<T>{};
            }
        };

        template<typename T>
        constexpr as_dyn_arr_t<T> as_dyn_arr{};

        struct _autostr8_sink {
            using return_type = AutoString8;
            AutoString8 _str;
            template<typename Reader>
            void operator()(const lexy::lexeme<Reader>& lexeme) {
                append(_str, lexeme.begin(), lexeme.end());
            }
            AutoString8 finish() && {
                return move(_str);
            }
        };

        struct as_autostr8_t {
            constexpr auto sink() const {
                return _autostr8_sink{};
            }
        };

        constexpr as_autostr8_t as_autostr8{};

        // ====================================================================
        // keywords
        // ====================================================================
        constexpr auto kw_create       = LEXY_LIT_CI("create");
        constexpr auto kw_alter        = LEXY_LIT_CI("alter");
        constexpr auto kw_drop         = LEXY_LIT_CI("drop");
        constexpr auto kw_truncate     = LEXY_LIT_CI("truncate");
        constexpr auto kw_keyspace     = LEXY_LIT_CI("keyspace");
        constexpr auto kw_table        = LEXY_LIT_CI("table");
        constexpr auto kw_columnfamily = LEXY_LIT_CI("columnfamily");
        constexpr auto kw_if           = LEXY_LIT_CI("if");
        constexpr auto kw_not          = LEXY_LIT_CI("not");
        constexpr auto kw_exists       = LEXY_LIT_CI("exists");
        constexpr auto kw_insert       = LEXY_LIT_CI("insert");
        constexpr auto kw_into         = LEXY_LIT_CI("into");
        constexpr auto kw_values       = LEXY_LIT_CI("values");
        constexpr auto kw_select       = LEXY_LIT_CI("select");
        constexpr auto kw_from         = LEXY_LIT_CI("from");
        constexpr auto kw_with         = LEXY_LIT_CI("with");
        constexpr auto kw_primary      = LEXY_LIT_CI("primary");
        constexpr auto kw_key          = LEXY_LIT_CI("key");
        constexpr auto kw_and          = LEXY_LIT_CI("and");
        constexpr auto kw_use          = LEXY_LIT_CI("use");
        constexpr auto kw_update       = LEXY_LIT_CI("update");
        constexpr auto kw_set          = LEXY_LIT_CI("set");
        constexpr auto kw_where        = LEXY_LIT_CI("where");
        constexpr auto kw_delete       = LEXY_LIT_CI("delete");
        constexpr auto kw_using        = LEXY_LIT_CI("using");
        constexpr auto kw_timestamp    = LEXY_LIT_CI("timestamp");
        constexpr auto kw_ttl          = LEXY_LIT_CI("ttl");
        constexpr auto kw_begin        = LEXY_LIT_CI("begin");
        constexpr auto kw_apply        = LEXY_LIT_CI("apply");
        constexpr auto kw_batch        = LEXY_LIT_CI("batch");
        constexpr auto kw_unlogged     = LEXY_LIT_CI("unlogged");
        constexpr auto kw_counter      = LEXY_LIT_CI("counter");
        constexpr auto kw_add          = LEXY_LIT_CI("add");
        constexpr auto kw_rename       = LEXY_LIT_CI("rename");
        constexpr auto kw_to           = LEXY_LIT_CI("to");
        constexpr auto kw_limit        = LEXY_LIT_CI("limit");
        constexpr auto kw_in           = LEXY_LIT_CI("in");
        constexpr auto kw_contains     = LEXY_LIT_CI("contains");
        constexpr auto kw_true         = LEXY_LIT_CI("true");
        constexpr auto kw_false        = LEXY_LIT_CI("false");
        constexpr auto kw_null         = LEXY_LIT_CI("null");
        constexpr auto kw_json         = LEXY_LIT_CI("json");
        constexpr auto kw_distinct     = LEXY_LIT_CI("distinct");
        constexpr auto kw_as           = LEXY_LIT_CI("as");
        constexpr auto kw_cast         = LEXY_LIT_CI("cast");
        constexpr auto kw_count        = LEXY_LIT_CI("count");
        constexpr auto kw_order        = LEXY_LIT_CI("order");
        constexpr auto kw_by           = LEXY_LIT_CI("by");
        constexpr auto kw_asc          = LEXY_LIT_CI("asc");
        constexpr auto kw_desc         = LEXY_LIT_CI("desc");
        constexpr auto kw_allow        = LEXY_LIT_CI("allow");
        constexpr auto kw_filtering    = LEXY_LIT_CI("filtering");
        constexpr auto kw_group        = LEXY_LIT_CI("group");
        constexpr auto kw_per          = LEXY_LIT_CI("per");
        constexpr auto kw_partition    = LEXY_LIT_CI("partition");
        constexpr auto kw_static       = LEXY_LIT_CI("static");
        constexpr auto kw_frozen       = LEXY_LIT_CI("frozen");
        constexpr auto kw_list         = LEXY_LIT_CI("list");
        constexpr auto kw_map          = LEXY_LIT_CI("map");
        constexpr auto kw_tuple        = LEXY_LIT_CI("tuple");
        constexpr auto kw_logged       = LEXY_LIT_CI("logged");
        constexpr auto kw_unset        = LEXY_LIT_CI("unset");
        constexpr auto kw_default      = LEXY_LIT_CI("default");
        constexpr auto kw_compact      = LEXY_LIT_CI("compact");
        constexpr auto kw_storage      = LEXY_LIT_CI("storage");
        constexpr auto kw_clustering   = LEXY_LIT_CI("clustering");
        constexpr auto kw_token        = LEXY_LIT_CI("token");
        constexpr auto kw_mask         = LEXY_LIT_CI("mask");
        constexpr auto kw_masked       = LEXY_LIT_CI("masked");
        constexpr auto kw_index        = LEXY_LIT_CI("index");
        constexpr auto kw_type         = LEXY_LIT_CI("type");
        constexpr auto kw_on           = LEXY_LIT_CI("on");
        constexpr auto kw_custom       = LEXY_LIT_CI("custom");

        struct ws {
            static constexpr auto rule  = dsl::whitespace(dsl::ascii::space / dsl::ascii::newline);
            static constexpr auto value = lexy::noop;
        };

        // ====================================================================
        // identifiers
        // ====================================================================
        struct unquoted_identifier {
            static constexpr auto rule  = dsl::identifier(dsl::ascii::alpha_digit_underscore);
            static constexpr auto value = lexy::callback<AutoString8>(
                [](auto lex) -> AutoString8 {
                    AutoString8 result{static_cast<U64>(lex.size())};
                    os::memory_copy(result.c_str, lex.begin(), lex.size());
                    to_lowercase_inplace(result);
                    return move(result);
                }
            );
        };

        struct quoted_identifier {
            static constexpr auto rule = [] {
                auto escaped_quote = dsl::escape(LEXY_LIT("\"")).rule(dsl::lit_c<'"'>);
                return dsl::delimited(LEXY_LIT("\""))(dsl::ascii::print, escaped_quote);
            }();
            static constexpr auto value = as_autostr8;
        };

        struct identifier {
            static constexpr auto rule  = dsl::p<quoted_identifier> | dsl::p<unquoted_identifier>;
            static constexpr auto value = lexy::forward<AutoString8>;
        };

        // ====================================================================
        // literals
        // ====================================================================
        struct string_literal {
            static constexpr auto rule = [] {
                auto not_quote = -dsl::lit_c<'\''>;
                auto content   = LEXY_LIT("''") | not_quote;
                return dsl::capture(dsl::token(dsl::lit_c<'\''> + dsl::while_(content) + dsl::lit_c<'\''>));
            }();

            static constexpr auto value = lexy::callback<AutoString8>(
                [](auto lex) -> AutoString8 {
                    auto src = lex.begin() + 1; // skip opening '
                    auto end = lex.end() - 1;   // skip closing '

                    AutoString8 result{static_cast<U64>(end - src)};
                    U64         wi = 0;
                    for (; src != end; ++src) {
                        result.c_str[wi++] = *src;
                        if (*src == '\'' && src + 1 != end && *(src + 1) == '\'') {
                            ++src; // skip escaped quote
                        }
                    }
                    result.length    = wi;
                    result.c_str[wi] = '\0';
                    return result;
                }
            );
        };

        struct dollar_string_literal {
            static constexpr auto rule  = dsl::delimited(LEXY_LIT("$$"))(dsl::ascii::character);
            static constexpr auto value = as_autostr8;
        };

        struct integer_literal {
            static constexpr auto rule = [] {
                auto plus_minus = dsl::lit_c<'-'> / dsl::lit_c<'+'>;
                return dsl::peek(plus_minus / dsl::digit<>) >> dsl::sign + dsl::integer<S64>;
            }();
            static constexpr auto value = lexy::as_integer<S64>;
        };

        struct float_literal {
            struct integer : lexy::transparent_production {
                static constexpr auto rule  = dsl::sign + dsl::integer<S64>;
                static constexpr auto value = lexy::as_integer<S64>;
            };
            struct fraction : lexy::transparent_production {
                static constexpr auto rule  = dsl::lit_c<'.'> >> dsl::capture(dsl::digits<>);
                static constexpr auto value = lexy::callback<F64>(
                    [](auto lexeme) -> F64 {
                        F64 val = 0.0, divisor = 1.0;
                        for (auto c : lexeme) {
                            divisor *= 10.0;
                            val += static_cast<F64>(c - '0') / divisor;
                        }
                        return val;
                    }
                );
            };
            struct exponent : lexy::transparent_production {
                static constexpr auto rule = [] {
                    auto exp_char = dsl::lit_c<'e'> / dsl::lit_c<'E'>;
                    return exp_char >> dsl::sign + dsl::integer<S64>;
                }();
                static constexpr auto value = lexy::as_integer<S64>;
            };

            static constexpr auto rule = [] {
                auto has_dot_or_exp = dsl::lookahead(LEXY_LIT("."), dsl::literal_set(LEXY_LIT(";"), LEXY_LIT(")"), LEXY_LIT(","), LEXY_LIT(" "), LEXY_LIT("]"), LEXY_LIT("}"))) | dsl::lookahead(LEXY_LIT("e"), dsl::literal_set(LEXY_LIT(";"), LEXY_LIT(")"), LEXY_LIT(","), LEXY_LIT(" "), LEXY_LIT("]"), LEXY_LIT("}"))) | dsl::lookahead(LEXY_LIT("E"), dsl::literal_set(LEXY_LIT(";"), LEXY_LIT(")"), LEXY_LIT(","), LEXY_LIT(" "), LEXY_LIT("]"), LEXY_LIT("}")));
                return has_dot_or_exp >> dsl::p<integer> + dsl::opt(dsl::p<fraction>) + dsl::opt(dsl::p<exponent>);
            }();

            static auto apply_exp(F64 val, S64 exp) -> F64 {
                S64 abs_exp = exp < 0 ? -exp : exp;
                F64 factor  = 1.0;
                for (S64 i = 0; i < abs_exp; i++) {
                    factor *= 10.0;
                }
                return exp > 0 ? val * factor : val / factor;
            }

            static constexpr auto value = lexy::callback<F64>(
                [](S64 ip, lexy::nullopt, lexy::nullopt) -> F64 {
                    return static_cast<F64>(ip);
                },
                [](S64 ip, F64 frac, lexy::nullopt) -> F64 {
                    return ip < 0 ? static_cast<F64>(ip) - frac : static_cast<F64>(ip) + frac;
                },
                [](S64 ip, lexy::nullopt, S64 exp) -> F64 {
                    return apply_exp(static_cast<F64>(ip), exp);
                },
                [](S64 ip, F64 frac, S64 exp) -> F64 {
                    F64 val = ip < 0 ? static_cast<F64>(ip) - frac : static_cast<F64>(ip) + frac;
                    return apply_exp(val, exp);
                }
            );
        };

        struct boolean_literal {
            struct true_ : lexy::transparent_production {
                static constexpr auto rule  = kw_true;
                static constexpr auto value = lexy::constant(true);
            };
            struct false_ : lexy::transparent_production {
                static constexpr auto rule  = kw_false;
                static constexpr auto value = lexy::constant(false);
            };
            static constexpr auto rule  = dsl::p<true_> | dsl::p<false_>;
            static constexpr auto value = lexy::forward<bool>;
        };

        struct null_literal {
            static constexpr auto rule  = kw_null;
            static constexpr auto value = lexy::constant(Null{});
        };

        struct uuid_literal {
            static constexpr auto rule = [] {
                auto is_uuid = dsl::peek(dsl::n_digits<8, dsl::hex> + dsl::lit_c<'-'>);
                auto sep     = dsl::lit_c<'-'>;
                return is_uuid >> dsl::capture(dsl::n_digits<8, dsl::hex>) + sep + dsl::capture(dsl::n_digits<4, dsl::hex>) + sep + dsl::capture(dsl::n_digits<4, dsl::hex>) + sep + dsl::capture(dsl::n_digits<4, dsl::hex>) + sep + dsl::capture(dsl::n_digits<12, dsl::hex>);
            }();
            static constexpr auto value = lexy::callback<UUID>(
                [](auto g8, auto g4a, auto g4b, auto g4c, auto g12) -> UUID {
                    UUID uuid{};
                    U64  byte_idx = 0;
                    bool high     = true;
                    auto hex_val  = [](char c) -> U8 {
                        if (c >= '0' && c <= '9') {
                            return static_cast<U8>(c - '0');
                        }
                        if (c >= 'a' && c <= 'f') {
                            return static_cast<U8>(c - 'a' + 10);
                        }
                        return static_cast<U8>(c - 'A' + 10);
                    };
                    auto process = [&](auto lexeme) {
                        for (char c : lexeme) {
                            if (high) {
                                uuid.value[byte_idx] = hex_val(c) << 4;
                                high                 = false;
                            } else {
                                uuid.value[byte_idx] |= hex_val(c);
                                byte_idx++;
                                high = true;
                            }
                        }
                    };
                    process(g8);
                    process(g4a);
                    process(g4b);
                    process(g4c);
                    process(g12);
                    return uuid;
                }
            );
        };

        struct duration_literal {
            struct unit_mo : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("mo");
                static constexpr auto value = lexy::constant(1);
            };
            struct unit_ms : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("ms");
                static constexpr auto value = lexy::constant(5);
            };
            struct unit_us : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("us");
                static constexpr auto value = lexy::constant(8);
            };
            struct unit_ns : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("ns");
                static constexpr auto value = lexy::constant(9);
            };
            struct unit_y : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("y");
                static constexpr auto value = lexy::constant(0);
            };
            struct unit_w : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("w");
                static constexpr auto value = lexy::constant(2);
            };
            struct unit_d : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("d");
                static constexpr auto value = lexy::constant(3);
            };
            struct unit_h : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("h");
                static constexpr auto value = lexy::constant(4);
            };
            struct unit_m : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("m");
                static constexpr auto value = lexy::constant(6);
            };
            struct unit_s : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("s");
                static constexpr auto value = lexy::constant(7);
            };
            struct dur_unit {
                static constexpr auto rule  = dsl::p<unit_mo> | dsl::p<unit_ms> | dsl::p<unit_us> | dsl::p<unit_ns> | dsl::p<unit_y> | dsl::p<unit_w> | dsl::p<unit_d> | dsl::p<unit_h> | dsl::p<unit_m> | dsl::p<unit_s>;
                static constexpr auto value = lexy::forward<int>;
            };
            struct dur_amount : lexy::transparent_production {
                static constexpr auto rule  = dsl::integer<S64>;
                static constexpr auto value = lexy::as_integer<S64>;
            };
            struct component {
                static constexpr auto rule  = dsl::p<dur_amount> + dsl::p<dur_unit>;
                static constexpr auto value = lexy::callback<Duration>(
                    [](S64 n, int u) -> Duration {
                        Duration d{};
                        switch (u) {
                            case 0:
                                d.months = static_cast<S32>(n * 12);
                                break;
                            case 1:
                                d.months = static_cast<S32>(n);
                                break;
                            case 2:
                                d.days = static_cast<S32>(n * 7);
                                break;
                            case 3:
                                d.days = static_cast<S32>(n);
                                break;
                            case 4:
                                d.nanoseconds = n * 3'600'000'000'000LL;
                                break;
                            case 5:
                                d.nanoseconds = n * 1'000'000LL;
                                break;
                            case 6:
                                d.nanoseconds = n * 60'000'000'000LL;
                                break;
                            case 7:
                                d.nanoseconds = n * 1'000'000'000LL;
                                break;
                            case 8:
                                d.nanoseconds = n * 1'000LL;
                                break;
                            case 9:
                                d.nanoseconds = n;
                                break;
                            default:
                                break;
                        }
                        return d;
                    }
                );
            };
            struct components {
                static constexpr auto rule  = dsl::list(dsl::peek(dsl::digit<>) >> dsl::p<component>);
                static constexpr auto value = as_dyn_arr<Duration>;
            };
            static constexpr auto rule = [] {
                auto dur_start   = dsl::lit_c<'y'> | dsl::lit_c<'m'> | dsl::lit_c<'w'> | dsl::lit_c<'d'> | dsl::lit_c<'h'> | dsl::lit_c<'s'> | dsl::lit_c<'u'> | dsl::lit_c<'n'>;
                auto is_duration = dsl::peek(dsl::while_one(dsl::digit<>) + dur_start);
                return is_duration >> dsl::p<components>;
            }();
            static constexpr auto value = lexy::callback<Duration>(
                [](DynamicArray<Duration>&& deltas) -> Duration {
                    Duration result{};
                    for (U64 i = 0; i < deltas.length; i++) {
                        result.months += deltas[i].months;
                        result.days += deltas[i].days;
                        result.nanoseconds += deltas[i].nanoseconds;
                    }
                    return result;
                }
            );
        };

        struct constant_value {
            static constexpr auto rule = [] {
                auto str      = dsl::p<string_literal>;
                auto dollar   = dsl::p<dollar_string_literal>;
                auto boolean  = dsl::p<boolean_literal>;
                auto null     = dsl::p<null_literal>;
                auto uuid     = dsl::p<uuid_literal>;
                auto flt      = dsl::p<float_literal>;
                auto duration = dsl::p<duration_literal>;
                auto integer  = dsl::p<integer_literal>;
                return str | dollar | boolean | null | uuid | flt | duration | integer;
            }();
            static constexpr auto value = lexy::callback<Constant>(
                [](AutoString8&& s) -> Constant { return {.value = move(s)}; },
                [](bool b) -> Constant { return {.value = b}; },
                [](Null n) -> Constant { return {.value = n}; },
                [](F64 f) -> Constant { return {.value = f}; },
                [](S64 i) -> Constant { return {.value = i}; },
                [](UUID&& u) -> Constant { return {.value = move(u)}; },
                [](Duration&& d) -> Constant { return {.value = move(d)}; }
            );
        };

        // ====================================================================
        // data types
        // ====================================================================
        struct basic_type {
            struct t_ascii : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("ascii");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::ascii); });
            };
            struct t_bigint : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("bigint");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::bigint); });
            };
            struct t_blob : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("blob");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::blob); });
            };
            struct t_boolean : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("boolean");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::boolean); });
            };
            struct t_counter : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("counter");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::counter); });
            };
            struct t_date : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("date");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::date); });
            };
            struct t_decimal : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("decimal");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::decimal); });
            };
            struct t_double : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("double");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::double_); });
            };
            struct t_duration : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("duration");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::duration); });
            };
            struct t_float : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("float");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::float_); });
            };
            struct t_inet : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("inet");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::inet); });
            };
            struct t_int : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("int");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::int_); });
            };
            struct t_smallint : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("smallint");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::smallint); });
            };
            struct t_text : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("text");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::text); });
            };
            struct t_time : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("time");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::time); });
            };
            struct t_timestamp : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("timestamp");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::timestamp); });
            };
            struct t_timeuuid : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("timeuuid");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::timeuuid); });
            };
            struct t_tinyint : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("tinyint");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::tinyint); });
            };
            struct t_uuid : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("uuid");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::uuid); });
            };
            struct t_varchar : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("varchar");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::varchar); });
            };
            struct t_varint : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("varint");
                static constexpr auto value = lexy::callback<type::Type>([] { return type::create_basic(type::Basic::varint); });
            };

            // @note order matters: longer prefixes first (timestamp before time, smallint before set, etc.)
            static constexpr auto rule  = dsl::p<t_timestamp> | dsl::p<t_timeuuid> | dsl::p<t_tinyint> | dsl::p<t_text> | dsl::p<t_time> | dsl::p<t_ascii> | dsl::p<t_bigint> | dsl::p<t_blob> | dsl::p<t_boolean> | dsl::p<t_counter> | dsl::p<t_date> | dsl::p<t_decimal> | dsl::p<t_double> | dsl::p<t_duration> | dsl::p<t_float> | dsl::p<t_inet> | dsl::p<t_int> | dsl::p<t_smallint> | dsl::p<t_uuid> | dsl::p<t_varchar> | dsl::p<t_varint>;
            static constexpr auto value = lexy::forward<type::Type>;
        };

        struct collection_type;

        struct frozen_type {
            static constexpr auto rule  = kw_frozen >> dsl::p<ws> + dsl::lit_c<'<'> + dsl::p<ws> + (dsl::p<basic_type> | dsl::else_ >> dsl::recurse<collection_type>)+dsl::p<ws> + dsl::lit_c<'>'>;
            static constexpr auto value = lexy::callback<type::Type>(
                [](type::Type fwd) {
                    visit(fwd.value, [](auto& v) {
                        using T = RemoveRef<decltype(v)>;
                        if constexpr (!SameAs<T, type::Basic>) {
                            v.frozen = true;
                        }
                    });
                    return fwd;
                }
            );
        };

        struct collection_type {
            struct element_type {
                static constexpr auto rule  = dsl::p<frozen_type> | dsl::p<basic_type> | dsl::else_ >> dsl::recurse<collection_type>;
                static constexpr auto value = lexy::forward<type::Type>;
            };
            struct list_type {
                static constexpr auto rule  = kw_list >> dsl::p<ws> + dsl::lit_c<'<'> + dsl::p<ws> + dsl::p<element_type> + dsl::p<ws> + dsl::lit_c<'>'>;
                static constexpr auto value = lexy::callback<type::Type>(
                    [](type::Type el) { return type::create_list(move(el)); }
                );
            };
            struct set_type {
                static constexpr auto rule  = kw_set >> dsl::p<ws> + dsl::lit_c<'<'> + dsl::p<ws> + dsl::p<element_type> + dsl::p<ws> + dsl::lit_c<'>'>;
                static constexpr auto value = lexy::callback<type::Type>(
                    [](type::Type key) { return type::create_set(move(key)); }
                );
            };
            struct map_type {
                static constexpr auto rule  = kw_map >> dsl::p<ws> + dsl::lit_c<'<'> + dsl::p<ws> + dsl::p<element_type> + dsl::p<ws> + dsl::lit_c<','> + dsl::p<ws> + dsl::p<element_type> + dsl::p<ws> + dsl::lit_c<'>'>;
                static constexpr auto value = lexy::callback<type::Type>(
                    [](type::Type key, type::Type val) { return type::create_map(move(key), move(val)); }
                );
            };
            struct tuple_type {
                struct element_list {
                    static constexpr auto rule  = dsl::list(dsl::p<element_type> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
                    static constexpr auto value = as_dyn_arr<type::Type>;
                };
                static constexpr auto rule  = kw_tuple >> dsl::p<ws> + dsl::lit_c<'<'> + dsl::p<ws> + dsl::p<element_list> + dsl::lit_c<'>'>;
                static constexpr auto value = lexy::callback<type::Type>(
                    [](DynamicArray<type::Type>&& elements) { return type::create_tuple(move(elements)); }
                );
            };
            static constexpr auto rule  = dsl::p<list_type> | dsl::p<set_type> | dsl::p<map_type> | dsl::p<tuple_type>;
            static constexpr auto value = lexy::forward<type::Type>;
        };

        struct data_type {
            static constexpr auto rule  = dsl::p<frozen_type> | dsl::p<collection_type> | dsl::p<basic_type>;
            static constexpr auto value = lexy::forward<type::Type>;
        };

        // ====================================================================
        // names
        // ====================================================================
        struct table_name {
            static constexpr auto rule = [] {
                return dsl::p<identifier> + dsl::opt(dsl::lit_c<'.'> >> dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::callback<TableName>(
                [](AutoString8&& a, AutoString8&& b) -> TableName { return {.keyspace_name = move(a), .table_name = move(b)}; },
                [](AutoString8&& a, lexy::nullopt) -> TableName { return {.keyspace_name = {}, .table_name = move(a)}; }
            );
        };

        struct column_name {
            static constexpr auto rule  = dsl::p<identifier>;
            static constexpr auto value = lexy::callback<ColumnName>(
                [](AutoString8&& name) -> ColumnName { return {.identifier = move(name)}; }
            );
        };

        // ====================================================================
        // terms
        // ====================================================================
        struct term_expr; // forward declare for recursion

        struct bind_marker {
            static constexpr auto rule = [] {
                auto anonymous = dsl::lit_c<'?'>;
                auto named     = dsl::lit_c<':'> >> dsl::p<identifier>;
                return anonymous | named;
            }();
            static constexpr auto value = lexy::callback<Term>(
                []() -> Term { return Term{.value = BindMarker{}}; },
                [](AutoString8&& name) -> Term { return Term{.value = BindMarker{.identifier = move(name)}}; }
            );
        };

        struct term_args_list {
            static constexpr auto rule  = dsl::list(dsl::recurse<term_expr> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Term>;
        };

        struct function_call_term {
            static constexpr auto rule = [] {
                auto args    = dsl::opt(dsl::peek_not(dsl::lit_c<')'>) >> dsl::p<term_args_list>);
                auto fn_head = dsl::peek(dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'('>);
                return fn_head >> dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + args + dsl::p<ws> + dsl::lit_c<')'>;
            }();
            static constexpr auto value = lexy::callback<Term>(
                [](AutoString8&& name, lexy::nullopt) -> Term {
                    return Term{
                        .value = FunctionCall{.identifier = move(name), .arguments = {}}
                    };
                },
                [](AutoString8&& name, DynamicArray<Term>&& args) -> Term {
                    return Term{
                        .value = FunctionCall{.identifier = move(name), .arguments = move(args)}
                    };
                }
            );
        };

        struct list_literal {
            static constexpr auto rule = [] {
                auto elems = dsl::opt(dsl::peek_not(dsl::lit_c<']'>) >> dsl::p<term_args_list>);
                return dsl::lit_c<'['> >> dsl::p<ws> + elems + dsl::p<ws> + dsl::lit_c<']'>;
            }();
            static constexpr auto value = lexy::callback<Term>(
                [](lexy::nullopt) -> Term { return Term{.value = ListOrVectorLiteral{}}; },
                [](DynamicArray<Term>&& elems) -> Term { return Term{.value = ListOrVectorLiteral{.elements = move(elems)}}; }
            );
        };

        struct map_entry {
            static constexpr auto rule  = dsl::recurse<term_expr> + dsl::p<ws> + dsl::lit_c<':'> + dsl::p<ws> + dsl::recurse<term_expr>;
            static constexpr auto value = lexy::callback<Pair<Term, Term>>(
                [](Term&& k, Term&& v) -> Pair<Term, Term> { return {move(k), move(v)}; }
            );
        };

        struct map_literal_inner {
            static constexpr auto rule  = dsl::list(dsl::p<map_entry> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Pair<Term, Term>>;
        };

        struct set_literal_inner {
            static constexpr auto rule  = dsl::list(dsl::recurse<term_expr> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Term>;
        };

        struct udt_entry {
            static constexpr auto rule  = dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<':'> + dsl::p<ws> + dsl::recurse<term_expr>;
            static constexpr auto value = lexy::callback<Pair<AutoString8, Term>>(
                [](AutoString8&& k, Term&& v) -> Pair<AutoString8, Term> { return {move(k), move(v)}; }
            );
        };
        struct udt_literal_inner {
            static constexpr auto rule  = dsl::list(dsl::p<udt_entry> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Pair<AutoString8, Term>>;
        };
        struct curly_literal {
            static constexpr auto rule = [] {
                auto is_udt  = dsl::peek(dsl::ascii::alpha_underscore + dsl::while_(dsl::ascii::alpha_digit_underscore) + dsl::p<ws> + dsl::lit_c<':'>);
                auto is_map  = dsl::lookahead(LEXY_LIT(":"), LEXY_LIT("}"));
                auto is_set  = dsl::peek_not(dsl::lit_c<'}'>);
                auto content = is_udt >> dsl::p<udt_literal_inner> | is_map >> dsl::p<map_literal_inner> | is_set >> dsl::p<set_literal_inner>;
                return dsl::lit_c<'{'> >> dsl::p<ws> + dsl::opt(content) + dsl::p<ws> + dsl::lit_c<'}'>;
            }();
            static constexpr auto value = lexy::callback<Term>(
                [](lexy::nullopt) -> Term { return Term{.value = SetLiteral{}}; },
                [](DynamicArray<Pair<AutoString8, Term>>&& entries) -> Term { return Term{.value = UdtLiteral{.identifier_values = move(entries)}}; },
                [](DynamicArray<Pair<Term, Term>>&& entries) -> Term { return Term{.value = MapLiteral{.key_values = move(entries)}}; },
                [](DynamicArray<Term>&& entries) -> Term { return Term{.value = SetLiteral{.keys = move(entries)}}; }
            );
        };

        struct tuple_or_paren {
            static constexpr auto rule = [] {
                auto elems = dsl::opt(dsl::peek_not(dsl::lit_c<')'>) >> dsl::p<term_args_list>);
                return dsl::lit_c<'('> >> dsl::p<ws> + elems + dsl::p<ws> + dsl::lit_c<')'>;
            }();
            static constexpr auto value = lexy::callback<Term>(
                [](lexy::nullopt) -> Term { return Term{.value = TupleLiteral{}}; },
                [](DynamicArray<Term>&& elems) -> Term {
                    if (elems.length == 1) {
                        return move(elems[0]);
                    }
                    return Term{.value = TupleLiteral{.elements = move(elems)}};
                }
            );
        };

        struct type_hint_term {
            static constexpr auto rule = [] {
                auto cast_head = dsl::peek(dsl::lit_c<'('> + dsl::p<ws> + dsl::p<data_type> + dsl::p<ws> + dsl::lit_c<')'>);
                return cast_head >> dsl::lit_c<'('> + dsl::p<ws> + dsl::p<data_type> + dsl::p<ws> + dsl::lit_c<')'> + dsl::p<ws> + dsl::recurse<term_expr>;
            }();
            static constexpr auto value = lexy::callback<Term>(
                [](type::Type type, Term&& operand) -> Term {
                    return Term{
                        .value = TypeHint{type, move(operand)}
                    };
                }
            );
        };

        struct constant_term {
            static constexpr auto rule  = dsl::p<constant_value>;
            static constexpr auto value = lexy::callback<Term>(
                [](Constant&& cv) -> Term { return Term{.value = move(cv)}; }
            );
        };

        // @note order matters for disambiguation
        struct primary_term {
            static constexpr auto rule = [] {
                auto bind  = dsl::p<bind_marker>;
                auto curly = dsl::p<curly_literal>;
                auto list  = dsl::p<list_literal>;
                auto func  = dsl::p<function_call_term>;
                auto hint  = dsl::p<type_hint_term>;
                auto paren = dsl::p<tuple_or_paren>;
                auto cnst  = dsl::p<constant_term>;
                return bind | curly | list | func | hint | paren | cnst;
            }();
            static constexpr auto value = lexy::forward<Term>;
        };

        // arithmetic expression with precedence
        constexpr auto op_plus  = dsl::op(LEXY_LIT("+"));
        constexpr auto op_minus = dsl::op(LEXY_LIT("-"));
        constexpr auto op_times = dsl::op(LEXY_LIT("*"));
        constexpr auto op_div   = dsl::op(LEXY_LIT("/"));
        constexpr auto op_mod   = dsl::op(dsl::lit_c<'%'>);
        constexpr auto op_neg   = dsl::op(LEXY_LIT("-"));

        struct term_expr : lexy::expression_production {
            static constexpr auto whitespace = dsl::ascii::space | dsl::ascii::newline;
            static constexpr auto atom       = dsl::p<primary_term>;

            struct negate : dsl::prefix_op {
                static constexpr auto op = op_neg;
                using operand            = dsl::atom;
            };

            struct mul_div : dsl::infix_op_left {
                static constexpr auto op = op_times / op_div / op_mod;
                using operand            = negate;
            };

            struct add_sub : dsl::infix_op_left {
                static constexpr auto op = op_plus / op_minus;
                using operand            = mul_div;
            };

            using operation = add_sub;

            static constexpr auto value = lexy::callback<Term>(
                [](Term&& t) -> Term { return move(t); },
                [](Term&& lhs, lexy::op<op_plus>, Term&& rhs) -> Term {
                    BinaryArithmeticOperation bin{move(lhs), ArithmeticOperator::plus, move(rhs)};
                    return Term{.value = ArithmeticOperation{.value = move(bin)}};
                },
                [](Term&& lhs, lexy::op<op_minus>, Term&& rhs) -> Term {
                    BinaryArithmeticOperation bin{move(lhs), ArithmeticOperator::minus, move(rhs)};
                    return Term{.value = ArithmeticOperation{.value = move(bin)}};
                },
                [](Term&& lhs, lexy::op<op_times>, Term&& rhs) -> Term {
                    BinaryArithmeticOperation bin{move(lhs), ArithmeticOperator::times, move(rhs)};
                    return Term{.value = ArithmeticOperation{.value = move(bin)}};
                },
                [](Term&& lhs, lexy::op<op_div>, Term&& rhs) -> Term {
                    BinaryArithmeticOperation bin{move(lhs), ArithmeticOperator::divide, move(rhs)};
                    return Term{.value = ArithmeticOperation{.value = move(bin)}};
                },
                [](Term&& lhs, lexy::op<op_mod>, Term&& rhs) -> Term {
                    BinaryArithmeticOperation bin{move(lhs), ArithmeticOperator::mod, move(rhs)};
                    return Term{.value = ArithmeticOperation{.value = move(bin)}};
                },
                [](lexy::op<op_neg>, Term&& t) -> Term {
                    UnaryMinusArithmeticOperation unary{move(t)};
                    return Term{.value = ArithmeticOperation{.value = move(unary)}};
                }
            );
        };

        // term_or_id: like primary_term but with identifier fallback
        struct primary_term_with_identifiers {
            static constexpr auto rule = [] {
                auto bind  = dsl::p<bind_marker>;
                auto curly = dsl::p<curly_literal>;
                auto list  = dsl::p<list_literal>;
                auto func  = dsl::p<function_call_term>;
                auto hint  = dsl::p<type_hint_term>;
                auto paren = dsl::p<tuple_or_paren>;
                auto cnst  = dsl::p<constant_term>;
                auto id    = dsl::p<identifier>;
                return bind | curly | list | func | hint | paren | cnst | dsl::else_ >> id;
            }();
            static constexpr auto value = lexy::callback<TermWithIdentifiers>(
                [](Term&& t) -> TermWithIdentifiers { return TermWithIdentifiers{move(t)}; },
                [](AutoString8&& s) -> TermWithIdentifiers { return TermWithIdentifiers{move(s)}; }
            );
        };

        struct term_with_identifiers_expr : lexy::expression_production {
            static constexpr auto whitespace = dsl::ascii::space | dsl::ascii::newline;
            static constexpr auto atom       = dsl::p<primary_term_with_identifiers>;

            struct negate : dsl::prefix_op {
                static constexpr auto op = op_neg;
                using operand            = dsl::atom;
            };

            struct mul_div : dsl::infix_op_left {
                static constexpr auto op = op_times / op_div / op_mod;
                using operand            = negate;
            };

            struct add_sub : dsl::infix_op_left {
                static constexpr auto op = op_plus / op_minus;
                using operand            = mul_div;
            };

            using operation = add_sub;

            static constexpr auto value = lexy::callback<TermWithIdentifiers>(
                [](TermWithIdentifiers&& t) -> TermWithIdentifiers { return move(t); },
                [](TermWithIdentifiers&& lhs, lexy::op<op_plus>, TermWithIdentifiers&& rhs) -> TermWithIdentifiers {
                    TOIBinaryArithmetic bin{move(lhs), ArithmeticOperator::plus, move(rhs)};
                    return TermWithIdentifiers{TOIArithmeticOperation{.value = move(bin)}};
                },
                [](TermWithIdentifiers&& lhs, lexy::op<op_minus>, TermWithIdentifiers&& rhs) -> TermWithIdentifiers {
                    TOIBinaryArithmetic bin{move(lhs), ArithmeticOperator::minus, move(rhs)};
                    return TermWithIdentifiers{TOIArithmeticOperation{.value = move(bin)}};
                },
                [](TermWithIdentifiers&& lhs, lexy::op<op_times>, TermWithIdentifiers&& rhs) -> TermWithIdentifiers {
                    TOIBinaryArithmetic bin{move(lhs), ArithmeticOperator::times, move(rhs)};
                    return TermWithIdentifiers{TOIArithmeticOperation{.value = move(bin)}};
                },
                [](TermWithIdentifiers&& lhs, lexy::op<op_div>, TermWithIdentifiers&& rhs) -> TermWithIdentifiers {
                    TOIBinaryArithmetic bin{move(lhs), ArithmeticOperator::divide, move(rhs)};
                    return TermWithIdentifiers{TOIArithmeticOperation{.value = move(bin)}};
                },
                [](TermWithIdentifiers&& lhs, lexy::op<op_mod>, TermWithIdentifiers&& rhs) -> TermWithIdentifiers {
                    TOIBinaryArithmetic bin{move(lhs), ArithmeticOperator::mod, move(rhs)};
                    return TermWithIdentifiers{TOIArithmeticOperation{.value = move(bin)}};
                },
                [](lexy::op<op_neg>, TermWithIdentifiers&& t) -> TermWithIdentifiers {
                    TOIUnaryMinus unary{move(t)};
                    return TermWithIdentifiers{TOIArithmeticOperation{.value = move(unary)}};
                }
            );
        };

        // ====================================================================
        // common
        // ====================================================================
        struct if_not_exists {
            static constexpr auto rule = [] {
                auto key = kw_if + dsl::p<ws> + kw_not + dsl::p<ws> + kw_exists;
                return dsl::opt(dsl::peek(key) >> key);
            }();
            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };

        struct if_exists {
            static constexpr auto rule = [] {
                auto key = kw_if + dsl::p<ws> + kw_exists;
                return dsl::opt(dsl::peek(key) >> key);
            }();
            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };

        struct comparison_op {
            struct eq : lexy::transparent_production {
                static constexpr auto rule  = dsl::lit_c<'='>;
                static constexpr auto value = lexy::constant(Operator::eq);
            };
            struct ne : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("!=");
                static constexpr auto value = lexy::constant(Operator::ne);
            };
            struct le : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT("<=");
                static constexpr auto value = lexy::constant(Operator::le);
            };
            struct lt : lexy::transparent_production {
                static constexpr auto rule  = dsl::lit_c<'<'>;
                static constexpr auto value = lexy::constant(Operator::lt);
            };
            struct ge : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT(">=");
                static constexpr auto value = lexy::constant(Operator::ge);
            };
            struct gt : lexy::transparent_production {
                static constexpr auto rule  = dsl::lit_c<'>'>;
                static constexpr auto value = lexy::constant(Operator::gt);
            };
            struct in_ : lexy::transparent_production {
                static constexpr auto rule  = kw_in;
                static constexpr auto value = lexy::constant(Operator::in);
            };
            struct contains_rule : lexy::transparent_production {
                static constexpr auto rule  = kw_contains >> dsl::opt(dsl::peek(dsl::p<ws> + kw_key) >> dsl::p<ws> + kw_key);
                static constexpr auto value = lexy::callback<Operator>(
                    [](lexy::nullopt) { return Operator::contains; },
                    []() { return Operator::contains_key; }
                );
            };

            static constexpr auto rule  = dsl::p<ne> | dsl::p<le> | dsl::p<ge> | dsl::p<lt> | dsl::p<gt> | dsl::p<eq> | dsl::p<in_> | dsl::p<contains_rule>;
            static constexpr auto value = lexy::forward<Operator>;
        };

        struct column_expression_relation {
            static constexpr auto rule = [] {
                return dsl::p<column_name> + dsl::p<ws> + dsl::p<comparison_op> + dsl::p<ws> + dsl::p<term_expr>;
            }();
            static constexpr auto value = lexy::callback<WhereClause::Relation>(
                [](ColumnName&& col, Operator op, Term&& val) -> WhereClause::Relation {
                    return {
                        .value = WhereClause::ColumnExpressionRelation{move(col), op, move(val)}
                    };
                }
            );
        };
        struct subscripted_relation {
            static constexpr auto rule = [] {
                auto subscripted_lhs = dsl::peek(dsl::p<column_name> + dsl::p<ws> + dsl::lit_c<'['>) >> dsl::p<column_name> + dsl::p<ws> + dsl::lit_c<'['> + dsl::p<ws> + dsl::p<term_expr> + dsl::p<ws> + dsl::lit_c<']'>;
                return subscripted_lhs + dsl::p<ws> + dsl::p<comparison_op> + dsl::p<ws> + dsl::p<term_expr>;
            }();
            static constexpr auto value = lexy::callback<WhereClause::Relation>(
                [](ColumnName&& col, Term&& sub, Operator op, Term&& val) -> WhereClause::Relation {
                    return {
                        .value = WhereClause::SubscriptedRelation{move(col), move(sub), op, move(val)}
                    };
                }
            );
        };
        struct column_name_list {
            static constexpr auto rule  = dsl::list(dsl::p<column_name> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<ColumnName>;
        };
        struct token_relation {
            static constexpr auto rule = [] {
                auto cols = dsl::parenthesized(dsl::p<ws> + dsl::p<column_name_list> + dsl::p<ws>);
                return kw_token >> dsl::p<ws> + cols + dsl::p<ws> + dsl::p<comparison_op> + dsl::p<ws> + dsl::p<term_expr>;
            }();
            static constexpr auto value = lexy::callback<WhereClause::Relation>(
                [](DynamicArray<ColumnName>&& cols, Operator op, Term&& val) -> WhereClause::Relation {
                    return {
                        .value = WhereClause::TokenRelation{move(cols), op, move(val)}
                    };
                }
            );
        };
        struct tuple_expression_relation {
            static constexpr auto rule = [] {
                auto cols      = dsl::parenthesized(dsl::p<ws> + dsl::p<column_name_list> + dsl::p<ws>);
                auto rhs_paren = dsl::lit_c<'('> >> dsl::p<ws> + dsl::p<term_args_list> + dsl::p<ws> + dsl::lit_c<')'>;
                auto rhs_term  = dsl::else_ >> dsl::p<term_expr>;
                auto rhs       = rhs_paren | rhs_term;
                return dsl::peek(dsl::lit_c<'('>) >> cols + dsl::p<ws> + dsl::p<comparison_op> + dsl::p<ws> + rhs;
            }();
            static constexpr auto value = lexy::callback<WhereClause::Relation>(
                [](DynamicArray<ColumnName>&& cols, Operator op, DynamicArray<Term>&& vals) -> WhereClause::Relation {
                    return {
                        .value = WhereClause::TupleExpressionRelation{move(cols), op, move(vals)}
                    };
                },
                [](DynamicArray<ColumnName>&& cols, Operator op, Term&& val) -> WhereClause::Relation {
                    DynamicArray<Term> vals;
                    push_back(vals, move(val));
                    return {
                        .value = WhereClause::TupleExpressionRelation{move(cols), op, move(vals)}
                    };
                }
            );
        };
        struct where_relation {
            static constexpr auto rule  = dsl::p<token_relation> | dsl::p<tuple_expression_relation> | dsl::p<subscripted_relation> | dsl::else_ >> dsl::p<column_expression_relation>;
            static constexpr auto value = lexy::forward<WhereClause::Relation>;
        };
        struct where_relations_list {
            static constexpr auto rule  = dsl::list(dsl::p<where_relation> + dsl::p<ws>, dsl::sep(kw_and >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<WhereClause::Relation>;
        };
        struct where_clause {
            static constexpr auto rule  = kw_where >> dsl::p<ws> + dsl::p<where_relations_list>;
            static constexpr auto value = lexy::callback<WhereClause>(
                [](DynamicArray<WhereClause::Relation>&& rels) -> WhereClause { return {.relations = move(rels)}; }
            );
        };

        struct simple_selection_rule {
            static constexpr auto rule = [] {
                auto subscript    = dsl::lit_c<'['> >> dsl::p<ws> + dsl::p<term_expr> + dsl::p<ws> + dsl::lit_c<']'>;
                auto field_access = dsl::lit_c<'.'> >> dsl::p<identifier>;
                return dsl::p<column_name> + dsl::opt(subscript | field_access);
            }();
            static constexpr auto value = lexy::callback<SimpleSelection>(
                [](ColumnName&& col, lexy::nullopt) -> SimpleSelection { return {.column = move(col), .access = {}}; },
                [](ColumnName&& col, Term&& idx) -> SimpleSelection {
                    return {.column = move(col), .access = TaggedUnion<SimpleSelection::Subscript, SimpleSelection::FieldAccess>(SimpleSelection::Subscript{move(idx)})};
                },
                [](ColumnName&& col, AutoString8&& field) -> SimpleSelection {
                    return {.column = move(col), .access = TaggedUnion<SimpleSelection::Subscript, SimpleSelection::FieldAccess>(SimpleSelection::FieldAccess{move(field)})};
                }
            );
        };

        struct condition {
            static constexpr auto rule = [] {
                return dsl::p<simple_selection_rule> + dsl::p<ws> + dsl::p<comparison_op> + dsl::p<ws> + dsl::p<term_expr>;
            }();
            static constexpr auto value = lexy::callback<Condition>(
                [](SimpleSelection&& sel, Operator op, Term&& val) -> Condition {
                    return {.selection = move(sel), .op = op, .value = move(val)};
                }
            );
        };
        struct condition_list {
            static constexpr auto rule  = dsl::list(dsl::p<condition> + dsl::p<ws>, dsl::sep(kw_and >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Condition>;
        };
        struct if_clause {
            static constexpr auto rule = [] {
                auto exists     = dsl::peek(kw_if + dsl::p<ws> + kw_exists) >> kw_if + dsl::p<ws> + kw_exists;
                auto conditions = kw_if >> dsl::p<ws> + dsl::p<condition_list>;
                return exists | conditions;
            }();
            static constexpr auto value = lexy::callback<IfClause>(
                []() -> IfClause { return IfClause{IfExists{}}; },
                [](DynamicArray<Condition>&& conds) -> IfClause { return IfClause{IfConditions{.conditions = move(conds)}}; }
            );
        };

        struct using_term {
            static constexpr auto rule  = dsl::p<bind_marker> | dsl::p<integer_literal>;
            static constexpr auto value = lexy::callback<TaggedUnion<S64, BindMarker>>(
                [](Term&& t) -> TaggedUnion<S64, BindMarker> {
                    return TaggedUnion<S64, BindMarker>{get<BindMarker>(t.value)};
                },
                [](S64 val) -> TaggedUnion<S64, BindMarker> {
                    return TaggedUnion<S64, BindMarker>{val};
                }
            );
        };
        struct using_timestamp_param {
            static constexpr auto rule  = kw_timestamp >> dsl::p<ws> + dsl::p<using_term>;
            static constexpr auto value = lexy::callback<UpdateParameter>(
                [](TaggedUnion<S64, BindMarker>&& val) -> UpdateParameter {
                    return {.kind = UpdateParameter::Kind::TIMESTAMP, .value = move(val)};
                }
            );
        };
        struct using_ttl_param {
            static constexpr auto rule  = kw_ttl >> dsl::p<ws> + dsl::p<using_term>;
            static constexpr auto value = lexy::callback<UpdateParameter>(
                [](TaggedUnion<S64, BindMarker>&& val) -> UpdateParameter {
                    return {.kind = UpdateParameter::Kind::TTL, .value = move(val)};
                }
            );
        };
        struct using_params_list {
            // Items include trailing ws so the AND separator can match without leading-space lookahead.
            static constexpr auto rule = dsl::list(
                (dsl::p<using_timestamp_param> | dsl::p<using_ttl_param>)+dsl::p<ws>,
                dsl::sep(kw_and >> dsl::p<ws>)
            );
            static constexpr auto value = as_dyn_arr<UpdateParameter>;
        };
        struct using_clause {
            static constexpr auto rule = [] {
                return dsl::opt(kw_using >> dsl::p<ws> + dsl::p<using_params_list>);
            }();
            static constexpr auto value = lexy::callback<DynamicArray<UpdateParameter>>(
                [](lexy::nullopt) -> DynamicArray<UpdateParameter> { return {}; },
                [](DynamicArray<UpdateParameter>&& params) -> DynamicArray<UpdateParameter> { return move(params); }
            );
        };

        // @note no ambiguity so we can define a map literal directly
        struct option_map_literal {
            static constexpr auto rule = [] {
                auto content = dsl::peek_not(dsl::lit_c<'}'>) >> dsl::p<map_literal_inner>;
                return dsl::lit_c<'{'> >> dsl::p<ws> + dsl::opt(content) + dsl::p<ws> + dsl::lit_c<'}'>;
            }();
            static constexpr auto value = lexy::callback<MapLiteral>(
                [](lexy::nullopt) -> MapLiteral { return MapLiteral{}; },
                [](DynamicArray<Pair<Term, Term>>&& entries) -> MapLiteral { return {.key_values = move(entries)}; }
            );
        };
        struct option_pair_rule {
            static constexpr auto rule = [] {
                return dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'='> + dsl::p<ws> + (dsl::p<option_map_literal> | dsl::p<constant_value> | dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::callback<OptionPair>(
                [](AutoString8&& key, MapLiteral&& m) -> OptionPair { return {move(key), move(m)}; },
                [](AutoString8&& key, Constant&& cv) -> OptionPair { return {move(key), move(cv)}; },
                [](AutoString8&& key, AutoString8&& id) -> OptionPair { return {move(key), move(id)}; }
            );
        };
        struct option_pairs_list {
            static constexpr auto rule  = dsl::list(dsl::p<option_pair_rule> + dsl::p<ws>, dsl::sep(kw_and >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<OptionPair>;
        };
        struct with_options {
            static constexpr auto rule = [] {
                return dsl::opt(kw_with >> dsl::p<ws> + dsl::p<option_pairs_list>);
            }();
            static constexpr auto value = lexy::callback<Options>(
                [](lexy::nullopt) -> Options { return {}; },
                [](DynamicArray<OptionPair>&& pairs) -> Options { return {.identifier_values = move(pairs)}; }
            );
        };

        struct with_mask_function_or_default {
            static constexpr auto rule = [] {
                auto default_ = kw_default >> dsl::nullopt;
                auto function = dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + dsl::p<term_args_list> + dsl::p<ws> + dsl::lit_c<')'>;
                return default_ | dsl::else_ >> function;
            }();
            static constexpr auto value = lexy::callback<ColumnMask>(
                [](AutoString8&& name, DynamicArray<Term>&& args) -> ColumnMask { return {
                                                                                      .mask_function = FunctionCall{move(name), move(args)}
 }; },
                [](lexy::nullopt) -> ColumnMask { return {}; }
            );
        };
        struct mask_with {
            static constexpr auto rule = [] {
                return kw_mask + dsl::p<with_mask_function_or_default>;
            }();
            static constexpr auto value = lexy::forward<ColumnMask>;
        };
        struct opt_static_kw {
            static constexpr auto rule  = dsl::opt(dsl::peek(kw_static) >> kw_static);
            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };
        struct opt_pk_kw {
            static constexpr auto rule  = dsl::opt(dsl::peek(kw_primary) >> kw_primary + dsl::p<ws> + kw_key);
            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };
        struct opt_mask {
            static constexpr auto rule  = dsl::opt(dsl::peek(kw_mask) >> dsl::p<mask_with>);
            static constexpr auto value = lexy::callback<Optional<ColumnMask>>(
                [](lexy::nullopt) -> Optional<ColumnMask> { return {}; },
                [](ColumnMask&& m) -> Optional<ColumnMask> { return move(m); }
            );
        };
        struct column_definition_rule {
            static constexpr auto rule = [] {
                return dsl::p<column_name> + dsl::p<ws> + dsl::p<data_type> + dsl::p<ws> + dsl::p<opt_static_kw> + dsl::p<ws> + dsl::p<opt_mask> + dsl::p<ws> + dsl::p<opt_pk_kw>;
            }();
            static constexpr auto value = lexy::construct<ColumnDefinition>;
        };

        // ====================================================================
        // Data definition (DDL)
        // ====================================================================
        struct use_stmt {
            static constexpr auto rule = [] {
                return dsl::peek(kw_use) >> kw_use + dsl::p<ws> + dsl::p<identifier>;
            }();
            static constexpr auto value = lexy::construct<UseKeyspace>;
        };

        struct create_keyspace_stmt {
            static constexpr auto rule = [] {
                auto key = kw_create + dsl::p<ws> + kw_keyspace;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_not_exists> + dsl::p<ws> + dsl::p<identifier> + dsl::p<ws> + dsl::p<with_options>;
            }();
            static constexpr auto value = lexy::construct<CreateKeyspace>;
        };

        struct alter_keyspace_stmt {
            static constexpr auto rule = [] {
                auto key = kw_alter + dsl::p<ws> + kw_keyspace;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<identifier> + dsl::p<ws> + dsl::p<with_options>;
            }();
            static constexpr auto value = lexy::construct<AlterKeyspace>;
        };

        struct drop_keyspace_stmt {
            static constexpr auto rule = [] {
                auto key = kw_drop + dsl::p<ws> + kw_keyspace;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<identifier>;
            }();
            static constexpr auto value = lexy::construct<DropKeyspace>;
        };

        struct table_primary_key {
            static constexpr auto rule = [] {
                auto single    = dsl::p<column_name>;
                auto composite = dsl::parenthesized(dsl::p<ws> + dsl::p<column_name_list> + dsl::p<ws>);
                return composite | single;
            }();
            // @todo why does forward not work here?
            static constexpr auto value = lexy::callback<CreateTable::PartitionKey>(
                [](DynamicArray<ColumnName>&& cols) -> CreateTable::PartitionKey {
                    return {.column_or_columns = move(cols)};
                },
                [](ColumnName&& col) -> CreateTable::PartitionKey {
                    return {.column_or_columns = move(col)};
                }
            );
        };
        struct opt_clustering_columns {
            static constexpr auto rule  = dsl::opt(dsl::lit_c<','> >> dsl::p<ws> + dsl::p<column_name_list>);
            static constexpr auto value = lexy::callback<CreateTable::ClusteringColumns>(
                [](lexy::nullopt) -> CreateTable::ClusteringColumns { return {}; },
                [](DynamicArray<ColumnName>&& cols) -> CreateTable::ClusteringColumns { return move(cols); }
            );
        };
        struct primary_key_clause {
            static constexpr auto rule = [] {
                return kw_primary >> dsl::p<ws> + kw_key + dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + dsl::p<table_primary_key> + dsl::p<ws> + dsl::p<opt_clustering_columns> + dsl::p<ws> + dsl::lit_c<')'>;
            }();
            static constexpr auto value = lexy::construct<CreateTable::PrimaryKey>;
        };
        struct col_leading_comma_opt : lexy::transparent_production {
            static constexpr auto rule  = dsl::opt(dsl::lit_c<','> >> dsl::p<ws>);
            static constexpr auto value = lexy::noop;
        };
        struct col_or_pk_el {
            static constexpr auto rule  = dsl::p<col_leading_comma_opt> + (dsl::p<primary_key_clause> | dsl::else_ >> dsl::p<column_definition_rule>);
            static constexpr auto value = lexy::forward<TaggedUnion<ColumnDefinition, CreateTable::PrimaryKey>>;
        };
        struct col_or_pk_list {
            static constexpr auto rule = dsl::list(
                dsl::peek_not(dsl::lit_c<')'>) >> dsl::p<col_or_pk_el> + dsl::p<ws>,
                dsl::trailing_sep(dsl::lit_c<','> >> dsl::p<ws>)
            );
            static constexpr auto value = as_dyn_arr<TaggedUnion<ColumnDefinition, CreateTable::PrimaryKey>>;
        };
        struct column_def_list {
            static constexpr auto rule = [] {
                return dsl::parenthesized(dsl::p<ws> + dsl::p<col_or_pk_list> + dsl::p<ws>);
            }();
            static constexpr auto value = lexy::callback<Pair<DynamicArray<ColumnDefinition>, Optional<CreateTable::PrimaryKey>>>(
                [](DynamicArray<TaggedUnion<ColumnDefinition, CreateTable::PrimaryKey>>&& els) {
                    DynamicArray<ColumnDefinition>    cols;
                    Optional<CreateTable::PrimaryKey> pk;
                    for (auto& el : els) {
                        visit(el, [&](auto& v) {
                            using T = Decay<decltype(v)>;
                            if constexpr (SameAs<T, ColumnDefinition>) {
                                push_back(cols, move(v));
                            } else if constexpr (SameAs<T, CreateTable::PrimaryKey>) {
                                pk = move(v);
                            }
                        });
                    }
                    return Pair<DynamicArray<ColumnDefinition>, Optional<CreateTable::PrimaryKey>>{move(cols), move(pk)};
                }
            );
        };

        using TableOption = decltype(CreateTable::TableOptions::value)::Element;
        struct compact_storage_option {
            static constexpr auto rule = [] {
                // Peek "compact" only when followed by non-identifier char (not "compaction" etc.)
                auto branch = dsl::peek(kw_compact + dsl::peek_not(dsl::ascii::alpha_digit_underscore));
                return branch >> kw_compact + dsl::p<ws> + kw_storage;
            }();
            static constexpr auto value = lexy::callback<TableOption>(
                []() -> TableOption { return TableOption{CreateTable::CompactStorage{}}; }
            );
        };
        struct sort_order {
            struct asc : lexy::transparent_production {
                static constexpr auto rule  = kw_asc;
                static constexpr auto value = lexy::constant(Sort::ASC);
            };
            struct desc : lexy::transparent_production {
                static constexpr auto rule  = kw_desc;
                static constexpr auto value = lexy::constant(Sort::DESC);
            };
            static constexpr auto rule  = dsl::opt(dsl::p<asc> | dsl::p<desc>);
            static constexpr auto value = lexy::callback<Sort>(
                // @note default ASC
                [](lexy::nullopt) { return Sort::ASC; },
                [](Sort s) { return s; }
            );
        };
        struct clustering_order_el {
            static constexpr auto rule  = dsl::p<column_name> + dsl::p<ws> + dsl::p<sort_order>;
            static constexpr auto value = lexy::construct<CreateTable::ColumnOrder>;
        };
        struct clustering_order_list {
            static constexpr auto rule  = dsl::list(dsl::p<clustering_order_el> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<CreateTable::ColumnOrder>;
        };
        struct clustering_order_option {
            static constexpr auto rule  = kw_clustering >> dsl::p<ws> + kw_order + dsl::p<ws> + kw_by + dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + dsl::p<clustering_order_list> + dsl::p<ws> + dsl::lit_c<')'>;
            static constexpr auto value = lexy::callback<TableOption>(
                [](DynamicArray<CreateTable::ColumnOrder>&& orders) -> TableOption {
                    return TableOption{CreateTable::ClusteringOrder{.column_orders = move(orders)}};
                }
            );
        };
        struct regular_option {
            static constexpr auto rule  = dsl::p<option_pair_rule>;
            static constexpr auto value = lexy::callback<TableOption>(
                [](OptionPair&& pair) -> TableOption { return TableOption{move(pair)}; }
            );
        };
        struct table_option_el {
            static constexpr auto rule  = dsl::p<compact_storage_option> | dsl::p<clustering_order_option> | dsl::else_ >> dsl::p<regular_option>;
            static constexpr auto value = lexy::forward<TableOption>;
        };
        struct table_options_list {
            static constexpr auto rule  = dsl::list(dsl::p<table_option_el> + dsl::p<ws>, dsl::sep(kw_and >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<TableOption>;
        };
        struct table_with_options {
            static constexpr auto rule = [] {
                return dsl::opt(kw_with >> dsl::p<ws> + dsl::p<table_options_list>);
            }();
            static constexpr auto value = lexy::callback<Optional<CreateTable::TableOptions>>(
                [](lexy::nullopt) -> Optional<CreateTable::TableOptions> { return {}; },
                [](DynamicArray<TableOption>&& opts) -> Optional<CreateTable::TableOptions> {
                    return CreateTable::TableOptions{.value = move(opts)};
                }
            );
        };
        struct create_table_stmt {
            static constexpr auto rule = [] {
                auto key = kw_create + dsl::p<ws> + (kw_table | kw_columnfamily);
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_not_exists> + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + dsl::p<column_def_list> + dsl::p<ws> + dsl::p<table_with_options>;
            }();
            static constexpr auto value = lexy::callback<CreateTable>(
                [](bool ine, TableName&& name, Pair<DynamicArray<ColumnDefinition>, Optional<CreateTable::PrimaryKey>>&& cols_pk, Optional<CreateTable::TableOptions>&& opts) -> CreateTable {
                    CreateTable ct;
                    ct.if_not_exists      = ine;
                    ct.name               = move(name);
                    ct.column_definitions = move(cols_pk.first);
                    ct.primary_key        = move(cols_pk.second);
                    if (opts) {
                        ct.options = move(*opts);
                    }
                    return ct;
                }
            );
        };

        struct column_definition_list {
            static constexpr auto rule  = dsl::list(dsl::p<column_definition_rule> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<ColumnDefinition>;
        };
        struct alter_table_add {
            static constexpr auto rule  = kw_add >> dsl::p<ws> + dsl::p<if_not_exists> + dsl::p<ws> + dsl::p<column_definition_list>;
            static constexpr auto value = lexy::callback<AlterTable>(
                [](bool if_not_exists, DynamicArray<ColumnDefinition>&& cols) -> AlterTable {
                    return {
                        .if_exists               = {},
                        .table                   = {},
                        .alter_table_instruction = AlterTable::AddColumnInstruction{.if_not_exists = if_not_exists, .column_definitions = move(cols)}
                    };
                }
            );
        };
        struct drop_column_list {
            static constexpr auto rule =
                dsl::parenthesized(dsl::p<ws> + dsl::p<column_name_list> + dsl::p<ws>) | dsl::else_ >> dsl::p<column_name_list>;
            static constexpr auto value = lexy::forward<DynamicArray<ColumnName>>;
        };
        struct alter_table_drop {
            // Optional USING TIMESTAMP n at the end (parsed and discarded — engine ignores it).
            static constexpr auto rule  = kw_drop >> dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<drop_column_list> + dsl::p<ws> + dsl::p<using_clause>;
            static constexpr auto value = lexy::callback<AlterTable>(
                [](bool if_exists, DynamicArray<ColumnName>&& cols, DynamicArray<UpdateParameter>&&) -> AlterTable {
                    return {
                        .if_exists               = {},
                        .table                   = {},
                        .alter_table_instruction = AlterTable::DropColumnInstruction{.if_exists = if_exists, .columns = move(cols)}
                    };
                }
            );
        };
        struct rename_pair {
            static constexpr auto rule  = dsl::p<column_name> + dsl::p<ws> + kw_to + dsl::p<ws> + dsl::p<column_name>;
            static constexpr auto value = lexy::construct<Pair<ColumnName, ColumnName>>;
        };
        struct rename_pairs_list {
            static constexpr auto rule  = dsl::list(dsl::p<rename_pair> + dsl::p<ws>, dsl::sep(kw_and >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Pair<ColumnName, ColumnName>>;
        };
        struct alter_table_rename {
            static constexpr auto rule = [] {
                return kw_rename >> dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<rename_pairs_list>;
            }();
            static constexpr auto value = lexy::callback<AlterTable>(
                [](bool if_exists, DynamicArray<Pair<ColumnName, ColumnName>>&& pairs) -> AlterTable {
                    return {
                        .if_exists               = {},
                        .table                   = {},
                        .alter_table_instruction = AlterTable::RenameColumnInstruction{.if_exists = if_exists, .old_to_new_columns = move(pairs)}
                    };
                }
            );
        };
        struct alter_table_alter {
            static constexpr auto rule = [] {
                auto masked_with = kw_masked >> dsl::p<ws> + dsl::p<with_mask_function_or_default>;
                auto drop_mask   = kw_drop >> dsl::p<ws> + kw_masked;
                return kw_alter >> dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<column_name> + dsl::p<ws> + (masked_with | drop_mask);
            }();
            static constexpr auto value = lexy::callback<AlterTable>(
                [](bool if_exists, ColumnName&& col, ColumnMask&& mask) -> AlterTable {
                    return {
                        .if_exists               = {},
                        .table                   = {},
                        .alter_table_instruction = AlterTable::AlterColumnInstruction{.if_exists = if_exists, .column = move(col), .column_mask = move(mask)}
                    };
                },
                [](bool if_exists, ColumnName&& col) -> AlterTable {
                    return {
                        .if_exists               = {},
                        .table                   = {},
                        .alter_table_instruction = AlterTable::AlterColumnInstruction{.if_exists = if_exists, .column = move(col), .column_mask = {}}
                    };
                }
            );
        };
        struct alter_table_with {
            static constexpr auto rule  = dsl::p<with_options>;
            static constexpr auto value = lexy::callback<AlterTable>(
                [](Options&& opts) -> AlterTable {
                    return {.if_exists = {}, .table = {}, .alter_table_instruction = move(opts)};
                }
            );
        };
        struct alter_table_stmt {
            static constexpr auto rule = [] {
                auto key         = kw_alter + dsl::p<ws> + (kw_table | kw_columnfamily);
                auto instruction = dsl::p<alter_table_add> | dsl::p<alter_table_drop> | dsl::p<alter_table_rename> | dsl::p<alter_table_alter> | dsl::else_ >> dsl::p<alter_table_with>;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + instruction;
            }();
            static constexpr auto value = lexy::callback<AlterTable>(
                [](bool ie, TableName&& tbl, AlterTable&& at) -> AlterTable {
                    at.if_exists = ie;
                    at.table     = move(tbl);
                    return move(at);
                }
            );
        };

        struct drop_table_stmt {
            static constexpr auto rule = [] {
                auto key = kw_drop + dsl::p<ws> + (kw_table | kw_columnfamily);
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<table_name>;
            }();
            static constexpr auto value = lexy::construct<DropTable>;
        };

        struct truncate_stmt {
            static constexpr auto rule = [] {
                auto opt_table = dsl::opt(dsl::peek(kw_table | kw_columnfamily) >> (kw_table | kw_columnfamily) + dsl::p<ws>);
                return dsl::peek(kw_truncate) >> kw_truncate + dsl::p<ws> + opt_table + dsl::p<table_name>;
            }();
            static constexpr auto value = lexy::callback<TruncateTable>(
                [](lexy::nullopt, TableName&& tbl) -> TruncateTable { return {.table = move(tbl)}; },
                [](TableName&& tbl) -> TruncateTable { return {.table = move(tbl)}; }
            );
        };

        struct create_index_stmt {
            struct opt_custom_kw : lexy::transparent_production {
                static constexpr auto rule  = dsl::opt(dsl::peek(kw_custom) >> kw_custom + dsl::p<ws>);
                static constexpr auto value = lexy::callback<bool>(
                    [](lexy::nullopt) { return false; },
                    []() { return true; }
                );
            };
            // Only consume an identifier as the index name when the token after it is ON.
            struct opt_index_name : lexy::transparent_production {
                static constexpr auto rule = dsl::opt(
                    dsl::peek(dsl::p<identifier> + dsl::p<ws> + kw_on) >> dsl::p<identifier>
                );
                static constexpr auto value = lexy::callback<Optional<AutoString8>>(
                    [](lexy::nullopt) -> Optional<AutoString8> { return {}; },
                    [](AutoString8&& name) -> Optional<AutoString8> { return move(name); }
                );
            };
            // Index column specifier: identifier or func(identifier) — returns (selector, col_name).
            struct index_col_spec : lexy::transparent_production {
                using Result               = Pair<Optional<AutoString8>, AutoString8>;
                static constexpr auto rule = [] {
                    auto fn_form = dsl::peek(dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'('>) >> dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<')'>;
                    return fn_form | dsl::else_ >> dsl::p<identifier>;
                }();
                static constexpr auto value = lexy::callback<Result>(
                    [](AutoString8&& selector, AutoString8&& col_name) -> Result { return {Optional<AutoString8>{move(selector)}, move(col_name)}; },
                    [](AutoString8&& col_name) -> Result { return {Optional<AutoString8>{}, move(col_name)}; }
                );
            };

            static constexpr auto rule = [] {
                auto key_no_custom = kw_create + dsl::p<ws> + kw_index;
                auto key_custom    = kw_create + dsl::p<ws> + kw_custom + dsl::p<ws> + kw_index;
                auto cols          = dsl::parenthesized(dsl::p<ws> + dsl::p<index_col_spec> + dsl::p<ws>);
                return (dsl::peek(key_custom) | dsl::peek(key_no_custom)) >> kw_create + dsl::p<ws> + dsl::p<opt_custom_kw> + kw_index + dsl::p<ws> + dsl::p<if_not_exists> + dsl::p<ws> + dsl::p<opt_index_name> + dsl::p<ws> + kw_on + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + cols;
            }();
            static constexpr auto value = lexy::callback<CreateIndex>(
                [](bool custom, bool if_not_exists, Optional<AutoString8>&& index_name, TableName&& table, Pair<Optional<AutoString8>, AutoString8>&& col_spec) -> CreateIndex {
                    return {.custom = custom, .if_not_exists = if_not_exists, .index_name = move(index_name), .table = move(table), .column_name = move(col_spec.second), .selector = move(col_spec.first)};
                }
            );
        };

        struct drop_index_stmt {
            static constexpr auto rule = [] {
                auto key = kw_drop + dsl::p<ws> + kw_index;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<table_name>;
            }();
            static constexpr auto value = lexy::construct<DropIndex>;
        };

        struct create_type_stmt {
            static constexpr auto rule = [] {
                auto key = kw_create + dsl::p<ws> + kw_type;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_not_exists> + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + dsl::parenthesized(dsl::p<ws> + dsl::p<column_definition_list> + dsl::p<ws>);
            }();
            static constexpr auto value = lexy::callback<CreateType>(
                [](bool if_not_exists, TableName&& name, DynamicArray<ColumnDefinition>&& fields) -> CreateType {
                    return {.if_not_exists = if_not_exists, .name = move(name), .fields = move(fields)};
                }
            );
        };

        struct drop_type_stmt {
            static constexpr auto rule = [] {
                auto key = kw_drop + dsl::p<ws> + kw_type;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<if_exists> + dsl::p<ws> + dsl::p<table_name>;
            }();
            static constexpr auto value = lexy::construct<DropType>;
        };

        struct alter_type_add {
            static constexpr auto rule  = kw_add >> dsl::p<ws> + dsl::p<column_definition_list>;
            static constexpr auto value = lexy::callback<AlterType::AddFieldInstruction>(
                [](DynamicArray<ColumnDefinition>&& fields) -> AlterType::AddFieldInstruction {
                    return {.fields = move(fields)};
                }
            );
        };
        struct alter_type_rename_pair {
            static constexpr auto rule  = dsl::p<column_name> + dsl::p<ws> + kw_to + dsl::p<ws> + dsl::p<column_name>;
            static constexpr auto value = lexy::construct<Pair<ColumnName, ColumnName>>;
        };
        struct alter_type_rename_list {
            static constexpr auto rule  = dsl::list(dsl::p<alter_type_rename_pair> + dsl::p<ws>, dsl::sep(kw_and >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Pair<ColumnName, ColumnName>>;
        };
        struct alter_type_rename {
            static constexpr auto rule  = kw_rename >> dsl::p<ws> + dsl::p<alter_type_rename_list>;
            static constexpr auto value = lexy::callback<AlterType::RenameFieldInstruction>(
                [](DynamicArray<Pair<ColumnName, ColumnName>>&& pairs) -> AlterType::RenameFieldInstruction {
                    return {.old_to_new_fields = move(pairs)};
                }
            );
        };
        struct alter_type_stmt {
            static constexpr auto rule = [] {
                auto key         = kw_alter + dsl::p<ws> + kw_type;
                auto instruction = dsl::p<alter_type_add> | dsl::else_ >> dsl::p<alter_type_rename>;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + instruction;
            }();
            static constexpr auto value = lexy::callback<AlterType>(
                [](TableName&& name, AlterType::AddFieldInstruction&& ins) -> AlterType {
                    return {.name = move(name), .instruction = move(ins)};
                },
                [](TableName&& name, AlterType::RenameFieldInstruction&& ins) -> AlterType {
                    return {.name = move(name), .instruction = move(ins)};
                }
            );
        };

        // ====================================================================
        // Data manipulation (DML)
        // ====================================================================

        struct insert_names_values {
            static constexpr auto rule = [] {
                auto names  = dsl::parenthesized(dsl::p<ws> + dsl::p<column_name_list> + dsl::p<ws>);
                auto values = dsl::parenthesized(dsl::p<ws> + dsl::p<term_args_list> + dsl::p<ws>);
                return names + dsl::p<ws> + kw_values + dsl::p<ws> + values;
            }();
            static constexpr auto value = lexy::construct<Insert::NamesValues>;
        };
        struct insert_positional_values {
            static constexpr auto rule = [] {
                auto values = dsl::parenthesized(dsl::p<ws> + dsl::p<term_args_list> + dsl::p<ws>);
                return kw_values >> dsl::p<ws> + values;
            }();
            static constexpr auto value = lexy::callback<Insert::NamesValues>(
                [](DynamicArray<Term>&& vals) -> Insert::NamesValues {
                    return {.names = {}, .values = move(vals)};
                }
            );
        };
        struct json_default_kind {
            struct null_default : lexy::transparent_production {
                static constexpr auto rule  = kw_null;
                static constexpr auto value = lexy::constant(Insert::JsonClause::Default::NUL);
            };
            struct unset_default : lexy::transparent_production {
                static constexpr auto rule  = kw_unset;
                static constexpr auto value = lexy::constant(Insert::JsonClause::Default::UNSET);
            };
            static constexpr auto rule  = dsl::p<null_default> | dsl::p<unset_default>;
            static constexpr auto value = lexy::forward<Insert::JsonClause::Default>;
        };
        struct insert_json {
            static constexpr auto rule = [] {
                auto default_clause = dsl::opt(kw_default >> dsl::p<ws> + dsl::p<json_default_kind>);
                return kw_json >> dsl::p<ws> + dsl::p<string_literal> + dsl::p<ws> + default_clause;
            }();
            static constexpr auto value = lexy::callback<Insert::JsonClause>(
                [](AutoString8&& json, lexy::nullopt) -> Insert::JsonClause {
                    return {.string = move(json), .default_ = Insert::JsonClause::Default::UNSET};
                },
                [](AutoString8&& json, Insert::JsonClause::Default d) -> Insert::JsonClause {
                    return {.string = move(json), .default_ = d};
                }
            );
        };
        struct insert_stmt {
            static constexpr auto rule = [] {
                auto key    = kw_insert + dsl::p<ws> + kw_into;
                auto clause = dsl::p<insert_json> | dsl::p<insert_names_values> | dsl::p<insert_positional_values>;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + clause + dsl::p<ws> + dsl::p<if_not_exists> + dsl::p<ws> + dsl::p<using_clause>;
            }();
            static constexpr auto value = lexy::construct<Insert>;
        };

        struct assignment_rule {
            static constexpr auto rule = [] {
                return dsl::p<simple_selection_rule> + dsl::p<ws> + dsl::lit_c<'='> + dsl::p<ws> + dsl::p<term_with_identifiers_expr>;
            }();
            static constexpr auto value = lexy::construct<Update::Assignment>;
        };
        struct assignment_list {
            static constexpr auto rule  = dsl::list(dsl::p<assignment_rule> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Update::Assignment>;
        };
        struct opt_if_clause {
            static constexpr auto rule  = dsl::opt(dsl::p<if_clause>);
            static constexpr auto value = lexy::callback<Optional<IfClause>>(
                [](lexy::nullopt) -> Optional<IfClause> { return {}; },
                [](IfClause&& ifc) -> Optional<IfClause> { return move(ifc); }
            );
        };
        struct update_stmt {
            static constexpr auto rule = [] {
                auto key = kw_update;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + dsl::p<using_clause> + dsl::p<ws> + kw_set + dsl::p<ws> + dsl::p<assignment_list> + dsl::p<ws> + dsl::p<where_clause> + dsl::p<ws> + dsl::p<opt_if_clause>;
            }();
            static constexpr auto value = lexy::construct<Update>;
        };

        struct simple_selection_list {
            static constexpr auto rule  = dsl::list(dsl::p<simple_selection_rule> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<SimpleSelection>;
        };
        struct delete_selections {
            static constexpr auto rule = [] {
                return dsl::opt(dsl::peek_not(kw_from) >> dsl::p<simple_selection_list>);
            }();
            static constexpr auto value = lexy::callback<DynamicArray<SimpleSelection>>(
                [](lexy::nullopt) -> DynamicArray<SimpleSelection> { return {}; },
                [](DynamicArray<SimpleSelection>&& sels) -> DynamicArray<SimpleSelection> { return move(sels); }
            );
        };
        struct delete_stmt {
            static constexpr auto rule = [] {
                auto key = kw_delete;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<delete_selections> + dsl::p<ws> + kw_from + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + dsl::p<using_clause> + dsl::p<ws> + dsl::p<where_clause> + dsl::p<ws> + dsl::p<opt_if_clause>;
            }();
            static constexpr auto value = lexy::construct<Delete>;
        };

        struct batch_kind {
            struct logged : lexy::transparent_production {
                static constexpr auto rule  = kw_logged;
                static constexpr auto value = lexy::constant(Batch::Kind::LOGGED);
            };
            struct unlogged : lexy::transparent_production {
                static constexpr auto rule  = kw_unlogged;
                static constexpr auto value = lexy::constant(Batch::Kind::UNLOGGED);
            };
            struct counter : lexy::transparent_production {
                static constexpr auto rule  = kw_counter;
                static constexpr auto value = lexy::constant(Batch::Kind::COUNTER);
            };
            static constexpr auto rule  = dsl::opt(dsl::p<unlogged> | dsl::p<counter> | dsl::p<logged>);
            static constexpr auto value = lexy::callback<Batch::Kind>(
                // @note defaults to LOGGED
                [](lexy::nullopt) { return Batch::Kind::LOGGED; },
                [](Batch::Kind k) { return k; }
            );
        };
        struct batch_modification {
            static constexpr auto rule  = (dsl::p<insert_stmt> | dsl::p<update_stmt> | dsl::p<delete_stmt>)+dsl::p<ws> + dsl::lit_c<';'>;
            static constexpr auto value = lexy::construct<Batch::ModificationStatement>;
        };
        struct batch_modifications_inner {
            static constexpr auto rule  = dsl::list(dsl::peek_not(dsl::p<ws> + kw_apply) >> dsl::p<ws> + dsl::p<batch_modification>);
            static constexpr auto value = as_dyn_arr<Batch::ModificationStatement>;
        };
        struct batch_modifications_list {
            static constexpr auto rule = dsl::opt(
                dsl::peek_not(dsl::p<ws> + kw_apply) >> dsl::p<batch_modifications_inner>
            );
            static constexpr auto value = lexy::callback<DynamicArray<Batch::ModificationStatement>>(
                [](lexy::nullopt) -> DynamicArray<Batch::ModificationStatement> { return {}; },
                [](DynamicArray<Batch::ModificationStatement>&& stmts) -> DynamicArray<Batch::ModificationStatement> { return move(stmts); }
            );
        };
        struct batch_stmt {
            static constexpr auto rule = [] {
                auto key = kw_begin;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<batch_kind> + dsl::p<ws> + kw_batch + dsl::p<ws> + dsl::p<using_clause> + dsl::p<ws> + dsl::p<batch_modifications_list> + dsl::p<ws> + kw_apply + dsl::p<ws> + kw_batch;
            }();
            static constexpr auto value = lexy::construct<Batch>;
        };

        // ====================================================================
        // Queries
        // ====================================================================
        struct selector_rule;
        struct count_selector {
            static constexpr auto rule  = kw_count >> dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + (dsl::lit_c<'*'> | dsl::lit_c<'1'>)+dsl::p<ws> + dsl::lit_c<')'>;
            static constexpr auto value = lexy::callback<Select::Selector>(
                []() -> Select::Selector { return {.value = Select::Count{}}; }
            );
        };
        struct cast_selector {
            static constexpr auto rule  = kw_cast >> dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + dsl::recurse<selector_rule> + dsl::p<ws> + kw_as + dsl::p<ws> + dsl::p<data_type> + dsl::p<ws> + dsl::lit_c<')'>;
            static constexpr auto value = lexy::callback<Select::Selector>(
                [](Select::Selector&& sel, type::Type type) -> Select::Selector {
                    return {
                        .value = Select::Cast{.column = move(sel), .to = type}
                    };
                }
            );
        };
        struct selector_args_list {
            static constexpr auto rule  = dsl::list(dsl::recurse<selector_rule> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Select::Selector>;
        };
        struct function_selector {
            static constexpr auto rule = [] {
                auto args     = dsl::opt(dsl::peek_not(dsl::lit_c<')'>) >> dsl::p<selector_args_list>);
                auto id_paren = dsl::peek(dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'('>);
                return id_paren >> dsl::p<identifier> + dsl::p<ws> + dsl::lit_c<'('> + dsl::p<ws> + args + dsl::p<ws> + dsl::lit_c<')'>;
            }();
            static constexpr auto value = lexy::callback<Select::Selector>(
                [](AutoString8&& name, lexy::nullopt) -> Select::Selector {
                    return {
                        .value = Select::Function{.function_name = move(name), .arguments = {}}
                    };
                },
                [](AutoString8&& name, DynamicArray<Select::Selector>&& args) -> Select::Selector {
                    return {
                        .value = Select::Function{.function_name = move(name), .arguments = move(args)}
                    };
                }
            );
        };
        struct column_selector {
            static constexpr auto rule  = dsl::p<column_name>;
            static constexpr auto value = lexy::construct<Select::Selector>;
        };
        struct term_selector {
            static constexpr auto rule  = dsl::p<term_expr>;
            static constexpr auto value = lexy::construct<Select::Selector>;
        };
        struct selector_rule {
            static constexpr auto rule  = dsl::p<count_selector> | dsl::p<cast_selector> | dsl::p<function_selector> | dsl::p<column_selector>;
            static constexpr auto value = lexy::forward<Select::Selector>;
        };
        struct select_column {
            static constexpr auto rule = [] {
                return dsl::p<selector_rule> + dsl::p<ws> + dsl::opt(kw_as >> dsl::p<ws> + dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::callback<Select::SelectColumn>(
                [](Select::Selector&& sel, lexy::nullopt) -> Select::SelectColumn { return {.column = move(sel), .as = {}}; },
                [](Select::Selector&& sel, AutoString8&& alias) -> Select::SelectColumn { return {.column = move(sel), .as = move(alias)}; }
            );
        };
        struct select_columns_list {
            static constexpr auto rule  = dsl::list(dsl::p<select_column> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Select::SelectColumn>;
        };
        struct select_clause {
            struct star : lexy::transparent_production {
                static constexpr auto rule  = dsl::lit_c<'*'>;
                static constexpr auto value = lexy::construct<Select::SelectClause>;
            };
            struct columns {
                static constexpr auto rule  = dsl::p<select_columns_list>;
                static constexpr auto value = lexy::construct<Select::SelectClause>;
            };
            static constexpr auto rule  = dsl::p<star> | dsl::else_ >> dsl::p<columns>;
            static constexpr auto value = lexy::forward<Select::SelectClause>;
        };
        struct order_by_el {
            static constexpr auto rule  = (dsl::member<&Select::ColumnOrderBy::column> = dsl::p<column_name>)+dsl::p<ws> + (dsl::member<&Select::ColumnOrderBy::sort> = dsl::p<sort_order>);
            static constexpr auto value = lexy::as_aggregate<Select::ColumnOrderBy>;
        };
        struct order_by_list {
            static constexpr auto rule  = dsl::list(dsl::p<order_by_el> + dsl::p<ws>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
            static constexpr auto value = as_dyn_arr<Select::ColumnOrderBy>;
        };
        struct order_by_clause {
            static constexpr auto rule = [] {
                return kw_order >> dsl::p<ws> + kw_by + dsl::p<ws> + dsl::p<order_by_list>;
            }();
            static constexpr auto value = lexy::construct<Select::OrderByClause>;
        };
        struct group_by_clause {
            static constexpr auto rule = [] {
                return kw_group >> dsl::p<ws> + kw_by + dsl::p<ws> + dsl::p<column_name_list>;
            }();
            static constexpr auto value = lexy::construct<Select::GroupByClause>;
        };
        struct limit_value {
            static constexpr auto rule  = dsl::p<bind_marker> | dsl::p<integer_literal>;
            static constexpr auto value = lexy::callback<Select::Limit>(
                [](Term&& t) -> Select::Limit { return Select::Limit{get<BindMarker>(t.value)}; },
                [](S64 val) -> Select::Limit { return Select::Limit{val}; }
            );
        };
        struct per_partition_limit_value {
            static constexpr auto rule  = dsl::p<bind_marker> | dsl::p<integer_literal>;
            static constexpr auto value = lexy::callback<Select::PerPartitionLimit>(
                [](Term&& t) -> Select::PerPartitionLimit { return Select::PerPartitionLimit{get<BindMarker>(t.value)}; },
                [](S64 val) -> Select::PerPartitionLimit { return Select::PerPartitionLimit{val}; }
            );
        };
        struct allow_filtering_clause {
            static constexpr auto rule = [] {
                auto key = kw_allow + dsl::p<ws> + kw_filtering;
                return dsl::opt(dsl::peek(key) >> key);
            }();
            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };
        struct select_transform {
            struct json : lexy::transparent_production {
                static constexpr auto rule  = kw_json;
                static constexpr auto value = lexy::constant(Select::Transform::JSON);
            };
            struct distinct : lexy::transparent_production {
                static constexpr auto rule  = kw_distinct;
                static constexpr auto value = lexy::constant(Select::Transform::UNIQUE);
            };
            // @note json/distinct are column names, not transforms, when immediately followed by ','
            static constexpr auto rule = dsl::opt(
                dsl::peek(kw_json + dsl::peek_not(dsl::lit_c<','>)) >> dsl::p<json> | dsl::peek(kw_distinct + dsl::peek_not(dsl::lit_c<','>)) >> dsl::p<distinct>
            );
            static constexpr auto value = lexy::callback<Optional<Select::Transform>>(
                [](lexy::nullopt) -> Optional<Select::Transform> { return {}; },
                [](Select::Transform t) -> Optional<Select::Transform> { return t; }
            );
        };
        struct select_stmt {
            static constexpr auto rule = [] {
                auto key = kw_select;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<select_transform> + dsl::p<ws> + dsl::p<select_clause> + dsl::p<ws> + kw_from + dsl::p<ws> + dsl::p<table_name> + dsl::p<ws> + dsl::opt(dsl::p<where_clause>) + dsl::p<ws> + dsl::opt(dsl::p<group_by_clause>) + dsl::p<ws> + dsl::opt(dsl::p<order_by_clause>) + dsl::p<ws> + dsl::opt(kw_per >> dsl::p<ws> + kw_partition + dsl::p<ws> + kw_limit + dsl::p<ws> + dsl::p<per_partition_limit_value>) + dsl::p<ws> + dsl::opt(kw_limit >> dsl::p<ws> + dsl::p<limit_value>) + dsl::p<ws> + dsl::p<allow_filtering_clause>;
            }();
            static constexpr auto value = lexy::callback<Select>(
                [](Optional<Select::Transform>&& transform, Select::SelectClause&& sel, TableName&& from,
                   auto&&... rest) -> Select {
                    Select s;
                    s.transform = move(transform);
                    s.select    = move(sel);
                    s.from      = move(from);

                    auto process = [&](auto&& arg) {
                        using T = Decay<decltype(arg)>;
                        if constexpr (SameAs<T, WhereClause>) {
                            s.where = move(arg);
                        } else if constexpr (SameAs<T, Select::GroupByClause>) {
                            s.group_by = move(arg);
                        } else if constexpr (SameAs<T, Select::OrderByClause>) {
                            s.order_by = move(arg);
                        } else if constexpr (SameAs<T, Select::PerPartitionLimit>) {
                            s.per_partition_limit = move(arg);
                        } else if constexpr (SameAs<T, Select::Limit>) {
                            s.limit = move(arg);
                        } else if constexpr (SameAs<T, bool>) {
                            s.allow_filtering = arg;
                        }
                    };
                    (process(rest), ...);
                    return s;
                }
            );
        };

        // ====================================================================
        // top-level
        // ====================================================================
        struct statement {
            static constexpr auto rule = [] {
                auto stmt = dsl::p<use_stmt> | dsl::p<create_keyspace_stmt> | dsl::p<alter_keyspace_stmt> | dsl::p<drop_keyspace_stmt> | dsl::p<create_table_stmt> | dsl::p<create_index_stmt> | dsl::p<drop_index_stmt> | dsl::p<create_type_stmt> | dsl::p<alter_type_stmt> | dsl::p<drop_type_stmt> | dsl::p<alter_table_stmt> | dsl::p<drop_table_stmt> | dsl::p<truncate_stmt> | dsl::p<batch_stmt> | dsl::p<insert_stmt> | dsl::p<update_stmt> | dsl::p<delete_stmt> | dsl::p<select_stmt>;
                return dsl::p<ws> + stmt + dsl::p<ws> + (dsl::lit_c<';'> | dsl::eof);
            }();

            static constexpr auto value = lexy::callback<Statement>(
                [](CreateKeyspace&& s) -> Statement { return {.value = move(s)}; },
                [](UseKeyspace&& s) -> Statement { return {.value = move(s)}; },
                [](AlterKeyspace&& s) -> Statement { return {.value = move(s)}; },
                [](DropKeyspace&& s) -> Statement { return {.value = move(s)}; },
                [](CreateTable&& s) -> Statement { return {.value = move(s)}; },
                [](AlterTable&& s) -> Statement { return {.value = move(s)}; },
                [](DropTable&& s) -> Statement { return {.value = move(s)}; },
                [](TruncateTable&& s) -> Statement { return {.value = move(s)}; },
                [](CreateIndex&& s) -> Statement { return {.value = move(s)}; },
                [](DropIndex&& s) -> Statement { return {.value = move(s)}; },
                [](CreateType&& s) -> Statement { return {.value = move(s)}; },
                [](AlterType&& s) -> Statement { return {.value = move(s)}; },
                [](DropType&& s) -> Statement { return {.value = move(s)}; },
                [](Select&& s) -> Statement { return {.value = move(s)}; },
                [](Insert&& s) -> Statement { return {.value = move(s)}; },
                [](Update&& s) -> Statement { return {.value = move(s)}; },
                [](Delete&& s) -> Statement { return {.value = move(s)}; },
                [](Batch&& s) -> Statement { return {.value = move(s)}; }
            );
        };

        struct query_complete_scanner {
            static constexpr auto rule  = dsl::until(dsl::lit_c<';'>);
            static constexpr auto value = lexy::noop;
        };
    }

    Optional<String8> check_specific_errors(String8 query) {
        const char* p   = query.data;
        const char* end = query.data + query.length;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }

        auto ci_eq3 = [](const char* s, char a, char b, char c) {
            auto lc = [](char x) {
                return (x >= 'A' && x <= 'Z') ? char(x | 0x20) : x;
            };
            return lc(s[0]) == a && lc(s[1]) == b && lc(s[2]) == c;
        };

        if (p + 3 <= end && ci_eq3(p, 'u', 's', 'e') && (p + 3 == end || p[3] == ' ' || p[3] == '\t' || p[3] == '\n' || p[3] == '\r')) {
            p += 3;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
                ++p;
            }
            if (p < end && (*p == '?' || *p == ':')) {
                return String8{"Bind variables cannot be used for keyspace names"};
            }
        }
        return {};
    }

    Optional<Statement> parse(String8 bytes, void (*error_fn)(const String8& error)) {
        ZoneScopedN("parsers::parse");
        log::db_query_text(bytes);

        auto input = lexy::string_input<lexy::ascii_encoding>(bytes.data, bytes.length);

        auto try_parse = [&](auto callback) -> Optional<Statement> {
            auto result = lexy::parse<grammar::statement>(input, callback);
            if (result.has_value()) {
                return result.value();
            }
            return {};
        };

        return try_parse(support::lexy::ErrorCallback<void (*)(const String8& error)>{error_fn});
    }
}
