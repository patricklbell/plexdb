#include <catch2/catch_test_macros.hpp>

#include <plexdb/macros/macros.h>
#include <plexdb/test_macros/test_macros.h>
#include <cql/test_macros/test_macros.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;
import plexdb.test.pager_helpers;
import plexdb.aio;
import plexdb.test.server_helpers;

import cql.crc;
import cql.lz4;
import cql.engine;
import cql.native;
import cql.test_helpers;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;
using namespace cql::column_types;
using namespace cql::test;

namespace {
    constexpr U8  OP_AUTHENTICATE  = 0x03;
    constexpr U8  OP_REGISTER      = 0x0B;
    constexpr U8  OP_EVENT         = 0x0C;
    constexpr U8  OP_AUTH_RESPONSE = 0x0F;
    constexpr U8  OP_AUTH_SUCCESS  = 0x10;
    constexpr S32 ERR_PROTOCOL     = 0x000A;
    constexpr S32 ERR_BAD_CREDS    = 0x0100;
    constexpr S32 ERR_INVALID      = 0x2200;

    S32 error_code(const Frame& f) {
        return result_kind(f);
    }

    U8 g_version = 4;

    void append_flags(DynamicArray<U8>& b, U32 flags) {
        if (g_version >= 5) {
            append_be_s32(b, S32(flags));
        } else {
            append_u8(b, U8(flags));
        }
    }

    // QUERY whose positional values are given as raw [value] bytes.
    DynamicArray<U8> build_query_values(String8 cql, const DynamicArray<DynamicArray<U8>>& values, S16 stream = 0) {
        DynamicArray<U8> b;
        append_cql_long_string(b, cql);
        append_be_u16(b, 0x0001);
        append_flags(b, values.length ? 0x01 : 0x00);
        if (values.length) {
            append_be_u16(b, U16(values.length));
            for (const auto& v : values) {
                append_bytes(b, v.ptr, v.length);
            }
        }
        prepend_frame_header(b, op::QUERY, stream, g_version);
        return b;
    }

    DynamicArray<U8> raw_value(const U8* data, S32 n) {
        DynamicArray<U8> v;
        append_cql_bytes(v, data, n);
        return v;
    }

    DynamicArray<U8> int_value(S32 x) {
        DynamicArray<U8> v;
        append_cql_bind_s32(v, x);
        return v;
    }

    DynamicArray<U8> text_value(String8 s) {
        DynamicArray<U8> v;
        append_cql_bind_str(v, s);
        return v;
    }

    Frame send_auth(Socket& s, String8 user, String8 pass) {
        DynamicArray<U8> token;
        append_u8(token, 0);
        append_bytes(token, reinterpret_cast<const U8*>(user.data), user.length);
        append_u8(token, 0);
        append_bytes(token, reinterpret_cast<const U8*>(pass.data), pass.length);
        DynamicArray<U8> b;
        append_cql_bytes(b, token.ptr, S32(token.length));
        prepend_frame_header(b, OP_AUTH_RESPONSE, 3, g_version);
        send_frame(s, b);
        return recv_frame(s);
    }

    Frame send_register(Socket& s, String8 event_type) {
        DynamicArray<U8> b;
        append_be_u16(b, 1);
        append_cql_string(b, event_type);
        prepend_frame_header(b, OP_REGISTER, 1, g_version);
        send_frame(s, b);
        return recv_frame(s);
    }

    DynamicArray<U8> build_execute(TArrayView<const U8> id, S16 stream = 0) {
        DynamicArray<U8> b;
        append_cql_short_bytes(b, id.ptr, U16(id.length));
        if (g_version >= 5) {
            append_cql_short_bytes(b, id.ptr, U16(id.length)); // result_metadata_id, unchecked
        }
        append_be_u16(b, 0x0001);
        append_flags(b, 0x00);
        prepend_frame_header(b, op::EXECUTE, stream, g_version);
        return b;
    }

    void connect_and_handshake(Socket& s, U8 version) {
        g_version = version;
        handshake(s, version);
    }

    // Sends `envelope` as a sequence of non-self-contained uncompressed v5 frames.
    void send_v5_split(Socket& s, const DynamicArray<U8>& envelope) {
        constexpr U64 MAX_PAYLOAD = 131071;
        U64           offset      = 0;
        while (offset < envelope.length) {
            U64 chunk   = min(envelope.length - offset, MAX_PAYLOAD);
            U32 pl      = U32(chunk);
            U8  bits[3] = {U8(pl), U8(pl >> 8), U8(pl >> 16 & 0x01u)};
            U32 hcrc    = crc::crc24(bits, 3);
            U8  hdr[6]  = {bits[0], bits[1], bits[2], U8(hcrc), U8(hcrc >> 8), U8(hcrc >> 16)};
            U32 pcrc    = crc::crc32(envelope.ptr + offset, chunk, crc::CRC32_CQL_V5_INIT);
            U8  tr[4]   = {U8(pcrc), U8(pcrc >> 8), U8(pcrc >> 16), U8(pcrc >> 24)};
            DynamicArray<U8> out;
            append_bytes(out, hdr, 6);
            append_bytes(out, envelope.ptr + offset, chunk);
            append_bytes(out, tr, 4);
            send_raw(s, out);
            offset += chunk;
        }
    }
}

CQL_NATIVE_TEST_CASE("Authentication: PasswordAuthenticator SASL PLAIN handshake", "[cql.native][auth]") {
    native::ServerConfig config{};
    push_back(config.credentials, native::AuthCredential{.username = AutoString8("alice"), .password = AutoString8("s3cret")});

    for (U8 version : {U8(4), U8(5)}) {
        run_native_server_with_config(fixture, config, [version](U16 port, Notifier& interrupt) {
            g_version  = version;
            Socket s   = client_connect(port);
            Frame  hello = send_startup(s, 0, version);
            CHECK(hello.opcode == OP_AUTHENTICATE);
            CHECK(body_contains(hello, "org.apache.cassandra.auth.PasswordAuthenticator"));
            set_connection_version(version);

            Frame early = send_query(s, "SELECT * FROM system.local;");
            CHECK(early.opcode == op::ERROR);
            CHECK(error_code(early) == ERR_PROTOCOL);

            Frame bad = send_auth(s, "alice", "wrong");
            CHECK(bad.opcode == op::ERROR);
            CHECK(error_code(bad) == ERR_BAD_CREDS);

            Frame unknown = send_auth(s, "mallory", "s3cret");
            CHECK(error_code(unknown) == ERR_BAD_CREDS);

            Frame ok = send_auth(s, "alice", "s3cret");
            CHECK(ok.opcode == OP_AUTH_SUCCESS);
            CHECK(send_query(s, "SELECT * FROM system.local;").opcode == op::RESULT);

            Frame again = send_auth(s, "alice", "s3cret");
            CHECK(error_code(again) == ERR_PROTOCOL);
            signal_notify_safe(interrupt);
        });
    }
    co_return;
}

CQL_NATIVE_TEST_CASE("REGISTER: SCHEMA_CHANGE events are pushed to registered connections", "[cql.native][events]") {
    for (U8 version : {U8(4), U8(5)}) {
        run_native_server_with_config(fixture, {}, [version](U16 port, Notifier& interrupt) {
            AutoString8 ks = fmt("ev%d", (int)version);

            Socket listener = client_connect(port);
            connect_and_handshake(listener, version);
            CHECK(send_register(listener, "SCHEMA_CHANGE").opcode == op::READY);
            CHECK(send_register(listener, "TOPOLOGY_CHANGE").opcode == op::READY);
            Frame bad = send_register(listener, "NOT_AN_EVENT");
            CHECK(bad.opcode == op::ERROR);
            CHECK(error_code(bad) == ERR_PROTOCOL);

            Socket other = client_connect(port);
            connect_and_handshake(other, version);

            CHECK(result_kind(send_query(other, String8(fmt("CREATE KEYSPACE %s;", ks.c_str)))) == result::SCHEMA_CHANGE);
            Frame ev = recv_frame(listener);
            CHECK(ev.opcode == OP_EVENT);
            CHECK(ev.stream == -1);
            CHECK(body_contains(ev, "SCHEMA_CHANGE"));
            CHECK(body_contains(ev, "CREATED"));
            CHECK(body_contains(ev, "KEYSPACE"));
            CHECK(body_contains(ev, String8(ks)));

            CHECK(result_kind(send_query(other, String8(fmt("CREATE TABLE %s.t (k int PRIMARY KEY, v text);", ks.c_str)))) == result::SCHEMA_CHANGE);
            ev = recv_frame(listener);
            CHECK(ev.opcode == OP_EVENT);
            CHECK(body_contains(ev, "CREATED"));
            CHECK(body_contains(ev, "TABLE"));

            CHECK(result_kind(send_query(other, String8(fmt("CREATE TYPE %s.addr (street text);", ks.c_str)))) == result::SCHEMA_CHANGE);
            ev = recv_frame(listener);
            CHECK(body_contains(ev, "TYPE"));
            CHECK(body_contains(ev, "addr"));

            Frame drop = send_query(other, String8(fmt("DROP TABLE %s.t;", ks.c_str)));
            CHECK(body_contains(drop, "DROPPED"));
            ev = recv_frame(listener);
            CHECK(body_contains(ev, "DROPPED"));
            CHECK(body_contains(ev, "TABLE"));

            // The registered connection keeps serving its own requests.
            CHECK(send_query(listener, "SELECT * FROM system.local;").opcode == op::RESULT);
            signal_notify_safe(interrupt);
        });
    }
    co_return;
}

CQL_NATIVE_TEST_CASE("Malformed requests return errors instead of crashing", "[cql.native][protocol]") {
    run_native_server_with_config(fixture, {}, [](U16 port, Notifier& interrupt) {
        Socket s = client_connect(port);
        connect_and_handshake(s, 4);
        CHECK(send_query(s, "CREATE KEYSPACE m;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE m.t (k int PRIMARY KEY, v text, l list<int>);").opcode == op::RESULT);

        {
            // [long string] claims more bytes than the body holds
            DynamicArray<U8> b;
            append_be_s32(b, 1000);
            append_bytes(b, reinterpret_cast<const U8*>("SELECT"), 6);
            prepend_frame_header(b, op::QUERY, 7, 4);
            send_frame(s, b);
            Frame f = recv_frame(s);
            CHECK(f.opcode == op::ERROR);
            CHECK(f.stream == 7);
            CHECK(error_code(f) == ERR_PROTOCOL);
        }
        {
            send_frame(s, build_query_values("SELECT * FROM m.t WHERE k = ?;", {}));
            Frame f = recv_frame(s);
            CHECK(error_code(f) == ERR_INVALID);
            CHECK(body_contains(f, "Invalid amount of bind variables"));
        }
        {
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(1));
            push_back(values, text_value("a"));
            push_back(values, text_value("extra"));
            send_frame(s, build_query_values("INSERT INTO m.t (k, v) VALUES (?, ?);", values));
            CHECK(error_code(recv_frame(s)) == ERR_INVALID);
        }
        {
            U8                             two[2] = {0, 1};
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, raw_value(two, 2));
            push_back(values, text_value("a"));
            send_frame(s, build_query_values("INSERT INTO m.t (k, v) VALUES (?, ?);", values));
            CHECK(error_code(recv_frame(s)) == ERR_INVALID);
        }
        {
            U8                             bad_utf8[2] = {0xC3, 0x28};
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(1));
            push_back(values, raw_value(bad_utf8, 2));
            send_frame(s, build_query_values("INSERT INTO m.t (k, v) VALUES (?, ?);", values));
            CHECK(error_code(recv_frame(s)) == ERR_INVALID);
        }
        {
            // list<int> holding a null element
            DynamicArray<U8> body;
            append_be_s32(body, 1);
            append_be_s32(body, -1);
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(1));
            push_back(values, raw_value(body.ptr, S32(body.length)));
            send_frame(s, build_query_values("INSERT INTO m.t (k, l) VALUES (?, ?);", values));
            CHECK(error_code(recv_frame(s)) == ERR_INVALID);
        }
        {
            // list<int> whose element count overruns its bytes
            DynamicArray<U8> body;
            append_be_s32(body, 3);
            append_cql_bind_s32(body, 1);
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(1));
            push_back(values, raw_value(body.ptr, S32(body.length)));
            send_frame(s, build_query_values("INSERT INTO m.t (k, l) VALUES (?, ?);", values));
            CHECK(error_code(recv_frame(s)) == ERR_INVALID);
        }
        {
            DynamicArray<U8> b;
            prepend_frame_header(b, 0x42, 9, 4);
            send_frame(s, b);
            Frame f = recv_frame(s);
            CHECK(error_code(f) == ERR_PROTOCOL);
        }
        {
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(1));
            push_back(values, text_value("ok"));
            send_frame(s, build_query_values("INSERT INTO m.t (k, v) VALUES (?, ?);", values));
            CHECK(recv_frame(s).opcode == op::RESULT);
            Frame sel = send_query(s, "SELECT v FROM m.t WHERE k = 1;");
            CHECK(body_contains(sel, "ok"));
        }
        {
            // A negative body length is unrecoverable: the server answers, then closes.
            Socket           bad = client_connect(port);
            DynamicArray<U8> b;
            prepend_frame_header(b, op::OPTIONS, 2, 4);
            b[5] = 0xFF;
            b[6] = 0xFF;
            b[7] = 0xFF;
            b[8] = 0xF0;
            send_raw(bad, b);
            Frame f = recv_raw(bad);
            CHECK(error_code(f) == ERR_PROTOCOL);
        }
        Socket fresh = client_connect(port);
        connect_and_handshake(fresh, 4);
        CHECK(send_query(fresh, "SELECT v FROM m.t WHERE k = 1;").opcode == op::RESULT);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Protocol version negotiation", "[cql.native][protocol]") {
    run_native_server_with_config(fixture, {}, [](U16 port, Notifier& interrupt) {
        {
            Socket s = client_connect(port);
            Frame  f = send_startup(s, 0, 6);
            CHECK(f.opcode == op::ERROR);
            CHECK(error_code(f) == ERR_PROTOCOL);
            CHECK(body_contains(f, "Invalid or unsupported protocol version"));
        }
        {
            Socket s = client_connect(port);
            DynamicArray<U8> b;
            prepend_frame_header(b, op::OPTIONS, 0, 5);
            send_raw(s, b);
            Frame f = recv_raw(s);
            CHECK(f.opcode == op::SUPPORTED);
            CHECK(f.version == 0x85);
            CHECK(body_contains(f, "PROTOCOL_VERSIONS"));
            CHECK(body_contains(f, "3/v3"));
        }
        {
            Socket s = client_connect(port);
            connect_and_handshake(s, 3);
            CHECK(send_query(s, "CREATE KEYSPACE v3;").opcode == op::RESULT);
            CHECK(send_query(s, "CREATE TABLE v3.t (k int PRIMARY KEY, d date, n smallint);").opcode == op::RESULT);
            CHECK(send_query(s, "INSERT INTO v3.t (k, n) VALUES (1, 3);").opcode == op::RESULT);
            Frame sel = send_query(s, "SELECT * FROM v3.t;");
            CHECK(sel.version == 0x83);
            CHECK(result_kind(sel) == result::ROWS);
            CHECK(body_contains(sel, "org.apache.cassandra.db.marshal.SimpleDateType"));
            CHECK(body_contains(sel, "org.apache.cassandra.db.marshal.ShortType"));
        }
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("v4 LZ4 envelope compression", "[cql.native][protocol]") {
    run_native_server_with_config(fixture, {}, [](U16 port, Notifier& interrupt) {
        Socket           s = client_connect(port);
        DynamicArray<U8> startup;
        append_be_u16(startup, 2);
        append_cql_string(startup, "CQL_VERSION");
        append_cql_string(startup, "3.0.0");
        append_cql_string(startup, "COMPRESSION");
        append_cql_string(startup, "lz4");
        prepend_frame_header(startup, op::STARTUP, 0, 4);
        send_raw(s, startup);
        CHECK(recv_raw(s).opcode == op::READY);
        g_version = 4;
        set_connection_version(4);

        DynamicArray<U8> body;
        append_cql_long_string(body, "SELECT * FROM system.local;");
        append_be_u16(body, 0x0001);
        append_u8(body, 0x00);

        DynamicArray<U8> compressed;
        S32              bound = lz4::compress_bound(S32(body.length));
        resize(compressed, 4 + U64(bound));
        compressed[0] = U8(body.length >> 24);
        compressed[1] = U8(body.length >> 16);
        compressed[2] = U8(body.length >> 8);
        compressed[3] = U8(body.length);
        S32 n         = lz4::compress(body.ptr, compressed.ptr + 4, S32(body.length), bound);
        REQUIRE(n > 0);
        resize(compressed, 4 + U64(n));
        prepend_frame_header(compressed, op::QUERY, 5, 4);
        compressed[1] = 0x01; // compression flag
        send_raw(s, compressed);
        Frame f = recv_raw(s);
        CHECK(f.opcode == op::RESULT);
        CHECK(f.stream == 5);
        CHECK(result_kind(f) == result::ROWS);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Prepared statements are scoped to their prepare-time keyspace", "[cql.native][prepare]") {
    run_native_server_with_config(fixture, {}, [](U16 port, Notifier& interrupt) {
        Socket s = client_connect(port);
        connect_and_handshake(s, 4);
        CHECK(send_query(s, "CREATE KEYSPACE ks1;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE KEYSPACE ks2;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE ks1.t (k int PRIMARY KEY, v text);").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE ks2.t (k int PRIMARY KEY, v text);").opcode == op::RESULT);
        CHECK(send_query(s, "INSERT INTO ks1.t (k, v) VALUES (1, 'from-ks1');").opcode == op::RESULT);
        CHECK(send_query(s, "INSERT INTO ks2.t (k, v) VALUES (1, 'from-ks2');").opcode == op::RESULT);

        CHECK(send_query(s, "USE ks1;").opcode == op::RESULT);
        Frame p1 = send_prepare(s, "SELECT v FROM t WHERE k = 1;");
        REQUIRE(result_kind(p1) == result::PREPARED);
        CHECK(send_query(s, "USE ks2;").opcode == op::RESULT);
        Frame p2 = send_prepare(s, "SELECT v FROM t WHERE k = 1;");
        REQUIRE(result_kind(p2) == result::PREPARED);

        auto id1 = read_prepared_id(p1);
        auto id2 = read_prepared_id(p2);
        REQUIRE(id1.length == id2.length);
        CHECK(memory_compare(id1.ptr, id2.ptr, id1.length) != 0);

        // Still USE ks2, but id1 was prepared in ks1.
        send_frame(s, build_execute(id1));
        Frame e1 = recv_frame(s);
        CHECK(body_contains(e1, "from-ks1"));
        CHECK(!body_contains(e1, "from-ks2"));

        // A schema change invalidates prepared statements: the client must re-prepare.
        CHECK(send_query(s, "CREATE TABLE ks1.other (k int PRIMARY KEY);").opcode == op::RESULT);
        send_frame(s, build_execute(id1));
        Frame stale = recv_frame(s);
        CHECK(error_code(stale) == 0x2500);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("v5 per-query keyspace and large envelopes", "[cql.native][protocol]") {
    run_native_server_with_config(fixture, {}, [](U16 port, Notifier& interrupt) {
        Socket s = client_connect(port);
        connect_and_handshake(s, 5);
        CHECK(send_query(s, "CREATE KEYSPACE big;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE big.t (k int PRIMARY KEY, v text);").opcode == op::RESULT);

        {
            // QUERY with the v5 keyspace flag resolves `t` without USE.
            DynamicArray<U8> b;
            append_cql_long_string(b, "INSERT INTO t (k, v) VALUES (0, 'keyspace-flag');");
            append_be_u16(b, 0x0001);
            append_be_s32(b, 0x0080);
            append_cql_string(b, "big");
            prepend_frame_header(b, op::QUERY, 0, 5);
            send_frame(s, b);
            CHECK(recv_frame(s).opcode == op::RESULT);
        }

        // A request envelope larger than one v5 frame, sent as non-self-contained frames.
        AutoString8 insert{"INSERT INTO big.t (k, v) VALUES (1, '"};
        insert += String8(AutoString8{'x', 200000});
        insert += String8("');");
        {
            DynamicArray<U8> b;
            append_cql_long_string(b, String8(insert));
            append_be_u16(b, 0x0001);
            append_be_s32(b, 0x00);
            prepend_frame_header(b, op::QUERY, 11, 5);
            send_v5_split(s, b);
            Frame f = recv_frame(s);
            CHECK(f.opcode == op::RESULT);
            CHECK(f.stream == 11);
        }

        // A response envelope larger than one v5 frame.
        Frame sel = send_query(s, "SELECT * FROM big.t;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(sel.body.length > 200000);
        CHECK(body_contains(sel, "keyspace-flag"));
        CHECK(send_query(s, "SELECT v FROM big.t WHERE k = 0;").opcode == op::RESULT);
        signal_notify_safe(interrupt);
    });
    co_return;
}
