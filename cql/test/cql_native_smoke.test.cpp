#include <cql/test_macros/test_macros.h>

import plexdb.os;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

CQL_NATIVE_TEST_CASE("smoke: STARTUP returns READY", "[cql.native][smoke]") {
    test::run_native_server(fixture, [](Socket& client, Notifier& interrupt) {
        test::Frame ready = test::send_startup(client);
        CHECK(ready.version == test::RESPONSE_VERSION);
        CHECK(ready.opcode == test::op::READY);
        CHECK(ready.body_len == 0);
        signal_notify_safe(interrupt);
    });
    co_return;
}

CQL_NATIVE_TEST_CASE("smoke: round-trip insert/select", "[cql.native][smoke]") {
    test::run_native_server_with_handshake(fixture, [](Socket& client, Notifier& interrupt) {
        CHECK(test::send_query(client, "CREATE KEYSPACE ks;").opcode == test::op::RESULT);
        CHECK(test::send_query(client, "CREATE TABLE ks.users (id int PRIMARY KEY, name text);").opcode == test::op::RESULT);
        CHECK(test::send_query(client, "INSERT INTO ks.users (id, name) VALUES (1, 'Alice');").opcode == test::op::RESULT);

        test::Frame sel = test::send_query(client, "SELECT * FROM ks.users;");
        CHECK(sel.opcode == test::op::RESULT);
        CHECK(test::result_kind(sel) == test::result::ROWS);
        CHECK(test::body_contains(sel, "Alice"));

        signal_notify_safe(interrupt);
    });
    co_return;
}
