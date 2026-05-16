module;
#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/error.hpp>
#include <lexy/input/string_input.hpp>

#include <plexdb/support/lexy/macros.h>

module keyvalue.protocol;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.os;
import plexdb.tagged_union;
import keyvalue.log;

using namespace plexdb;
using namespace keyvalue::protocol;

namespace keyvalue::parsers {
    namespace grammar {
        namespace dsl = lexy::dsl;

        // ----------------------------------------------------------------
        // shared sink helpers
        // ----------------------------------------------------------------
        template<typename T>
        struct _dyn_arr_sink {
            using return_type = DynamicArray<T>;
            DynamicArray<T> arr;
            void operator()(T&& v) { push_back(arr, move(v)); }
            return_type finish() && { return move(arr); }
        };
        template<typename T>
        struct as_dyn_arr_t { constexpr _dyn_arr_sink<T> sink() const { return {}; } };
        template<typename T>
        constexpr as_dyn_arr_t<T> as_dyn_arr{};

        // ----------------------------------------------------------------
        // terminals
        // ----------------------------------------------------------------
        constexpr auto space = dsl::while_one(dsl::ascii::blank);
        constexpr auto crlf  = LEXY_LIT("\r\n");

        struct arg {
            static constexpr auto rule = dsl::capture(dsl::token(
                dsl::while_one(dsl::ascii::print - dsl::ascii::blank
                               - dsl::lit_c<'\r'> - dsl::lit_c<'\n'>)
            ));
            static constexpr auto value = lexy::callback<AutoString8>(
                [](auto lex) -> AutoString8 {
                    return AutoString8{reinterpret_cast<const U8*>(lex.begin()),
                                      static_cast<U64>(lex.size())};
                }
            );
        };

        // List of one-or-more space-separated args → DynamicArray<AutoString8>
        struct arg_list {
            static constexpr auto rule  = dsl::list(space >> dsl::p<arg>);
            static constexpr auto value = as_dyn_arr<AutoString8>;
        };

        // ----------------------------------------------------------------
        // PING [message]
        // ----------------------------------------------------------------
        struct ping {
            static constexpr auto rule = LEXY_LIT_CI("ping") >> dsl::opt(space >> dsl::p<arg>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](lexy::nullopt)     -> Statement { return Ping{}; },
                [](AutoString8&& msg) -> Statement { return Ping{move(msg)}; }
            );
        };

        // ----------------------------------------------------------------
        // QUIT
        // ----------------------------------------------------------------
        struct quit {
            static constexpr auto rule = LEXY_LIT_CI("quit") >> crlf;
            static constexpr auto value = lexy::callback<Statement>([]() -> Statement { return Quit{}; });
        };

        // ----------------------------------------------------------------
        // GET key
        // ----------------------------------------------------------------
        struct get {
            static constexpr auto rule = LEXY_LIT_CI("get") >> space + dsl::p<arg> + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](AutoString8&& k) -> Statement { return Get{move(k)}; }
            );
        };

        // ----------------------------------------------------------------
        // SET key value [NX|XX|EX n|PX n|EXAT n|PXAT n]*
        // ----------------------------------------------------------------
        struct set_flag {
            struct nx_ : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("nx");
                static constexpr auto value = lexy::constant(0);
            };
            struct xx_ : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("xx");
                static constexpr auto value = lexy::constant(1);
            };
            struct ttl_ : lexy::transparent_production {
                static constexpr auto rule  =
                    (LEXY_LIT_CI("exat") | LEXY_LIT_CI("pxat") | LEXY_LIT_CI("ex") | LEXY_LIT_CI("px")) >> space + dsl::p<arg>;
                static constexpr auto value = lexy::callback<int>([](auto&&) -> int { return 2; });
            };
            static constexpr auto rule  = dsl::p<nx_> | dsl::p<xx_> | dsl::p<ttl_>;
            static constexpr auto value = lexy::forward<int>;
        };

        struct set_flag_list {
            struct _sink {
                using return_type = Pair<bool, bool>;
                bool nx = false, xx = false;
                void operator()(int f) { if (f == 0) nx = true; else if (f == 1) xx = true; }
                return_type finish() && { return {nx, xx}; }
            };
            struct as_flags_t { constexpr _sink sink() const { return {}; } };
            static constexpr auto rule  = dsl::list(space >> dsl::p<set_flag>);
            static constexpr auto value = as_flags_t{};
        };

        struct set {
            static constexpr auto rule = LEXY_LIT_CI("set") >>
                space + dsl::p<arg> + space + dsl::p<arg>
                + dsl::opt(dsl::p<set_flag_list>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](AutoString8&& k, AutoString8&& v, lexy::nullopt) -> Statement {
                    return Set{move(k), move(v), false, false};
                },
                [](AutoString8&& k, AutoString8&& v, Pair<bool,bool>&& f) -> Statement {
                    return Set{move(k), move(v), f.first, f.second};
                }
            );
        };

        // ----------------------------------------------------------------
        // DEL / EXISTS / MGET key [key ...]
        // ----------------------------------------------------------------
        struct del {
            static constexpr auto rule = LEXY_LIT_CI("del") >> dsl::p<arg_list> + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](DynamicArray<AutoString8>&& keys) -> Statement { return Del{move(keys)}; }
            );
        };
        struct exists {
            static constexpr auto rule = LEXY_LIT_CI("exists") >> dsl::p<arg_list> + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](DynamicArray<AutoString8>&& keys) -> Statement { return Exists{move(keys)}; }
            );
        };
        struct mget {
            static constexpr auto rule = LEXY_LIT_CI("mget") >> dsl::p<arg_list> + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](DynamicArray<AutoString8>&& keys) -> Statement { return Mget{move(keys)}; }
            );
        };

        // ----------------------------------------------------------------
        // MSET key value [key value ...]
        // ----------------------------------------------------------------
        struct kv_pair {
            static constexpr auto rule = dsl::p<arg> + space + dsl::p<arg>;
            static constexpr auto value = lexy::callback<Pair<AutoString8, AutoString8>>(
                [](AutoString8&& k, AutoString8&& v) -> Pair<AutoString8, AutoString8> {
                    return {move(k), move(v)};
                }
            );
        };
        struct kv_pair_list {
            static constexpr auto rule  = dsl::list(space >> dsl::p<kv_pair>);
            static constexpr auto value = as_dyn_arr<Pair<AutoString8, AutoString8>>;
        };
        struct mset {
            static constexpr auto rule = LEXY_LIT_CI("mset") >> dsl::p<kv_pair_list> + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](DynamicArray<Pair<AutoString8,AutoString8>>&& pairs) -> Statement {
                    return Mset{move(pairs)};
                }
            );
        };

        // ----------------------------------------------------------------
        // KEYS [pattern]
        // ----------------------------------------------------------------
        struct keys {
            static constexpr auto rule = LEXY_LIT_CI("keys") >> dsl::opt(space >> dsl::p<arg>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](lexy::nullopt)     -> Statement { return Keys{AutoString8{"*"}}; },
                [](AutoString8&& pat) -> Statement { return Keys{move(pat)}; }
            );
        };

        // ----------------------------------------------------------------
        // SCAN cursor [MATCH pattern] [COUNT n]
        // ----------------------------------------------------------------
        struct ScanOptVal {
            Optional<AutoString8> match;
            Optional<U64>        count;
        };

        struct scan_match : lexy::transparent_production {
            static constexpr auto rule  = LEXY_LIT_CI("match") >> space + dsl::p<arg>;
            static constexpr auto value = lexy::callback<ScanOptVal>(
                [](AutoString8&& m) -> ScanOptVal { return {move(m), {}}; }
            );
        };
        struct scan_count : lexy::transparent_production {
            static constexpr auto rule = LEXY_LIT_CI("count") >> space + dsl::capture(dsl::digits<>);
            static constexpr auto value = lexy::callback<ScanOptVal>(
                [](auto lex) -> ScanOptVal {
                    return {{}, u64_from_str({reinterpret_cast<const char*>(lex.begin()),
                                             static_cast<U64>(lex.size())})};
                }
            );
        };
        struct scan_opt : lexy::transparent_production {
            static constexpr auto rule  = dsl::p<scan_match> | dsl::p<scan_count>;
            static constexpr auto value = lexy::forward<ScanOptVal>;
        };
        struct scan_opt_list {
            struct _sink {
                using return_type = Pair<Optional<AutoString8>, Optional<U64>>;
                Optional<AutoString8> match;
                Optional<U64>        count;
                void operator()(ScanOptVal&& v) {
                    if (v.match) match = move(v.match);
                    if (v.count) count = move(v.count);
                }
                return_type finish() && { return {move(match), move(count)}; }
            };
            struct as_opts_t { constexpr _sink sink() const { return {}; } };
            static constexpr auto rule  = dsl::list(space >> dsl::p<scan_opt>);
            static constexpr auto value = as_opts_t{};
        };

        struct scan {
            static constexpr auto rule = LEXY_LIT_CI("scan") >>
                space + dsl::capture(dsl::digits<>)
                + dsl::opt(dsl::p<scan_opt_list>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](auto cursor_lex, lexy::nullopt) -> Statement {
                    return Scan{u64_from_str({reinterpret_cast<const char*>(cursor_lex.begin()),
                                             static_cast<U64>(cursor_lex.size())}), {}, {}};
                },
                [](auto cursor_lex, Pair<Optional<AutoString8>, Optional<U64>>&& opts) -> Statement {
                    return Scan{u64_from_str({reinterpret_cast<const char*>(cursor_lex.begin()),
                                             static_cast<U64>(cursor_lex.size())}),
                                move(opts.first), move(opts.second)};
                }
            );
        };

        // ----------------------------------------------------------------
        // FLUSHDB / FLUSHALL [ASYNC|SYNC]  DBSIZE  TYPE key
        // ----------------------------------------------------------------
        struct flushdb {
            static constexpr auto rule  = LEXY_LIT_CI("flushdb")  >> dsl::opt(space >> dsl::p<arg>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](lexy::nullopt)  -> Statement { return FlushDb{}; },
                [](AutoString8&&)  -> Statement { return FlushDb{}; }
            );
        };
        struct flushall {
            static constexpr auto rule  = LEXY_LIT_CI("flushall") >> dsl::opt(space >> dsl::p<arg>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](lexy::nullopt)  -> Statement { return FlushAll{}; },
                [](AutoString8&&)  -> Statement { return FlushAll{}; }
            );
        };
        struct dbsize {
            static constexpr auto rule  = LEXY_LIT_CI("dbsize") >> crlf;
            static constexpr auto value = lexy::callback<Statement>([]() -> Statement { return DbSize{}; });
        };
        struct type {
            static constexpr auto rule = LEXY_LIT_CI("type") >> space + dsl::p<arg> + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](AutoString8&& k) -> Statement { return TypeOf{move(k)}; }
            );
        };

        // ----------------------------------------------------------------
        // Statement [...]   SELECT index   CLIENT [...]   INFO [section]
        // ----------------------------------------------------------------
        struct Statement {
            static constexpr auto rule  = LEXY_LIT_CI("Statement") >> dsl::opt(dsl::p<arg_list>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](lexy::nullopt)                    -> Statement { return Cmd{}; },
                [](DynamicArray<AutoString8>&&)      -> Statement { return Cmd{}; }
            );
        };
        struct select {
            static constexpr auto rule = LEXY_LIT_CI("select") >> space + dsl::capture(dsl::digits<>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](auto lex) -> Statement {
                    return SelectDb{u64_from_str({reinterpret_cast<const char*>(lex.begin()),
                                                 static_cast<U64>(lex.size())})};
                }
            );
        };
        struct client {
            static constexpr auto rule = LEXY_LIT_CI("client") >> dsl::opt(dsl::p<arg_list>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](lexy::nullopt)                    -> Statement { return Client{}; },
                [](DynamicArray<AutoString8>&& args) -> Statement { return Client{move(args)}; }
            );
        };
        struct info {
            static constexpr auto rule = LEXY_LIT_CI("info") >> dsl::opt(space >> dsl::p<arg>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](lexy::nullopt)     -> Statement { return Info{}; },
                [](AutoString8&& sec) -> Statement { return Info{move(sec)}; }
            );
        };

        // ----------------------------------------------------------------
        // Unknown fallback
        // ----------------------------------------------------------------
        struct unknown {
            static constexpr auto rule = dsl::p<arg> >> dsl::opt(dsl::p<arg_list>) + crlf;
            static constexpr auto value = lexy::callback<Statement>(
                [](AutoString8&& name, lexy::nullopt) -> Statement {
                    return Unknown{move(name), {}};
                },
                [](AutoString8&& name, DynamicArray<AutoString8>&& rest) -> Statement {
                    return Unknown{move(name), move(rest)};
                }
            );
        };

        // ----------------------------------------------------------------
        // top-level: ordered alternatives, falls through to unknown
        // ----------------------------------------------------------------
        struct Statement {
            static constexpr auto rule =
                dsl::p<ping>    | dsl::p<quit>    | dsl::p<get>     |
                dsl::p<set>     | dsl::p<del>     | dsl::p<exists>  |
                dsl::p<mget>    | dsl::p<mset>    | dsl::p<keys>    |
                dsl::p<scan>    | dsl::p<flushdb> | dsl::p<flushall>|
                dsl::p<dbsize>  | dsl::p<type>    | dsl::p<Statement> |
                dsl::p<select>  | dsl::p<client>  | dsl::p<info>    |
                dsl::else_ >> dsl::p<unknown>;
            static constexpr auto value = lexy::forward<Statement>;
        };

        struct noop_error {
            struct _sink {
                using return_type = size_t;
                size_t n = 0;
                template<typename Input, typename Reader, typename Tag>
                void operator()(const lexy::error_context<Input>&,
                                const lexy::error<Reader, Tag>&) {}
                size_t finish() && { return n; }
            };
            _sink sink() const { return {}; }
        };
    }

    // ========================================================================
    // RESP array framing: *N\r\n ($L\r\n<data>\r\n)*
    // ========================================================================
    static const U8* find_crlf(const U8* p, const U8* end) {
        while (p + 1 < end) {
            if (p[0] == '\r' && p[1] == '\n') return p;
            p++;
        }
        return nullptr;
    }

    static bool parse_decimal(const U8* p, const U8* end, S64* out) {
        if (p >= end) return false;
        bool neg = (*p == '-');
        if (neg) p++;
        if (p >= end) return false;
        S64 r = 0;
        while (p < end) {
            if (*p < '0' || *p > '9') return false;
            r = r * 10 + (*p++ - '0');
        }
        *out = neg ? -r : r;
        return true;
    }

    static bool arg_eq(const AutoString8& a, const char* b) {
        if (a.length != String8{b}.length) return false;
        for (U64 i = 0; i < a.length; i++) {
            char ca = a.c_str[i], cb = b[i];
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) return false;
        }
        return true;
    }

    // Build typed Statement from pre-framed bulk-string args (RESP array path).
    static Statement build_from_args(DynamicArray<AutoString8>&& args) {
        if (args.length == 0) return Unknown{};

        resp::log::db_query_text(args[0]);

        if (arg_eq(args[0], "PING"))    return args.length >= 2 ? Statement{Ping{move(args[1])}} : Statement{Ping{}};
        if (arg_eq(args[0], "QUIT"))    return Quit{};
        if (arg_eq(args[0], "GET"))     return args.length >= 2 ? Statement{Get{move(args[1])}}    : Statement{Unknown{move(args[0]), {}}};
        if (arg_eq(args[0], "TYPE"))    return args.length >= 2 ? Statement{TypeOf{move(args[1])}} : Statement{Unknown{move(args[0]), {}}};
        if (arg_eq(args[0], "DBSIZE"))  return DbSize{};
        if (arg_eq(args[0], "Statement")) return Cmd{};
        if (arg_eq(args[0], "FLUSHDB")) return FlushDb{};
        if (arg_eq(args[0], "FLUSHALL"))return FlushAll{};

        if (arg_eq(args[0], "SELECT")) {
            return SelectDb{args.length >= 2 ? u64_from_str(args[1]) : 0};
        }
        if (arg_eq(args[0], "KEYS")) {
            return Keys{args.length >= 2 ? move(args[1]) : AutoString8{"*"}};
        }
        if (arg_eq(args[0], "INFO")) {
            return Info{args.length >= 2 ? Optional<AutoString8>{move(args[1])} : Optional<AutoString8>{}};
        }
        if (arg_eq(args[0], "SET")) {
            if (args.length < 3) return Unknown{move(args[0]), {}};
            bool nx = false, xx = false;
            for (U64 i = 3; i < args.length; i++) {
                if      (arg_eq(args[i], "NX"))                                                    nx = true;
                else if (arg_eq(args[i], "XX"))                                                    xx = true;
                else if (arg_eq(args[i], "EX")||arg_eq(args[i],"PX")||
                         arg_eq(args[i], "EXAT")||arg_eq(args[i],"PXAT"))                         i++;
            }
            return Set{move(args[1]), move(args[2]), nx, xx};
        }
        if (arg_eq(args[0], "DEL") || arg_eq(args[0], "EXISTS") || arg_eq(args[0], "MGET")) {
            DynamicArray<AutoString8> keys;
            for (U64 i = 1; i < args.length; i++) push_back(keys, move(args[i]));
            if (arg_eq(args[0], "DEL"))    return Del{move(keys)};
            if (arg_eq(args[0], "EXISTS")) return Exists{move(keys)};
            return Mget{move(keys)};
        }
        if (arg_eq(args[0], "MSET")) {
            DynamicArray<Pair<AutoString8, AutoString8>> pairs;
            for (U64 i = 1; i + 1 < args.length; i += 2)
                push_back(pairs, {move(args[i]), move(args[i+1])});
            return Mset{move(pairs)};
        }
        if (arg_eq(args[0], "SCAN")) {
            if (args.length < 2) return Unknown{move(args[0]), {}};
            U64 cursor = u64_from_str(args[1]);
            Optional<AutoString8> match;
            Optional<U64>        count;
            for (U64 i = 2; i + 1 < args.length; i += 2) {
                if      (arg_eq(args[i], "MATCH")) match = move(args[i+1]);
                else if (arg_eq(args[i], "COUNT")) count = u64_from_str(args[i+1]);
            }
            return Scan{cursor, move(match), move(count)};
        }
        if (arg_eq(args[0], "CLIENT")) {
            DynamicArray<AutoString8> cargs;
            for (U64 i = 1; i < args.length; i++) push_back(cargs, move(args[i]));
            return Client{move(cargs)};
        }

        AutoString8 name = move(args[0]);
        DynamicArray<AutoString8> rest;
        for (U64 i = 1; i < args.length; i++) push_back(rest, move(args[i]));
        return Unknown{move(name), move(rest)};
    }

    static Pair<ParseResult, Statement> parse_resp_array(const U8* data, U64 length, U64* consumed) {
        const U8* p   = data + 1;
        const U8* end = data + length;

        const U8* crlf = find_crlf(p, end);
        if (!crlf) return {ParseResult::Incomplete, {}};

        S64 count = 0;
        if (!parse_decimal(p, crlf, &count) || count < 0) return {ParseResult::Error, {}};

        p = crlf + 2;
        DynamicArray<AutoString8> args;

        for (S64 i = 0; i < count; i++) {
            if (p >= end || *p != '$') return {ParseResult::Error, {}};
            p++;
            crlf = find_crlf(p, end);
            if (!crlf) return {ParseResult::Incomplete, {}};
            S64 bulk_len = 0;
            if (!parse_decimal(p, crlf, &bulk_len) || bulk_len < 0) return {ParseResult::Error, {}};
            p = crlf + 2;
            if (p + U64(bulk_len) + 2 > end) return {ParseResult::Incomplete, {}};
            push_back(args, AutoString8{p, U64(bulk_len)});
            p += U64(bulk_len) + 2;
        }

        *consumed = U64(p - data);
        return {ParseResult::Ok, build_from_args(move(args))};
    }

    static Pair<ParseResult, Statement> parse_inline(const U8* data, U64 length, U64* consumed) {
        const U8* end  = data + length;
        const U8* crlf = find_crlf(data, end);
        if (!crlf) return {ParseResult::Incomplete, {}};

        *consumed = U64(crlf - data) + 2;

        resp::log::db_query_text(String8{reinterpret_cast<const char*>(data), U64(crlf - data)});

        auto input = lexy::string_input<lexy::ascii_encoding>(
            reinterpret_cast<const char*>(data), *consumed
        );
        auto result = lexy::parse<grammar::Statement>(input, grammar::noop_error{});
        if (!result.has_value()) return {ParseResult::Error, {}};
        return {ParseResult::Ok, move(result.value())};
    }

    export Optional<Statement> parse(const U8* data, U64 length, U64* consumed) {
        if (length == 0) return {ParseResult::Incomplete, {}};
        *consumed = 0;
        if (data[0] == '*') return parse_resp_array(data, length, consumed);
        return parse_inline(data, length, consumed);
    }
}
