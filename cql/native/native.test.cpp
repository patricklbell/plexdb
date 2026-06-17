#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>

#include <plexdb/macros/macros.h>
#include <plexdb/test_macros/test_macros.h>
#include <cql/test_macros/test_macros.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;
import plexdb.pager.test_helpers;
import plexdb.aio;
import plexdb.server.test_helpers;

import cql.engine;
import cql.native;
import cql.test_helpers;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;
using namespace cql::test;

CQL_NATIVE_TEST_CASE("Native protocol STARTUP handshake", "[cql.native]") {
    run_native_server(fixture, [](Socket& client, Notifier& interrupt) {
        Frame ready = send_startup(client);
        CHECK(ready.version == RESPONSE_VERSION);
        CHECK(ready.opcode == op::READY);
        CHECK(ready.body_len == 0);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol OPTIONS returns SUPPORTED", "[cql.native]") {
    run_native_server(fixture, [](Socket& client, Notifier& interrupt) {
        Frame supported = send_options(client);
        CHECK(supported.version == RESPONSE_VERSION);
        CHECK(supported.opcode == op::SUPPORTED);
        CHECK(supported.body_len > 0);
        CHECK(body_contains(supported, "CQL_VERSION"));
        CHECK(body_contains(supported, "3.0.0"));
        CHECK(body_contains(supported, "COMPRESSION"));
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol CQL DDL and DML operations", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.users (id int PRIMARY KEY, name text, age int);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.users (id, name, age) VALUES (1, 'Alice', 30);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.users (id, name, age) VALUES (2, 'Bob', 25);").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.users;");
        CHECK(sel.opcode == op::RESULT);
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "Alice"));
        CHECK(body_contains(sel, "Bob"));

        Frame use = send_query(client, "USE ks;");
        CHECK(use.opcode == op::RESULT);
        CHECK(result_kind(use) == result::SET_KEYSPACE);

        CHECK(result_kind(send_query(client, "DROP TABLE ks.users;")) == result::SCHEMA_CHANGE);
        CHECK(result_kind(send_query(client, "DROP KEYSPACE ks;")) == result::SCHEMA_CHANGE);

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol error responses", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "SELECT * FROM no_such_ks.no_table;").opcode == op::ERROR);

        Frame err = send_query(client, "SELECT * FROM missing.tbl;", /*stream=*/42);
        CHECK(err.opcode == op::ERROR);
        CHECK(err.stream == 42);

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol UPDATE modifies existing row", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (id int PRIMARY KEY, name text);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (id, name) VALUES (1, 'Alice');").opcode == op::RESULT);
        CHECK(send_query(client, "UPDATE ks.t SET name = 'Bob' WHERE id = 1;").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "Bob"));
        CHECK(!body_contains(sel, "Alice"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol DELETE removes a row by primary key", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (id int PRIMARY KEY, name text);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (id, name) VALUES (1, 'Alice');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (id, name) VALUES (2, 'Bob');").opcode == op::RESULT);
        CHECK(send_query(client, "DELETE FROM ks.t WHERE id = 1;").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "Bob"));
        CHECK(!body_contains(sel, "Alice"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol TRUNCATE clears all rows", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (id int PRIMARY KEY, name text);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (id, name) VALUES (1, 'Alice');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (id, name) VALUES (2, 'Bob');").opcode == op::RESULT);
        CHECK(send_query(client, "TRUNCATE TABLE ks.t;").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(!body_contains(sel, "Alice"));
        CHECK(!body_contains(sel, "Bob"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol system.local virtual view", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        Frame local = send_query(client, "SELECT * FROM system.local;");
        CHECK(local.opcode == op::RESULT);
        CHECK(result_kind(local) == result::ROWS);
        CHECK(body_contains(local, "local"));
        CHECK(body_contains(local, "datacenter1"));
        CHECK(body_contains(local, "rack1"));
        CHECK(body_contains(local, "cql"));

        Frame peers = send_query(client, "SELECT * FROM system.peers;");
        CHECK(peers.opcode == op::RESULT);
        CHECK(result_kind(peers) == result::ROWS);
        CHECK(body_contains(peers, "peer"));

        Frame peers2 = send_query(client, "SELECT * FROM system.peers_v2;");
        CHECK(peers2.opcode == op::RESULT);
        CHECK(result_kind(peers2) == result::ROWS);

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol system_schema virtual views", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE test_ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE test_ks.users (id int PRIMARY KEY, name text);").opcode == op::RESULT);

        Frame ks = send_query(client, "SELECT * FROM system_schema.keyspaces;");
        CHECK(result_kind(ks) == result::ROWS);
        CHECK(body_contains(ks, "keyspace_name"));
        CHECK(body_contains(ks, "test_ks"));
        CHECK(body_contains(ks, "system_schema"));
        CHECK(body_contains(ks, "system"));

        Frame tbls = send_query(client, "SELECT * FROM system_schema.tables;");
        CHECK(result_kind(tbls) == result::ROWS);
        CHECK(body_contains(tbls, "table_name"));
        CHECK(body_contains(tbls, "users"));
        CHECK(body_contains(tbls, "keyspaces"));
        CHECK(body_contains(tbls, "local"));

        Frame cols = send_query(client, "SELECT * FROM system_schema.columns;");
        CHECK(result_kind(cols) == result::ROWS);
        CHECK(body_contains(cols, "column_name"));
        CHECK(body_contains(cols, "partition_key"));
        CHECK(body_contains(cols, "name"));
        CHECK(body_contains(cols, "clustering"));

        CHECK(send_query(client, "DROP TABLE test_ks.users;").opcode == op::RESULT);
        CHECK(send_query(client, "DROP KEYSPACE test_ks;").opcode == op::RESULT);

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol collection serialization", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        Frame local = send_query(client, "SELECT * FROM system.local;");
        CHECK(result_kind(local) == result::ROWS);

        // tokens column type option is Set<Varchar>
        const U8 set_option[] = {0x00, 0x22, 0x00, 0x0D};
        CHECK(body_contains(local, set_option, 4));

        // tokens set value encodes one-element set containing '0'
        const U8 set_value[] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01,
                                0x00, 0x00, 0x00, 0x01, 0x30};
        CHECK(body_contains(local, set_value, sizeof(set_value)));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol PREPARE and EXECUTE", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE prep_ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE prep_ks.data (id int PRIMARY KEY, name text, score double);").opcode == op::RESULT);

        // Positional bind markers
        {
            Frame prep = send_prepare(client, "INSERT INTO prep_ks.data (id, name, score) VALUES (?, ?, ?)", 1);
            CHECK(result_kind(prep) == result::PREPARED);
            TArrayView<const U8> pid = read_prepared_id(prep);
            CHECK(pid.length == 8);

            DynamicArray<U8> ex;
            append_cql_short_bytes(ex, pid.ptr, U16(pid.length));
            append_be_u16(ex, 0x0001); // consistency = ONE
            append_u8(ex, 0x01);       // flags: values present
            append_be_u16(ex, 3);      // value count
            append_cql_bind_s32(ex, 42);
            append_cql_bind_str(ex, "Alice");
            append_cql_bind_f64(ex, 99.5);
            prepend_v4_header(ex, op::EXECUTE, 2);
            send_frame(client, ex);

            Frame exec = recv_frame(client);
            CHECK(exec.opcode == op::RESULT);
            CHECK(result_kind(exec) == result::VOID);

            Frame sel = send_query(client, "SELECT * FROM prep_ks.data;");
            CHECK(result_kind(sel) == result::ROWS);
            CHECK(body_contains(sel, "Alice"));
        }

        // Named bind markers, executed in reversed order
        {
            Frame prep = send_prepare(client, "INSERT INTO prep_ks.data (id, name, score) VALUES (:id, :name, :score)", 3);
            CHECK(result_kind(prep) == result::PREPARED);
            TArrayView<const U8> pid = read_prepared_id(prep);
            CHECK(pid.length == 8);

            DynamicArray<U8> ex;
            append_cql_short_bytes(ex, pid.ptr, U16(pid.length));
            append_be_u16(ex, 0x0001);
            append_u8(ex, 0x41); // flags: values + named values
            append_be_u16(ex, 3);
            append_cql_named_f64(ex, "score", 77.7);
            append_cql_named_str(ex, "name", "Bob");
            append_cql_named_s32(ex, "id", 99);
            prepend_v4_header(ex, op::EXECUTE, 4);
            send_frame(client, ex);

            Frame exec = recv_frame(client);
            CHECK(exec.opcode == op::RESULT);
            CHECK(result_kind(exec) == result::VOID);

            Frame sel = send_query(client, "SELECT * FROM prep_ks.data WHERE id = 99;");
            CHECK(result_kind(sel) == result::ROWS);
            CHECK(body_contains(sel, "Bob"));
        }

        // Evaluator coverage: constant-folding expressions via QUERY
        CHECK(send_query(client, "CREATE TABLE prep_ks.expr_values (val bigint PRIMARY KEY, tag text);").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE prep_ks.expr_texts (tag text PRIMARY KEY, note text);").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE prep_ks.expr_now (tag text PRIMARY KEY, val bigint);").opcode == op::RESULT);

        struct NumericCase {
            const char* query;
            S64         expected;
            const char* tag;
        };
        const NumericCase numeric_cases[] = {
            {                       "INSERT INTO prep_ks.expr_values (val, tag) VALUES (1 + 2 * 3, 'mul_precedence');",          7, "mul_precedence"},
            {                              "INSERT INTO prep_ks.expr_values (val, tag) VALUES ((1 + 2) * 3, 'paren');",          9,          "paren"},
            {                            "INSERT INTO prep_ks.expr_values (val, tag) VALUES (10 - 4 - 1, 'left_sub');",          5,       "left_sub"},
            {                            "INSERT INTO prep_ks.expr_values (val, tag) VALUES (20 / 5 / 2, 'left_div');",          2,       "left_div"},
            {                                 "INSERT INTO prep_ks.expr_values (val, tag) VALUES (20 % 6 % 4, 'mod');",          2,            "mod"},
            {                                         "INSERT INTO prep_ks.expr_values (val, tag) VALUES (-7, 'neg');",         -7,            "neg"},
            {                          "INSERT INTO prep_ks.expr_values (val, tag) VALUES (toDate(86400000), 'date');",          1,           "date"},
            {     "INSERT INTO prep_ks.expr_values (val, tag) VALUES (toTimestamp(minTimeuuid(1234567890)), 'mints');", 1234567890,          "mints"},
            {     "INSERT INTO prep_ks.expr_values (val, tag) VALUES (toTimestamp(maxTimeuuid(1234567891)), 'maxts');", 1234567891,          "maxts"},
            {"INSERT INTO prep_ks.expr_values (val, tag) VALUES (toUnixTimestamp(minTimeuuid(1234567892)), 'unixts');", 1234567892,         "unixts"},
            {         "INSERT INTO prep_ks.expr_values (val, tag) VALUES (dateOf(minTimeuuid(1234567893)), 'dateof');", 1234567893,         "dateof"},
        };
        for (const auto& c : numeric_cases) {
            CHECK(send_query(client, c.query).opcode == op::RESULT);
            auto  q   = fmt("SELECT tag FROM prep_ks.expr_values WHERE val = %lld;", (long long)c.expected);
            Frame sel = send_query(client, q);
            CHECK(result_kind(sel) == result::ROWS);
            CHECK(body_contains(sel, c.tag));
        }

        CHECK(send_query(client, "INSERT INTO prep_ks.expr_texts (tag, note) VALUES ('hello', 'he' + 'llo');").opcode == op::RESULT);
        Frame txt = send_query(client, "SELECT note FROM prep_ks.expr_texts WHERE tag = 'hello';");
        CHECK(result_kind(txt) == result::ROWS);
        CHECK(body_contains(txt, "hello"));

        const char* now_inserts[] = {
            "INSERT INTO prep_ks.expr_now (tag, val) VALUES ('currentTimestamp', currentTimestamp());",
            "INSERT INTO prep_ks.expr_now (tag, val) VALUES ('currentDate', currentDate());",
            "INSERT INTO prep_ks.expr_now (tag, val) VALUES ('currentTime', currentTime());",
            "INSERT INTO prep_ks.expr_now (tag, val) VALUES ('toTimestampNow', toTimestamp(now()));",
            "INSERT INTO prep_ks.expr_now (tag, val) VALUES ('toUnixTimestampNow', toUnixTimestamp(now()));",
        };
        for (const auto* q : now_inserts) {
            CHECK(send_query(client, q).opcode == op::RESULT);
        }

        Frame now_sel = send_query(client, "SELECT * FROM prep_ks.expr_now;");
        CHECK(result_kind(now_sel) == result::ROWS);
        CHECK(body_contains(now_sel, "currentTimestamp"));
        CHECK(body_contains(now_sel, "currentDate"));
        CHECK(body_contains(now_sel, "currentTime"));
        CHECK(body_contains(now_sel, "toTimestampNow"));
        CHECK(body_contains(now_sel, "toUnixTimestampNow"));

        // Bind marker in prepared statement (bigint)
        {
            Frame prep = send_prepare(client, "INSERT INTO prep_ks.expr_values (val, tag) VALUES (?, 'bind')", 5);
            CHECK(result_kind(prep) == result::PREPARED);
            TArrayView<const U8> pid = read_prepared_id(prep);
            CHECK(pid.length == 8);

            DynamicArray<U8> ex;
            append_cql_short_bytes(ex, pid.ptr, U16(pid.length));
            append_be_u16(ex, 0x0001);
            append_u8(ex, 0x01);
            append_be_u16(ex, 1);
            append_cql_bind_s64(ex, 41);
            prepend_v4_header(ex, op::EXECUTE, 2);
            send_frame(client, ex);
            CHECK(recv_frame(client).opcode == op::RESULT);

            Frame sel = send_query(client, "SELECT tag FROM prep_ks.expr_values WHERE val = 41;");
            CHECK(result_kind(sel) == result::ROWS);
            CHECK(body_contains(sel, "bind"));
        }

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol QUERY with bind values", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE qbind_ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE qbind_ks.items (id int PRIMARY KEY, label text, weight double);").opcode == op::RESULT);

        // Positional
        {
            DynamicArray<U8> b;
            append_cql_long_string(b, "INSERT INTO qbind_ks.items (id, label, weight) VALUES (?, ?, ?)");
            append_be_u16(b, 0x0001);
            append_u8(b, 0x01);
            append_be_u16(b, 3);
            append_cql_bind_s32(b, 7);
            append_cql_bind_str(b, "Gadget");
            append_cql_bind_f64(b, 3.14);
            prepend_v4_header(b, op::QUERY, 1);
            send_frame(client, b);
            CHECK(recv_frame(client).opcode == op::RESULT);
        }
        Frame sel = send_query(client, "SELECT * FROM qbind_ks.items;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "Gadget"));

        // Named
        {
            DynamicArray<U8> b;
            append_cql_long_string(b, "INSERT INTO qbind_ks.items (id, label, weight) VALUES (:id, :label, :weight)");
            append_be_u16(b, 0x0001);
            append_u8(b, 0x41);
            append_be_u16(b, 3);
            append_cql_named_f64(b, "weight", 2.71);
            append_cql_named_s32(b, "id", 13);
            append_cql_named_str(b, "label", "Gizmo");
            prepend_v4_header(b, op::QUERY, 2);
            send_frame(client, b);
            CHECK(recv_frame(client).opcode == op::RESULT);
        }
        Frame named_sel = send_query(client, "SELECT * FROM qbind_ks.items WHERE id = 13;");
        CHECK(result_kind(named_sel) == result::ROWS);
        CHECK(body_contains(named_sel, "Gizmo"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Native protocol collection INSERT and SELECT", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE coll_ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE coll_ks.t ("
                         "  id int PRIMARY KEY,"
                         "  tags list<text>,"
                         "  scores set<int>,"
                         "  meta map<text, int>"
                         ");")
                  .opcode == op::RESULT);

        // Literal collection values
        CHECK(send_query(client,
                         "INSERT INTO coll_ks.t (id, tags, scores, meta)"
                         " VALUES (1, ['a', 'b'], {10, 20}, {'x': 1, 'y': 2});")
                  .opcode == op::RESULT);

        Frame sel1 = send_query(client, "SELECT * FROM coll_ks.t WHERE id = 1;");
        CHECK(result_kind(sel1) == result::ROWS);
        CHECK(body_contains(sel1, "a"));
        CHECK(body_contains(sel1, "b"));

        // Bind-parameter list<text>
        // @todo A build_cql_list / build_cql_map helper would avoid this manual inner-buffer pattern.
        {
            DynamicArray<U8> list_body;
            append_be_s32(list_body, 2);
            append_be_s32(list_body, 5);
            append_bytes(list_body, reinterpret_cast<const U8*>("hello"), 5);
            append_be_s32(list_body, 5);
            append_bytes(list_body, reinterpret_cast<const U8*>("world"), 5);

            DynamicArray<U8> b;
            append_cql_long_string(b, "INSERT INTO coll_ks.t (id, tags) VALUES (?, ?)");
            append_be_u16(b, 0x0001);
            append_u8(b, 0x01);
            append_be_u16(b, 2);
            append_cql_bind_s32(b, 2);
            append_cql_bytes(b, list_body.ptr, S32(list_body.length));
            prepend_v4_header(b, op::QUERY, 10);
            send_frame(client, b);
            CHECK(recv_frame(client).opcode == op::RESULT);
        }
        Frame sel2 = send_query(client, "SELECT * FROM coll_ks.t WHERE id = 2;");
        CHECK(result_kind(sel2) == result::ROWS);
        CHECK(body_contains(sel2, "hello"));
        CHECK(body_contains(sel2, "world"));

        // Bind-parameter map<text, int>
        {
            DynamicArray<U8> map_body;
            append_be_s32(map_body, 1);
            append_be_s32(map_body, 1);
            push_back(map_body, U8('k'));
            U8 v99[4] = {0, 0, 0, 99};
            append_be_s32(map_body, 4);
            append_bytes(map_body, v99, 4);

            DynamicArray<U8> b;
            append_cql_long_string(b, "INSERT INTO coll_ks.t (id, meta) VALUES (?, ?)");
            append_be_u16(b, 0x0001);
            append_u8(b, 0x01);
            append_be_u16(b, 2);
            append_cql_bind_s32(b, 3);
            append_cql_bytes(b, map_body.ptr, S32(map_body.length));
            prepend_v4_header(b, op::QUERY, 11);
            send_frame(client, b);
            CHECK(recv_frame(client).opcode == op::RESULT);
        }
        Frame sel3 = send_query(client, "SELECT * FROM coll_ks.t WHERE id = 3;");
        CHECK(result_kind(sel3) == result::ROWS);
        CHECK(body_contains(sel3, "k"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Static columns: value shared across clustering rows", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.cyclists ("
                         "  country text,"
                         "  name    text,"
                         "  flag    text STATIC,"
                         "  PRIMARY KEY (country, name)"
                         ");")
                  .opcode == op::RESULT);

        CHECK(send_query(client, "INSERT INTO ks.cyclists (country, name, flag) VALUES ('Belgium', 'Boonen', 'BE');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.cyclists (country, name) VALUES ('Belgium', 'Steels');").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.cyclists WHERE country = 'Belgium';");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "Boonen"));
        CHECK(body_contains(sel, "Steels"));
        CHECK(body_contains(sel, "BE"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Static columns: overwrite replaces value partition-wide", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, info text STATIC, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, info) VALUES (1, 10, 'v1');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, info) VALUES (1, 20, 'v2');").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE pk = 1;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "v2"));
        CHECK(!body_contains(sel, "v1"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Static columns: static-only partition returns pk and static value", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.t ("
                         "  p text, c text, v text, s text STATIC,"
                         "  PRIMARY KEY (p, c)"
                         ");")
                  .opcode == op::RESULT);

        CHECK(send_query(client, "INSERT INTO ks.t(p, c, v, s) VALUES ('p1', 'k1', 'v1', 'sv1');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t(p, s) VALUES ('p2', 'sv2');").opcode == op::RESULT);

        // Partition with clustering rows includes static value in each row.
        Frame sel1 = send_query(client, "SELECT * FROM ks.t WHERE p='p1';");
        CHECK(result_kind(sel1) == result::ROWS);
        CHECK(body_contains(sel1, "p1"));
        CHECK(body_contains(sel1, "sv1"));

        // Static-only partition: returns one row with pk='p2', c=null, s='sv2', v=null.
        Frame sel2 = send_query(client, "SELECT * FROM ks.t WHERE p='p2';");
        CHECK(result_kind(sel2) == result::ROWS);
        CHECK(body_contains(sel2, "p2"));
        CHECK(body_contains(sel2, "sv2"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

// DDL: static column on a table without clustering columns must be rejected.
CQL_NATIVE_TEST_CASE("Static columns: rejected on table without clustering key", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        Frame err = send_query(client, "CREATE TABLE ks.bad (id int PRIMARY KEY, info text STATIC);");
        CHECK(err.opcode == op::ERROR);

        signal_notify_safe(interrupt);
    });
    co_return;
}

PAGER_TEST_CASE("Native protocol data persists across restarts", "[cql.native]") {
    U16    port1   = next_test_port();
    U16    port2   = next_test_port();
    Handle db_file = file_tmp();
    REQUIRE(!is_zero_handle(db_file));

    {
        Pager pager = create_test_pager(db_file, 4_kb);
        {
            pager::Transaction tx{&pager};
            co_await tx.begin();
            co_await engine::create_database(pager);
            co_await tx.commit();
        }
        Engine engine;
        co_await engine::init(engine, &pager);

        run_native_server_with_handshake(engine, port1, [&](Socket& client, Notifier& interrupt) {
            CHECK(send_query(client, "CREATE KEYSPACE pks;").opcode == op::RESULT);
            CHECK(send_query(client, "CREATE TABLE pks.data (id int PRIMARY KEY, val text);").opcode == op::RESULT);
            CHECK(send_query(client, "INSERT INTO pks.data (id, val) VALUES (99, 'persisted');").opcode == op::RESULT);
            signal_notify_safe(interrupt);
        });
        destroy_test_pager(pager);
    }

    {
        Pager  pager = test_pager(db_file);
        Engine engine;
        co_await engine::init(engine, &pager);

        run_native_server_with_handshake(engine, port2, [&](Socket& client, Notifier& interrupt) {
            Frame sel = send_query(client, "SELECT * FROM pks.data;");
            CHECK(result_kind(sel) == result::ROWS);
            CHECK(body_contains(sel, "persisted"));
            signal_notify_safe(interrupt);
        });
        destroy_test_pager(pager);
    }
    file_close(db_file);
}

PAGER_TEST_CASE("Native protocol data persists across WAL-enabled restart", "[cql.native]") {
    U16         port1    = next_test_port();
    U16         port2    = next_test_port();
    Handle      pid      = process_get_handle();
    AutoString8 db_path  = fmt("/tmp/plexdb_cql::wal_%" PLEXDB_FMT_U64 "_db", pid.u64[0]);
    AutoString8 wal_path = fmt("/tmp/plexdb_cql::wal_%" PLEXDB_FMT_U64 "_wal", pid.u64[0]);

    if (file_exists(db_path)) {
        file_delete(db_path);
    }
    if (file_exists(wal_path)) {
        file_delete(wal_path);
    }

    {
        Handle db    = file_open(db_path);
        Handle wal   = file_open(wal_path);
        Pager  pager = create_test_pager(db, wal, 4_kb);
        {
            pager::Transaction tx{&pager};
            co_await tx.begin();
            co_await engine::create_database(pager);
            co_await tx.commit();
        }
        Engine engine;
        co_await engine::init(engine, &pager);

        run_native_server_with_handshake(engine, port1, [&](Socket& client, Notifier& interrupt) {
            CHECK(send_query(client, "CREATE KEYSPACE wks;").opcode == op::RESULT);
            CHECK(send_query(client, "CREATE TABLE wks.entries (id int PRIMARY KEY, msg text);").opcode == op::RESULT);
            CHECK(send_query(client, "INSERT INTO wks.entries (id, msg) VALUES (1, 'durable');").opcode == op::RESULT);
            signal_notify_safe(interrupt);
        });
        destroy_test_pager(pager);
        file_close(db);
        file_close(wal);
    }

    {
        Handle db    = file_open(db_path);
        Handle wal   = file_open(wal_path);
        Pager  pager = test_pager(db, wal);
        Engine engine;
        co_await engine::init(engine, &pager);

        run_native_server_with_handshake(engine, port2, [&](Socket& client, Notifier& interrupt) {
            Frame sel = send_query(client, "SELECT * FROM wks.entries;");
            CHECK(result_kind(sel) == result::ROWS);
            CHECK(body_contains(sel, "durable"));
            signal_notify_safe(interrupt);
        });
        destroy_test_pager(pager);
        file_close(db);
        file_close(wal);
    }

    file_delete(db_path);
    file_delete(wal_path);
}

PAGER_TEST_CASE("Static columns: persist across reopen", "[cql.native]") {
    U16         port1    = next_test_port();
    U16         port2    = next_test_port();
    Handle      pid      = process_get_handle();
    AutoString8 db_path  = fmt("/tmp/plexdb_cql::static_%" PLEXDB_FMT_U64 "_db", pid.u64[0]);
    AutoString8 wal_path = fmt("/tmp/plexdb_cql::static_%" PLEXDB_FMT_U64 "_wal", pid.u64[0]);

    if (file_exists(db_path)) {
        file_delete(db_path);
    }
    if (file_exists(wal_path)) {
        file_delete(wal_path);
    }

    Handle db_file  = file_open(db_path);
    Handle wal_file = file_open(wal_path);
    REQUIRE(!is_zero_handle(db_file));
    REQUIRE(!is_zero_handle(wal_file));

    {
        Pager pager = create_test_pager(db_file, wal_file, 4_kb);
        {
            pager::Transaction tx{&pager};
            co_await tx.begin();
            co_await engine::create_database(pager);
            co_await tx.commit();
        }
        Engine engine;
        co_await engine::init(engine, &pager);

        run_native_server_with_handshake(engine, port1, [&](Socket& client, Notifier& interrupt) {
            CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
            CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, info text STATIC, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
            CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, info) VALUES (7, 1, 'persistent');").opcode == op::RESULT);
            signal_notify_safe(interrupt);
        });
        destroy_test_pager(pager);
    }

    Handle db2    = file_open(db_path);
    Handle wal2   = file_open(wal_path);
    Pager  pager2 = test_pager(db2, wal2);
    Engine engine2;
    co_await engine::init(engine2, &pager2);

    run_native_server_with_handshake(engine2, port2, [&](Socket& client, Notifier& interrupt) {
        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE pk = 7;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "persistent"));
        signal_notify_safe(interrupt);
    });
    destroy_test_pager(pager2);
    file_close(db2);
    file_close(wal2);
    file_close(db_file);
    file_close(wal_file);
    file_delete(db_path);
    file_delete(wal_path);
}

// ============================================================================
// SIGKILL crash-consistency test
//   Uses fork() to simulate a hard crash during writes. Cannot use
//   ServerFixture because the child process must manage its own pager.
//   @todo body_contains is a substring search — false positives are possible
//   if row content collides with frame metadata. A parse_rows helper would
//   make assertions exact.
// ============================================================================
PAGER_TEST_CASE("crash consistency after SIGKILL during writes", "[cql.native]") {
    Handle      pid      = process_get_handle();
    AutoString8 db_path  = fmt("/tmp/plexdb_cql::crash_%" PLEXDB_FMT_U64 "_db", pid.u64[0]);
    AutoString8 wal_path = fmt("/tmp/plexdb_cql::crash_%" PLEXDB_FMT_U64 "_wal", pid.u64[0]);

    if (file_exists(db_path)) {
        file_delete(db_path);
    }
    if (file_exists(wal_path)) {
        file_delete(wal_path);
    }

    U16 port1 = next_test_port();
    U16 port2 = next_test_port();

    int confirmed_count = GENERATE(take(1, random(5, 20)));

    Notifier ready{};
    auto     child_opt = process_fork();
    REQUIRE(static_cast<bool>(child_opt));
    Handle child = *child_opt;

    if (is_zero_handle(child)) {
        Handle db    = file_open(db_path);
        Handle wal   = file_open(wal_path);
        Pager  pager = create_test_pager(db, wal, 4_kb);
        {
            pager::Transaction tx{&pager};
            co_await tx.begin();
            co_await engine::create_database(pager);
            co_await tx.commit();
        }
        Engine eng;
        co_await engine::init(eng, &pager);

        Poll     poll{};
        Notifier interrupt{};
        auto     signal_consumer = aio::create_notifier_consumer(interrupt, poll);
        native::run(port1, eng, [&ready] { signal_notify_safe(ready); }, false, g_test_sync_consumer, signal_consumer, poll);
        process_exit(0);
    }

    {
        U8 b;
        stream_read(ready.read, &b, 1);
    }

    {
        Socket client = client_connect(port1);
        handshake(client);
        REQUIRE(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        REQUIRE(send_query(client, "CREATE TABLE ks.t (id int PRIMARY KEY, val text);").opcode == op::RESULT);

        for (int i = 0; i < confirmed_count; i++) {
            AutoString8 q = fmt("INSERT INTO ks.t (id, val) VALUES (%d, 'row_%d');", i, i);
            REQUIRE(send_query(client, q.c_str).opcode == op::RESULT);
        }
        // Fire-and-forget: this row may or may not survive the kill.
        {
            AutoString8 q = fmt("INSERT INTO ks.t (id, val) VALUES (%d, 'row_%d');",
                                confirmed_count, confirmed_count);
            send_frame(client, build_query(q.c_str));
        }
    }

    signal_send_kill(child);
    process_wait(child);

    Handle db    = file_open(db_path);
    Handle wal   = file_open(wal_path);
    Pager  pager = test_pager(db, wal);
    Engine eng2;
    co_await engine::init(eng2, &pager);

    run_native_server_with_handshake(eng2, port2, [&](Socket& client, Notifier& interrupt) {
        Frame sel = send_query(client, "SELECT * FROM ks.t;");
        CHECK(result_kind(sel) == result::ROWS);
        for (int i = 0; i < confirmed_count; i++) {
            AutoString8 val = fmt("row_%d", i);
            CHECK(body_contains(sel, val.c_str));
        }
        signal_notify_safe(interrupt);
    });

    destroy_test_pager(pager);
    file_close(db);
    file_close(wal);
    file_delete(db_path);
    file_delete(wal_path);
}

CQL_NATIVE_TEST_CASE("CREATE TABLE WITH: unknown options are ignored", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.t (id int PRIMARY KEY, val text) "
                         "WITH default_time_to_live = 0 "
                         "AND compaction = {'class': 'LeveledCompactionStrategy'} "
                         "AND gc_grace_seconds = 864000 "
                         "AND unknown_option = 'ignored';")
                  .opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (id, val) VALUES (1, 'a');").opcode == op::RESULT);
        Frame sel = send_query(client, "SELECT * FROM ks.t;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "a"));
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ALTER TABLE WITH: unknown options are ignored", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (id int PRIMARY KEY, val text);").opcode == op::RESULT);
        CHECK(send_query(client, "ALTER TABLE ks.t WITH gc_grace_seconds = 0 AND default_time_to_live = 0;")
                  .opcode == op::RESULT);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ALTER KEYSPACE WITH: unknown options are ignored", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "ALTER KEYSPACE ks WITH replication = {'class': 'SimpleStrategy'} "
                         "AND durable_writes = true;")
                  .opcode == op::RESULT);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("INSERT: collection literal values (list, set, map)", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.t ("
                         "  id int PRIMARY KEY,"
                         "  tags list<text>,"
                         "  codes set<int>,"
                         "  meta map<text, text>"
                         ");")
                  .opcode == op::RESULT);

        CHECK(send_query(client,
                         "INSERT INTO ks.t (id, tags, codes, meta) "
                         "VALUES (1, ['a', 'b'], {10, 20}, {'k': 'v'});")
                  .opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE id = 1;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "a"));
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("UPDATE: collection literal assignment", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.t ("
                         "  id int PRIMARY KEY,"
                         "  tags list<text>"
                         ");")
                  .opcode == op::RESULT);

        CHECK(send_query(client, "INSERT INTO ks.t (id, tags) VALUES (1, ['old']);").opcode == op::RESULT);
        CHECK(send_query(client, "UPDATE ks.t SET tags = ['new', 'val'] WHERE id = 1;").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE id = 1;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "new"));
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("CREATE TABLE: frozen collection column", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.t ("
                         "  id int PRIMARY KEY,"
                         "  nums frozen<list<int>>"
                         ");")
                  .opcode == op::RESULT);

        CHECK(send_query(client, "INSERT INTO ks.t (id, nums) VALUES (1, [10, 20, 30]);").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE id = 1;");
        CHECK(result_kind(sel) == result::ROWS);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("CREATE TABLE: composite partition key", "[cql.native]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.t ("
                         "  a int,"
                         "  b text,"
                         "  val int,"
                         "  PRIMARY KEY ((a, b))"
                         ");")
                  .opcode == op::RESULT);

        CHECK(send_query(client, "INSERT INTO ks.t (a, b, val) VALUES (1, 'x', 42);").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE a = 1 AND b = 'x';");
        CHECK(result_kind(sel) == result::ROWS);
        signal_notify_safe(interrupt);
    });
    co_return;
}

// Regression: SELECT with CK equality/range on a deleted row must return empty, not crash.
// The bug was that apply_ck_bounds_on_clustering could leave clustering_it == clustering_end_it
// (when no CK row satisfied the bound), and deref() would then dereference the end iterator.
CQL_NATIVE_TEST_CASE("Clustering key: SELECT returns empty after CK row deleted", "[cql.native][clustering]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client,
                         "CREATE TABLE ks.t (pk int, ck int, val text, PRIMARY KEY (pk, ck));")
                  .opcode == op::RESULT);

        // Use multi-char values to avoid collisions with column name substrings in the binary
        // response body ('a' is in 'val', 'c' is in 'ck', so single-char checks are unreliable).
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, val) VALUES (1, 1, 'aaa');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, val) VALUES (1, 2, 'bbb');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, val) VALUES (1, 3, 'ccc');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, val) VALUES (2, 1, 'xxx');").opcode == op::RESULT);

        // Verify CK equality SELECT works BEFORE any deletion.
        Frame sel_before = send_query(client, "SELECT * FROM ks.t WHERE pk = 1 AND ck = 2;");
        CHECK(result_kind(sel_before) == result::ROWS);
        CHECK(body_contains(sel_before, "bbb"));

        // Delete the middle CK row from partition 1.
        CHECK(send_query(client, "DELETE FROM ks.t WHERE pk = 1 AND ck = 2;").opcode == op::RESULT);

        // CK equality on the deleted row must return empty (not crash).
        Frame sel_deleted = send_query(client, "SELECT * FROM ks.t WHERE pk = 1 AND ck = 2;");
        CHECK(result_kind(sel_deleted) == result::ROWS);
        CHECK(!body_contains(sel_deleted, "bbb"));

        // Surviving rows in the same partition are still visible.
        Frame sel1 = send_query(client, "SELECT * FROM ks.t WHERE pk = 1 AND ck = 1;");
        CHECK(result_kind(sel1) == result::ROWS);
        CHECK(body_contains(sel1, "aaa"));

        Frame sel3 = send_query(client, "SELECT * FROM ks.t WHERE pk = 1 AND ck = 3;");
        CHECK(result_kind(sel3) == result::ROWS);
        CHECK(body_contains(sel3, "ccc"));

        // Full scan must return all surviving rows across both partitions.
        Frame sel_all = send_query(client, "SELECT * FROM ks.t;");
        CHECK(result_kind(sel_all) == result::ROWS);
        CHECK(body_contains(sel_all, "aaa"));
        CHECK(!body_contains(sel_all, "bbb"));
        CHECK(body_contains(sel_all, "ccc"));
        CHECK(body_contains(sel_all, "xxx"));

        // Delete all remaining CK rows from partition 1 via range.
        CHECK(send_query(client, "DELETE FROM ks.t WHERE pk = 1 AND ck >= 1;").opcode == op::RESULT);

        // Range SELECT on now-empty partition must return empty (not crash).
        Frame sel_range = send_query(client, "SELECT * FROM ks.t WHERE pk = 1 AND ck >= 1;");
        CHECK(result_kind(sel_range) == result::ROWS);
        CHECK(!body_contains(sel_range, "aaa"));
        CHECK(!body_contains(sel_range, "ccc"));

        // Partition 2 must be unaffected.
        Frame sel_p2 = send_query(client, "SELECT * FROM ks.t WHERE pk = 2;");
        CHECK(result_kind(sel_p2) == result::ROWS);
        CHECK(body_contains(sel_p2, "xxx"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Static columns: INSERT merges with existing static values", "[cql.native][static]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, s1 text STATIC, s2 text STATIC, v text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, s1, s2) VALUES (1, 'aaa', 'bbb');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, s1) VALUES (1, 'ccc');").opcode == op::RESULT);
        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE pk = 1;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "ccc")); // s1 updated
        CHECK(body_contains(sel, "bbb")); // s2 preserved
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Static columns: partition removed after all statics deleted", "[cql.native][static]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, s text STATIC, v text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, s) VALUES (1, 'hello');").opcode == op::RESULT);
        Frame before = send_query(client, "SELECT * FROM ks.t WHERE pk = 1;");
        CHECK(result_kind(before) == result::ROWS);
        CHECK(body_contains(before, "hello"));
        CHECK(send_query(client, "DELETE s FROM ks.t WHERE pk = 1;").opcode == op::RESULT);
        Frame after = send_query(client, "SELECT * FROM ks.t WHERE pk = 1;");
        CHECK(result_kind(after) == result::ROWS);
        CHECK(!body_contains(after, "hello"));
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Delete: range delete on specific column is invalid", "[cql.native][clustering]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, v text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, v) VALUES (1, 1, 'xxx');").opcode == op::RESULT);
        Frame err = send_query(client, "DELETE v FROM ks.t WHERE pk = 1 AND ck >= 1;");
        CHECK(err.opcode == op::ERROR);
        Frame sel = send_query(client, "SELECT * FROM ks.t WHERE pk = 1 AND ck = 1;");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "xxx"));
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: CREATE INDEX and SELECT via indexed column", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.users (id int PRIMARY KEY, name text, age int);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.users (id, name, age) VALUES (1, 'Alice', 30);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.users (id, name, age) VALUES (2, 'Bob', 25);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.users (id, name, age) VALUES (3, 'Carol', 30);").opcode == op::RESULT);

        Frame ci = send_query(client, "CREATE INDEX ON ks.users (age);");
        CHECK(ci.opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT id, name FROM ks.users WHERE age = 30;");
        CHECK(sel.opcode == op::RESULT);
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "Alice"));
        CHECK(body_contains(sel, "Carol"));
        CHECK(!body_contains(sel, "Bob"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: backfill on CREATE INDEX covers pre-existing rows", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int PRIMARY KEY, city text);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, city) VALUES (10, 'London');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, city) VALUES (20, 'Paris');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, city) VALUES (30, 'London');").opcode == op::RESULT);

        CHECK(send_query(client, "CREATE INDEX ON ks.t (city);").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT pk, city FROM ks.t WHERE city = 'London';");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "London"));
        CHECK(!body_contains(sel, "Paris"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: UPDATE maintains index", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int PRIMARY KEY, v text);").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE INDEX ON ks.t (v);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, v) VALUES (1, 'aaa');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, v) VALUES (2, 'bbb');").opcode == op::RESULT);

        CHECK(send_query(client, "UPDATE ks.t SET v = 'bbb' WHERE pk = 1;").opcode == op::RESULT);

        Frame bbb = send_query(client, "SELECT pk FROM ks.t WHERE v = 'bbb';");
        CHECK(result_kind(bbb) == result::ROWS);
        // Both pk 1 and 2 now have v = 'bbb'; pk 1 with old value 'aaa' must not appear under 'aaa'.
        Frame aaa = send_query(client, "SELECT pk FROM ks.t WHERE v = 'aaa';");
        CHECK(result_kind(aaa) == result::ROWS);
        CHECK(!body_contains(aaa, "aaa")); // no rows match

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: DELETE removes row from index", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int PRIMARY KEY, name text, tag text);").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE INDEX ON ks.t (tag);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, name, tag) VALUES (1, 'Alice', 'x');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, name, tag) VALUES (2, 'Bob', 'x');").opcode == op::RESULT);

        CHECK(send_query(client, "DELETE FROM ks.t WHERE pk = 1;").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT name FROM ks.t WHERE tag = 'x';");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "Bob"));
        CHECK(!body_contains(sel, "Alice"));
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: DROP INDEX removes index", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int PRIMARY KEY, v text);").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE INDEX idx ON ks.t (v);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, v) VALUES (1, 'hello');").opcode == op::RESULT);

        CHECK(send_query(client, "DROP INDEX ks.idx;").opcode == op::RESULT);

        // After drop, query requires ALLOW FILTERING (no index to use).
        Frame err = send_query(client, "SELECT pk FROM ks.t WHERE v = 'hello';");
        CHECK(err.opcode == op::ERROR);

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: CREATE INDEX IF NOT EXISTS is idempotent", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int PRIMARY KEY, v text);").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE INDEX idx ON ks.t (v);").opcode == op::RESULT);
        // Second create with IF NOT EXISTS must not error.
        CHECK(send_query(client, "CREATE INDEX IF NOT EXISTS idx ON ks.t (v);").opcode == op::RESULT);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: clustering table index scan", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, label text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE INDEX ON ks.t (label);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, label) VALUES (1, 10, 'foo');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, label) VALUES (1, 20, 'bar');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, label) VALUES (2, 10, 'foo');").opcode == op::RESULT);

        Frame sel = send_query(client, "SELECT pk, ck, label FROM ks.t WHERE label = 'foo';");
        CHECK(result_kind(sel) == result::ROWS);
        CHECK(body_contains(sel, "foo"));
        CHECK(!body_contains(sel, "bar"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Secondary index: system_schema.indexes lists created indexes", "[cql.native][index]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int PRIMARY KEY, v text);").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE INDEX my_idx ON ks.t (v);").opcode == op::RESULT);

        Frame idx_sel = send_query(client, "SELECT * FROM system_schema.indexes;");
        CHECK(result_kind(idx_sel) == result::ROWS);
        CHECK(body_contains(idx_sel, "my_idx"));
        CHECK(body_contains(idx_sel, "COMPOSITES"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ORDER BY DESC returns clustering rows in reverse", "[cql.native][order_by]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, label text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, label) VALUES (1, 1, 'alpha');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, label) VALUES (1, 2, 'beta');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, label) VALUES (1, 3, 'gamma');").opcode == op::RESULT);

        Frame asc = send_query(client, "SELECT label FROM ks.t WHERE pk = 1 ORDER BY ck ASC;");
        CHECK(result_kind(asc) == result::ROWS);
        CHECK(body_index_of(asc, "alpha") < body_index_of(asc, "beta"));
        CHECK(body_index_of(asc, "beta") < body_index_of(asc, "gamma"));

        Frame desc = send_query(client, "SELECT label FROM ks.t WHERE pk = 1 ORDER BY ck DESC;");
        CHECK(result_kind(desc) == result::ROWS);
        CHECK(body_index_of(desc, "gamma") < body_index_of(desc, "beta"));
        CHECK(body_index_of(desc, "beta") < body_index_of(desc, "alpha"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ORDER BY DESC honors clustering range bounds", "[cql.native][order_by]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, label text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        for (int i = 0; i < 5; i++) {
            char buf[80];
            int  n = snprintf(buf, sizeof(buf), "INSERT INTO ks.t (pk, ck, label) VALUES (1, %d, 'v%d');", i, i);
            CHECK(send_query(client, String8(buf, U64(n))).opcode == op::RESULT);
        }

        Frame fr = send_query(client, "SELECT label FROM ks.t WHERE pk = 1 AND ck >= 1 AND ck < 4 ORDER BY ck DESC;");
        CHECK(result_kind(fr) == result::ROWS);
        CHECK(body_contains(fr, "v1"));
        CHECK(body_contains(fr, "v2"));
        CHECK(body_contains(fr, "v3"));
        CHECK(!body_contains(fr, "v0"));
        CHECK(!body_contains(fr, "v4"));
        CHECK(body_index_of(fr, "v3") < body_index_of(fr, "v2"));
        CHECK(body_index_of(fr, "v2") < body_index_of(fr, "v1"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ORDER BY on non-clustering column is rejected", "[cql.native][order_by]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, v text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        Frame bad = send_query(client, "SELECT * FROM ks.t WHERE pk = 1 ORDER BY v ASC;");
        CHECK(bad.opcode == op::ERROR);

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ORDER BY without partition-key restriction is rejected", "[cql.native][order_by]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk int, ck int, v text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        Frame bad = send_query(client, "SELECT * FROM ks.t ORDER BY ck;");
        CHECK(bad.opcode == op::ERROR);

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ORDER BY can skip a CK column restricted by equality", "[cql.native][order_by]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (a int, b int, c int, d int, PRIMARY KEY (a, b, c));").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (a, b, c, d) VALUES (0, 0, 0, 0);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (a, b, c, d) VALUES (0, 0, 1, 1);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (a, b, c, d) VALUES (0, 0, 2, 2);").opcode == op::RESULT);

        Frame desc = send_query(client, "SELECT c FROM ks.t WHERE a = 0 AND b = 0 ORDER BY c DESC;");
        CHECK(result_kind(desc) == result::ROWS);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("CLUSTERING ORDER BY (col DESC) reverses default scan", "[cql.native][order_by]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (k int, c int, v text, PRIMARY KEY (k, c)) WITH CLUSTERING ORDER BY (c DESC);").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (k, c, v) VALUES (0, 1, 'low');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (k, c, v) VALUES (0, 2, 'mid');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (k, c, v) VALUES (0, 3, 'high');").opcode == op::RESULT);

        // Default scan should follow the table's CLUSTERING ORDER (c DESC) — high first.
        Frame def = send_query(client, "SELECT v FROM ks.t WHERE k = 0;");
        CHECK(result_kind(def) == result::ROWS);
        CHECK(body_index_of(def, "high") < body_index_of(def, "mid"));
        CHECK(body_index_of(def, "mid") < body_index_of(def, "low"));

        // ORDER BY c ASC against a DESC table reverses back to ascending.
        Frame asc = send_query(client, "SELECT v FROM ks.t WHERE k = 0 ORDER BY c ASC;");
        CHECK(result_kind(asc) == result::ROWS);
        CHECK(body_index_of(asc, "low") < body_index_of(asc, "mid"));
        CHECK(body_index_of(asc, "mid") < body_index_of(asc, "high"));

        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("ORDER BY merges across PK IN partitions", "[cql.native][order_by]") {
    run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(send_query(client, "CREATE KEYSPACE ks;").opcode == op::RESULT);
        CHECK(send_query(client, "CREATE TABLE ks.t (pk text, ck int, v text, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, v) VALUES ('a', 3, 'three');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, v) VALUES ('b', 1, 'one');").opcode == op::RESULT);
        CHECK(send_query(client, "INSERT INTO ks.t (pk, ck, v) VALUES ('c', 2, 'two');").opcode == op::RESULT);

        Frame fr = send_query(client, "SELECT v FROM ks.t WHERE pk IN ('a', 'b', 'c') ORDER BY ck;");
        CHECK(result_kind(fr) == result::ROWS);
        CHECK(body_index_of(fr, "one") < body_index_of(fr, "two"));
        CHECK(body_index_of(fr, "two") < body_index_of(fr, "three"));

        signal_notify_safe(interrupt);
    });
    co_return;
}
