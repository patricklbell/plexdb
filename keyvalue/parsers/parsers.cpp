module keyvalue.parsers;

import plexdb.base;
import plexdb.dynamic.containers;
import keyvalue.engine.statements;
import keyvalue.log;

using namespace plexdb;

namespace keyvalue::parsers {
    static const U8* find_crlf(const U8* p, const U8* end) {
        while (p + 1 < end) {
            if (p[0] == '\r' && p[1] == '\n') {
                return p;
            }
            ++p;
        }
        return nullptr;
    }

    static bool parse_decimal(const U8* p, const U8* end, S64* out) {
        if (p >= end) {
            return false;
        }
        bool neg = (*p == '-');
        if (neg) {
            ++p;
        }
        if (p >= end) {
            return false;
        }
        S64 r = 0;
        while (p < end) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            r = r * 10 + (*p++ - '0');
        }
        *out = neg ? -r : r;
        return true;
    }

    static bool arg_eq(const AutoString8& a, const char* b) {
        String8 bs{b};
        if (a.length != bs.length) {
            return false;
        }
        for (U64 i = 0; i < a.length; i++) {
            char ca = a.c_str[i], cb = b[i];
            if (ca >= 'a' && ca <= 'z') {
                ca -= 32;
            }
            if (cb >= 'a' && cb <= 'z') {
                cb -= 32;
            }
            if (ca != cb) {
                return false;
            }
        }
        return true;
    }

    static Statement build_from_args(DynamicArray<AutoString8>&& args) {
        if (args.length == 0) {
            return Statement{Unknown{}};
        }

        log::db_query_text(args[0]);

        if (arg_eq(args[0], "PING")) {
            return args.length >= 2 ? Statement{Ping{move(args[1])}} : Statement{Ping{}};
        }
        if (arg_eq(args[0], "QUIT")) {
            return Statement{Quit{}};
        }
        if (arg_eq(args[0], "GET")) {
            return args.length >= 2 ? Statement{
                                          Get{move(args[1])}
            }
                                    : Statement{Unknown{move(args[0]), {}}};
        }
        if (arg_eq(args[0], "TYPE")) {
            return args.length >= 2 ? Statement{
                                          TypeOf{move(args[1])}
            }
                                    : Statement{Unknown{move(args[0]), {}}};
        }
        if (arg_eq(args[0], "DBSIZE")) {
            return Statement{DbSize{}};
        }
        if (arg_eq(args[0], "COMMAND")) {
            return Statement{Cmd{}};
        }
        if (arg_eq(args[0], "FLUSHDB")) {
            return Statement{FlushDb{}};
        }
        if (arg_eq(args[0], "FLUSHALL")) {
            return Statement{FlushAll{}};
        }

        if (arg_eq(args[0], "SELECT")) {
            return Statement{SelectDb{args.length >= 2 ? u64_from_str(args[1]) : 0}};
        }
        if (arg_eq(args[0], "KEYS")) {
            return Statement{Keys{args.length >= 2 ? move(args[1]) : AutoString8{"*"}}};
        }
        if (arg_eq(args[0], "INFO")) {
            return Statement{Info{args.length >= 2 ? Optional<AutoString8>{move(args[1])} : Optional<AutoString8>{}}};
        }
        if (arg_eq(args[0], "SET")) {
            if (args.length < 3) {
                return Statement{
                    Unknown{move(args[0]), {}}
                };
            }
            bool nx = false, xx = false;
            for (U64 i = 3; i < args.length; i++) {
                if (arg_eq(args[i], "NX")) {
                    nx = true;
                } else if (arg_eq(args[i], "XX")) {
                    xx = true;
                } else if (arg_eq(args[i], "EX") || arg_eq(args[i], "PX") || arg_eq(args[i], "EXAT") || arg_eq(args[i], "PXAT")) {
                    assert_not_implemented("key expiry");
                    i++;
                }
            }
            return Statement{
                Set{move(args[1]), move(args[2]), nx, xx}
            };
        }
        if (arg_eq(args[0], "DEL") || arg_eq(args[0], "EXISTS") || arg_eq(args[0], "MGET")) {
            DynamicArray<AutoString8> keys;
            for (U64 i = 1; i < args.length; i++) {
                push_back(keys, move(args[i]));
            }
            if (arg_eq(args[0], "DEL")) {
                return Statement{Del{move(keys)}};
            }
            if (arg_eq(args[0], "EXISTS")) {
                return Statement{Exists{move(keys)}};
            }
            return Statement{Mget{move(keys)}};
        }
        if (arg_eq(args[0], "MSET")) {
            DynamicArray<Pair<AutoString8, AutoString8>> pairs;
            for (U64 i = 1; i + 1 < args.length; i += 2) {
                push_back(pairs, {move(args[i]), move(args[i + 1])});
            }
            return Statement{Mset{move(pairs)}};
        }
        if (arg_eq(args[0], "SCAN")) {
            if (args.length < 2) {
                return Statement{
                    Unknown{move(args[0]), {}}
                };
            }
            U64                   cursor = u64_from_str(args[1]);
            Optional<AutoString8> match;
            Optional<U64>         count;
            for (U64 i = 2; i + 1 < args.length; i += 2) {
                if (arg_eq(args[i], "MATCH")) {
                    match = move(args[i + 1]);
                } else if (arg_eq(args[i], "COUNT")) {
                    count = u64_from_str(args[i + 1]);
                }
            }
            return Statement{
                Scan{cursor, move(match), move(count)}
            };
        }
        if (arg_eq(args[0], "CLIENT")) {
            if (args.length >= 2 && arg_eq(args[1], "GETNAME")) {
                return Statement{ClientGetName{}};
            }
            DynamicArray<AutoString8> cargs;
            for (U64 i = 1; i < args.length; i++) {
                push_back(cargs, move(args[i]));
            }
            return Statement{Client{move(cargs)}};
        }

        AutoString8               name = move(args[0]);
        DynamicArray<AutoString8> rest;
        for (U64 i = 1; i < args.length; i++) {
            push_back(rest, move(args[i]));
        }
        return Statement{
            Unknown{move(name), move(rest)}
        };
    }

    // ================================================================
    // RESP array:  *N\r\n  ($L\r\n <data> \r\n) × N
    // ================================================================
    static Pair<ParseResult, Statement> parse_resp_array(const U8* data, U64 length, U64* consumed) {
        const U8* p   = data + 1;
        const U8* end = data + length;

        const U8* eol = find_crlf(p, end);
        if (!eol) {
            return {ParseResult::Incomplete, {}};
        }

        S64 count = 0;
        if (!parse_decimal(p, eol, &count) || count < 0) {
            return {ParseResult::Error, {}};
        }

        p = eol + 2;
        DynamicArray<AutoString8> args;

        for (S64 i = 0; i < count; i++) {
            if (p >= end || *p != '$') {
                return {ParseResult::Error, {}};
            }
            ++p;
            eol = find_crlf(p, end);
            if (!eol) {
                return {ParseResult::Incomplete, {}};
            }
            S64 bulk_len = 0;
            if (!parse_decimal(p, eol, &bulk_len) || bulk_len < 0) {
                return {ParseResult::Error, {}};
            }
            p = eol + 2;
            if (p + U64(bulk_len) + 2 > end) {
                return {ParseResult::Incomplete, {}};
            }
            push_back(args, AutoString8{
                                String8{p, U64(bulk_len)}
            });
            p += U64(bulk_len) + 2;
        }

        *consumed = U64(p - data);
        return {ParseResult::Ok, build_from_args(move(args))};
    }

    // ================================================================
    // Inline:  COMMAND [arg ...]\r\n  (space-separated tokens)
    // ================================================================
    static Pair<ParseResult, Statement> parse_inline(const U8* data, U64 length, U64* consumed) {
        const U8* end = data + length;
        const U8* eol = find_crlf(data, end);
        if (!eol) {
            return {ParseResult::Incomplete, {}};
        }

        *consumed = U64(eol - data) + 2;
        log::db_query_text({data, U64(eol - data)});

        DynamicArray<AutoString8> args;
        const U8*                 p = data;
        while (p < eol) {
            while (p < eol && (*p == ' ' || *p == '\t')) {
                ++p;
            }
            if (p >= eol) {
                break;
            }
            const U8* tok = p;
            while (p < eol && *p != ' ' && *p != '\t') {
                ++p;
            }
            push_back(args, AutoString8{
                                String8{tok, U64(p - tok)}
            });
        }

        if (args.length == 0) {
            return {ParseResult::Error, {}};
        }
        return {ParseResult::Ok, build_from_args(move(args))};
    }

    Pair<ParseResult, Statement> parse(const U8* data, U64 length, U64* consumed) {
        if (length == 0) {
            return {ParseResult::Incomplete, {}};
        }
        *consumed = 0;
        if (data[0] == '*') {
            return parse_resp_array(data, length, consumed);
        }
        return parse_inline(data, length, consumed);
    }
}
