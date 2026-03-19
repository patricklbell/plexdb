#include <catch2/catch_test_macros.hpp>
#include "macros.h"

#if !PLEXDB_OS_LINUX
    #error "Process piping in REPL test not implemented for OS"
#endif

#include <sys/wait.h>
#include <unistd.h>
#include <string>

import plexdb.base;
import plexdb.os;
import plexdb.pager;

import objstore.engine;
import objstore.repl;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore;

namespace {
    static Handle fd_to_handle(int fd) { return Handle{.u32={static_cast<U32>(fd)}}; }

    // @note uses fork() to avoid fd issues with Catch2
    std::string run_repl_batch(engine::Engine& eng, const char* input) {
        int in_fds[2], out_fds[2];
        assert(pipe(in_fds) == 0);
        assert(pipe(out_fds) == 0);

        Handle in_read   = fd_to_handle(in_fds[0]);
        Handle in_write  = fd_to_handle(in_fds[1]);
        Handle out_read  = fd_to_handle(out_fds[0]);
        Handle out_write = fd_to_handle(out_fds[1]);

        U64 input_len = 0;
        while (input[input_len] != '\0') input_len++;
        os::stream_write(in_write, input, input_len);
        os::stream_close(in_write);

        pid_t pid = fork();
        assert(pid >= 0);

        if (pid == 0) {
            os::stream_close(out_read);
            repl::run(in_read, out_write, eng);
            os::stream_close(in_read);
            os::stream_close(out_write);
            _exit(0);
        }

        os::stream_close(in_read);
        os::stream_close(out_write);

        std::string out;
        char buf[256];
        while (true) {
            U64 n = os::stream_read(out_read, buf, sizeof(buf));
            if (n == 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        os::stream_close(out_read);

        waitpid(pid, nullptr, 0);
        return out;
    }

    bool contains(const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    }
}

TEST_CASE("REPL create keyspace and table", "[objstore.repl][!mayfail]") {
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    pager::create(db_file, page_size);
    Pager pager{db_file};
    engine::create_database(pager);
    engine::Engine eng{&pager};

    const char* input =
        "CREATE KEYSPACE test WITH replication = 'SimpleStrategy';\n"
        "CREATE TABLE test.items (id int PRIMARY KEY, name text);\n";

    std::string out = run_repl_batch(eng, input);
    REQUIRE(contains(out, "SUCCESS"));
}

TEST_CASE("REPL insert and select", "[objstore.repl]") {
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    pager::create(db_file, page_size);
    Pager pager{db_file};
    engine::create_database(pager);
    engine::Engine eng{&pager};

    const char* input =
        "CREATE KEYSPACE ks;\n"
        "CREATE TABLE ks.t (id int PRIMARY KEY, val text);\n"
        "INSERT INTO ks.t (id, val) VALUES (1, 'hello');\n"
        "SELECT * FROM ks.t;\n";

    std::string out = run_repl_batch(eng, input);
    REQUIRE(contains(out, "hello"));
    REQUIRE(contains(out, "1 rows"));
}

TEST_CASE("REPL reports parse error gracefully", "[objstore.repl]") {
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    pager::create(db_file, page_size);
    Pager pager{db_file};
    engine::create_database(pager);
    engine::Engine eng{&pager};

    std::string out = run_repl_batch(eng, "NOT VALID CQL;\n");
    REQUIRE(contains(out, "ERROR"));
}

TEST_CASE("REPL displays column headers on SELECT", "[objstore.repl]") {
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    pager::create(db_file, page_size);
    Pager pager{db_file};
    engine::create_database(pager);
    engine::Engine eng{&pager};

    const char* input =
        "CREATE KEYSPACE ks;\n"
        "CREATE TABLE ks.t (id int PRIMARY KEY, score int);\n"
        "INSERT INTO ks.t (id, score) VALUES (42, 100);\n"
        "SELECT * FROM ks.t;\n";

    std::string out = run_repl_batch(eng, input);
    REQUIRE(contains(out, "id"));
    REQUIRE(contains(out, "score"));
    REQUIRE(contains(out, "42"));
    REQUIRE(contains(out, "100"));
}
