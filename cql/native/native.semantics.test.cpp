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

import cql.engine;
import cql.native;
import cql.test_helpers;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;
using namespace cql::column_types;
using namespace cql::test;

CQL_NATIVE_TEST_CASE("Explicit write timestamps and non-positive limits are rejected", "[cql.native][semantics]") {
    run_native_server_with_handshake(fixture, [](Socket& s, Notifier& interrupt) {
        CHECK(send_query(s, "CREATE KEYSPACE sem;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE sem.t (k int PRIMARY KEY, v text);").opcode == op::RESULT);

        for (String8 q : {
                 String8("INSERT INTO sem.t (k, v) VALUES (1, 'a') USING TIMESTAMP 10;"),
                 String8("UPDATE sem.t USING TIMESTAMP 10 SET v = 'a' WHERE k = 1;"),
                 String8("DELETE FROM sem.t USING TIMESTAMP 10 WHERE k = 1;"),
                 String8("BEGIN BATCH USING TIMESTAMP 10 INSERT INTO sem.t (k, v) VALUES (1, 'a'); APPLY BATCH;"),
             }) {
            Frame f = send_query(s, q);
            CHECK(f.opcode == op::ERROR);
            CHECK(body_contains(f, "USING TIMESTAMP is not supported"));
        }
        CHECK(send_query(s, "SELECT * FROM sem.t;").opcode == op::RESULT);

        CHECK(send_query(s, "SELECT * FROM sem.t LIMIT 0;").opcode == op::ERROR);
        CHECK(send_query(s, "SELECT * FROM sem.t LIMIT -1;").opcode == op::ERROR);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Filtering compares collection, tuple and UDT columns by value", "[cql.native][semantics]") {
    run_native_server_with_handshake(fixture, [](Socket& s, Notifier& interrupt) {
        CHECK(send_query(s, "CREATE KEYSPACE fc;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE fc.t (k int PRIMARY KEY, l frozen<list<int>>, st frozen<set<text>>, tp frozen<tuple<int, text>>, d list<duration>);").opcode == op::RESULT);
        CHECK(send_query(s, "INSERT INTO fc.t (k, l, st, tp) VALUES (1, [1, 2], {'x', 'y'}, (1, 'one'));").opcode == op::RESULT);
        CHECK(send_query(s, "INSERT INTO fc.t (k, l, st, tp) VALUES (2, [3], {'z'}, (2, 'two'));").opcode == op::RESULT);

        Frame eq = send_query(s, "SELECT k FROM fc.t WHERE l = [1, 2] ALLOW FILTERING;");
        CHECK(result_kind(eq) == result::ROWS);
        CHECK(body_contains(eq, String8("\x00\x00\x00\x04\x00\x00\x00\x01", 8)));
        CHECK(!body_contains(eq, String8("\x00\x00\x00\x04\x00\x00\x00\x02", 8)));

        Frame gt = send_query(s, "SELECT k FROM fc.t WHERE l > [1, 9] ALLOW FILTERING;");
        CHECK(body_contains(gt, String8("\x00\x00\x00\x04\x00\x00\x00\x02", 8)));
        CHECK(!body_contains(gt, String8("\x00\x00\x00\x04\x00\x00\x00\x01", 8)));

        Frame in = send_query(s, "SELECT k FROM fc.t WHERE st IN ({'z'}, {'q'}) ALLOW FILTERING;");
        CHECK(body_contains(in, String8("\x00\x00\x00\x04\x00\x00\x00\x02", 8)));

        Frame tuple = send_query(s, "SELECT k FROM fc.t WHERE tp = (1, 'one') ALLOW FILTERING;");
        CHECK(body_contains(tuple, String8("\x00\x00\x00\x04\x00\x00\x00\x01", 8)));

        // Durations are unordered, also inside collections.
        CHECK(send_query(s, "SELECT k FROM fc.t WHERE d > [1s] ALLOW FILTERING;").opcode == op::ERROR);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Sets and maps are returned in element order", "[cql.native][semantics]") {
    run_native_server_with_handshake(fixture, [](Socket& s, Notifier& interrupt) {
        CHECK(send_query(s, "CREATE KEYSPACE so;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE so.t (k int PRIMARY KEY, st set<text>, m map<int, text>);").opcode == op::RESULT);
        CHECK(send_query(s, "INSERT INTO so.t (k, st, m) VALUES (1, {'delta', 'alpha', 'charlie', 'bravo'}, {30: 'm30', -5: 'm-5', 7: 'm7'});").opcode == op::RESULT);
        Frame f = send_query(s, "SELECT st, m FROM so.t WHERE k = 1;");
        CHECK(body_index_of(f, "alpha") < body_index_of(f, "bravo"));
        CHECK(body_index_of(f, "bravo") < body_index_of(f, "charlie"));
        CHECK(body_index_of(f, "charlie") < body_index_of(f, "delta"));
        CHECK(body_index_of(f, "m-5") < body_index_of(f, "m7"));
        CHECK(body_index_of(f, "m7") < body_index_of(f, "m30"));
        signal_notify_safe(interrupt);
    });
    co_return;
}
