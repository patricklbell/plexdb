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

namespace {
    // Row count of a ROWS result whose columns all have single-code (scalar) types.
    S32 row_count(const Frame& f) {
        if (result_kind(f) != result::ROWS) {
            return -1;
        }
        const U8* p      = f.body.ptr + 4;
        auto      rd_s32 = [&p]() { S32 v = S32((U32(p[0]) << 24) | (U32(p[1]) << 16) | (U32(p[2]) << 8) | U32(p[3])); p += 4; return v; };
        auto      rd_u16 = [&p]() { U16 v = U16((U16(p[0]) << 8) | p[1]); p += 2; return v; };
        S32       flags  = rd_s32();
        S32       cols   = rd_s32();
        if (flags & 0x0001) {
            p += rd_u16();
            p += rd_u16();
        }
        for (S32 i = 0; i < cols; i++) {
            p += rd_u16();
            (void)rd_u16();
        }
        return rd_s32();
    }

    S32 count_rows(Socket& s, String8 cql) {
        return row_count(send_query(s, cql));
    }
}

CQL_NATIVE_TEST_CASE("Row liveness: rows written only by UPDATE live through their cells", "[cql.native][liveness]") {
    run_native_server_with_handshake(fixture, [](Socket& s, Notifier& interrupt) {
        CHECK(send_query(s, "CREATE KEYSPACE l;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE l.t (k int PRIMARY KEY, v text, w int);").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE l.c (pk int, ck int, v text, s text static, PRIMARY KEY (pk, ck));").opcode == op::RESULT);

        // Setting a column to null on a row that never existed creates nothing.
        CHECK(send_query(s, "UPDATE l.t SET v = null WHERE k = 1;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 1;") == 0);
        CHECK(send_query(s, "UPDATE l.c SET v = null WHERE pk = 1 AND ck = 1;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT pk, ck FROM l.c WHERE pk = 1;") == 0);

        // An UPDATE-created row disappears with its last cell...
        CHECK(send_query(s, "UPDATE l.t SET v = 'a' WHERE k = 2;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 2;") == 1);
        CHECK(send_query(s, "UPDATE l.t SET v = null WHERE k = 2;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 2;") == 0);

        CHECK(send_query(s, "UPDATE l.t SET v = 'a', w = 1 WHERE k = 3;").opcode == op::RESULT);
        CHECK(send_query(s, "DELETE v FROM l.t WHERE k = 3;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 3;") == 1);
        CHECK(send_query(s, "DELETE w FROM l.t WHERE k = 3;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 3;") == 0);

        CHECK(send_query(s, "UPDATE l.c SET v = 'a' WHERE pk = 2 AND ck = 1;").opcode == op::RESULT);
        CHECK(send_query(s, "DELETE v FROM l.c WHERE pk = 2 AND ck = 1;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT pk, ck FROM l.c WHERE pk = 2;") == 0);

        // ...while an INSERTed row keeps its marker.
        CHECK(send_query(s, "INSERT INTO l.t (k, v) VALUES (4, 'a');").opcode == op::RESULT);
        CHECK(send_query(s, "DELETE v FROM l.t WHERE k = 4;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 4;") == 1);
        CHECK(send_query(s, "UPDATE l.t SET v = 'b' WHERE k = 4;").opcode == op::RESULT);
        CHECK(send_query(s, "UPDATE l.t SET v = null WHERE k = 4;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 4;") == 1);

        CHECK(send_query(s, "INSERT INTO l.t (k) VALUES (5);").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM l.t WHERE k = 5;") == 1);

        CHECK(send_query(s, "INSERT INTO l.c (pk, ck) VALUES (3, 1);").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT pk, ck FROM l.c WHERE pk = 3;") == 1);

        // Deleting the last clustering row keeps the partition's static row.
        CHECK(send_query(s, "INSERT INTO l.c (pk, ck, v, s) VALUES (4, 1, 'row', 'static-kept');").opcode == op::RESULT);
        CHECK(send_query(s, "DELETE FROM l.c WHERE pk = 4 AND ck = 1;").opcode == op::RESULT);
        Frame statics = send_query(s, "SELECT pk, s FROM l.c WHERE pk = 4;");
        CHECK(row_count(statics) == 1);
        CHECK(body_contains(statics, "static-kept"));
        CHECK(send_query(s, "DELETE FROM l.c WHERE pk = 4;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT pk, s FROM l.c WHERE pk = 4;") == 0);

        CHECK(count_rows(s, "SELECT k FROM l.t;") == 2);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Row liveness: marker and cell TTLs expire independently", "[cql.native][liveness][ttl]") {
    run_native_server_with_handshake(fixture, [](Socket& s, Notifier& interrupt) {
        CHECK(send_query(s, "CREATE KEYSPACE lt;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE lt.t (k int PRIMARY KEY, v text);").opcode == op::RESULT);

        // Marker with a TTL, cell refreshed without one: the cell keeps the row alive.
        CHECK(send_query(s, "INSERT INTO lt.t (k, v) VALUES (1, 'a') USING TTL 1;").opcode == op::RESULT);
        CHECK(send_query(s, "UPDATE lt.t SET v = 'kept' WHERE k = 1;").opcode == op::RESULT);
        // Key-only insert with a TTL.
        CHECK(send_query(s, "INSERT INTO lt.t (k) VALUES (2) USING TTL 1;").opcode == op::RESULT);
        // UPDATE-only row whose single cell has a TTL.
        CHECK(send_query(s, "UPDATE lt.t USING TTL 1 SET v = 'gone' WHERE k = 3;").opcode == op::RESULT);
        // Marker without a TTL, cell with one: the row outlives the cell.
        CHECK(send_query(s, "INSERT INTO lt.t (k) VALUES (4);").opcode == op::RESULT);
        CHECK(send_query(s, "UPDATE lt.t USING TTL 1 SET v = 'expires' WHERE k = 4;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM lt.t;") == 4);

        sleep_ms(1600);

        Frame one = send_query(s, "SELECT k, v FROM lt.t WHERE k = 1;");
        CHECK(row_count(one) == 1);
        CHECK(body_contains(one, "kept"));
        CHECK(count_rows(s, "SELECT k FROM lt.t WHERE k = 2;") == 0);
        CHECK(count_rows(s, "SELECT k FROM lt.t WHERE k = 3;") == 0);
        Frame four = send_query(s, "SELECT k, v FROM lt.t WHERE k = 4;");
        CHECK(row_count(four) == 1);
        CHECK(!body_contains(four, "expires"));

        // Deleting the refreshed cell of row 1 leaves only its expired marker.
        CHECK(send_query(s, "DELETE v FROM lt.t WHERE k = 1;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM lt.t WHERE k = 1;") == 0);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("Row liveness: dropping a column removes rows it alone kept alive", "[cql.native][liveness]") {
    run_native_server_with_handshake(fixture, [](Socket& s, Notifier& interrupt) {
        CHECK(send_query(s, "CREATE KEYSPACE ld;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE ld.t (k int PRIMARY KEY, x int, y int);").opcode == op::RESULT);
        CHECK(send_query(s, "UPDATE ld.t SET x = 1 WHERE k = 1;").opcode == op::RESULT);
        CHECK(send_query(s, "INSERT INTO ld.t (k, x) VALUES (2, 1);").opcode == op::RESULT);
        CHECK(send_query(s, "UPDATE ld.t SET x = 1, y = 2 WHERE k = 3;").opcode == op::RESULT);

        CHECK(send_query(s, "ALTER TABLE ld.t DROP x;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT k FROM ld.t WHERE k = 1;") == 0);
        CHECK(count_rows(s, "SELECT k FROM ld.t WHERE k = 2;") == 1);
        CHECK(count_rows(s, "SELECT k FROM ld.t WHERE k = 3;") == 1);

        // Re-adding the column does not resurrect the dropped values.
        CHECK(send_query(s, "ALTER TABLE ld.t ADD x int;").opcode == op::RESULT);
        CHECK(send_query(s, "INSERT INTO ld.t (k, y) VALUES (9, 99);").opcode == op::RESULT);
        Frame two = send_query(s, "SELECT k, x FROM ld.t WHERE k = 2;");
        CHECK(row_count(two) == 1);

        CHECK(send_query(s, "CREATE TABLE ld.c (pk int, ck int, x int, s int static, PRIMARY KEY (pk, ck));").opcode == op::RESULT);
        CHECK(send_query(s, "UPDATE ld.c SET x = 1 WHERE pk = 1 AND ck = 1;").opcode == op::RESULT);
        CHECK(send_query(s, "UPDATE ld.c SET s = 5 WHERE pk = 1;").opcode == op::RESULT);
        CHECK(send_query(s, "ALTER TABLE ld.c DROP x;").opcode == op::RESULT);
        CHECK(count_rows(s, "SELECT pk, ck FROM ld.c WHERE pk = 1 AND ck = 1;") == 0);
        CHECK(count_rows(s, "SELECT pk, s FROM ld.c WHERE pk = 1;") == 1);

        // Key columns and indexed columns cannot be dropped.
        CHECK(send_query(s, "ALTER TABLE ld.c DROP ck;").opcode == op::ERROR);
        CHECK(send_query(s, "CREATE INDEX ON ld.t (y);").opcode == op::RESULT);
        Frame indexed = send_query(s, "ALTER TABLE ld.t DROP y;");
        CHECK(indexed.opcode == op::ERROR);
        CHECK(body_contains(indexed, "dependent secondary indexes"));
        signal_notify_safe(interrupt);
    });
    co_return;
}
