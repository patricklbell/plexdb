#include <catch2/catch_test_macros.hpp>

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
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
    // Runs the REPL in a child process, feeds it all input, and returns its output.
    // Using fork() avoids all fd-number issues (Catch2 may close stdin/stdout).
    std::string run_repl_batch(engine::Engine& eng, const char* input) {
        int in_fds[2], out_fds[2];
        int rc = pipe(in_fds);
        assert(rc == 0);
        rc = pipe(out_fds);
        assert(rc == 0);

        // Write all input before forking so the child never blocks waiting
        write(in_fds[1], input, strlen(input));
        close(in_fds[1]);

        pid_t pid = fork();
        assert(pid >= 0);

        if (pid == 0) {
            // Child: run the REPL
            close(out_fds[0]);
            close(in_fds[1]);
            repl::run(in_fds[0], out_fds[1], eng);
            close(in_fds[0]);
            close(out_fds[1]);
            _exit(0);
        }

        // Parent: read all output until the child closes out_fds[1]
        close(in_fds[0]);
        close(out_fds[1]);

        std::string out;
        char buf[256];
        while (true) {
            ssize_t n = read(out_fds[0], buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
        close(out_fds[0]);

        waitpid(pid, nullptr, 0);
        return out;
    }

    bool contains(const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    }
}

TEST_CASE("REPL create keyspace and table", "[objstore.repl]") {
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
        "CREATE KEYSPACE ks WITH replication = 'SimpleStrategy';\n"
        "CREATE TABLE ks.t (id int PRIMARY KEY, val text);\n"
        "INSERT INTO ks.t VALUES (1, 'hello');\n"
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
        "CREATE KEYSPACE ks WITH replication = 'SimpleStrategy';\n"
        "CREATE TABLE ks.t (id int PRIMARY KEY, score int);\n"
        "INSERT INTO ks.t VALUES (42, 100);\n"
        "SELECT * FROM ks.t;\n";

    std::string out = run_repl_batch(eng, input);
    REQUIRE(contains(out, "id"));
    REQUIRE(contains(out, "score"));
    REQUIRE(contains(out, "42"));
    REQUIRE(contains(out, "100"));
}
