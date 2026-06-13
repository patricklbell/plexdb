#include <catch2/catch_test_macros.hpp>

#include <plexdb/macros/macros.h>
#include <cql/test_macros/test_macros.h>

#if !PLEXDB_OS_LINUX
#error "Process piping in REPL test not implemented for OS"
#endif

import plexdb.base;
import plexdb.os;

import cql.engine;
import cql.repl;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

namespace {
    // @note uses process_fork() to avoid fd issues with Catch2
    AutoString8 run_repl_batch(Engine& eng, const char* input) {
        auto [in_read, in_write]   = stream_pipe();
        auto [out_read, out_write] = stream_pipe();
        if (is_zero_handle(in_read) || is_zero_handle(out_read)) {
            abort();
        }

        stream_write(in_write, String8(input));
        stream_close(in_write);

        Optional<Handle> child_opt = process_fork();
        if (!child_opt) {
            abort();
        }
        Handle child = *child_opt;

        if (is_zero_handle(child)) {
            stream_close(out_read);
            repl::run(in_read, out_write, eng);
            stream_close(in_read);
            stream_close(out_write);
            process_exit(0);
        }

        stream_close(in_read);
        stream_close(out_write);

        AutoString8 out;
        U8          buf[256];
        while (true) {
            U64 n = stream_read(out_read, buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            out += String8(reinterpret_cast<const char*>(buf), n);
        }
        stream_close(out_read);

        process_wait(child);
        return out;
    }
}

CQL_NATIVE_TEST_CASE("REPL create keyspace and table", "[cql.repl]") {
    const char* input =
        "CREATE KEYSPACE test_ks;\n"
        "CREATE TABLE test_ks.items (id int PRIMARY KEY, name text);\n";

    AutoString8 out = run_repl_batch(fixture.engine, input);
    REQUIRE(contains(String8(out), "SUCCESS"));
    co_return;
}

CQL_NATIVE_TEST_CASE("REPL insert and select", "[cql.repl]") {
    const char* input =
        "CREATE KEYSPACE ks;\n"
        "CREATE TABLE ks.t (id int PRIMARY KEY, val text);\n"
        "INSERT INTO ks.t (id, val) VALUES (1, 'hello');\n"
        "SELECT * FROM ks.t;\n";

    AutoString8 out = run_repl_batch(fixture.engine, input);
    REQUIRE(contains(String8(out), "hello"));
    REQUIRE(contains(String8(out), "1 rows"));
    co_return;
}

CQL_NATIVE_TEST_CASE("REPL reports parse error gracefully", "[cql.repl]") {
    AutoString8 out = run_repl_batch(fixture.engine, "NOT VALID CQL;\n");
    REQUIRE(contains(String8(out), "ERROR"));
    co_return;
}

CQL_NATIVE_TEST_CASE("REPL displays column headers on SELECT", "[cql.repl]") {
    const char* input =
        "CREATE KEYSPACE ks;\n"
        "CREATE TABLE ks.t (id int PRIMARY KEY, score int);\n"
        "INSERT INTO ks.t (id, score) VALUES (42, 100);\n"
        "SELECT * FROM ks.t;\n";

    AutoString8 out = run_repl_batch(fixture.engine, input);
    REQUIRE(contains(String8(out), "id"));
    REQUIRE(contains(String8(out), "score"));
    REQUIRE(contains(String8(out), "42"));
    REQUIRE(contains(String8(out), "100"));
    co_return;
}
