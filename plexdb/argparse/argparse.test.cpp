#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.argparse;

using namespace plexdb;
using namespace plexdb::argparse;

TEST_CASE("parse two positional arguments", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_positional(parser, "db_path", "Path to database");
    add_positional(parser, "port", "Port number");

    const char* argv[] = {"test_prog", "test.db", "8080"};
    auto result = parse(parser, 3, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(!result.help_requested);
    REQUIRE(result.positional_count == 2);
    REQUIRE(get_positional(result, 0) == "test.db");
    REQUIRE(get_positional(result, 1) == "8080");
}

TEST_CASE("parse long flag", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_flag(parser, "--daemon", "-d", "Run as daemon");

    const char* argv[] = {"test_prog", "--daemon"};
    auto result = parse(parser, 2, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(has_flag(result, 0));
}

TEST_CASE("parse short flag", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_flag(parser, "--daemon", "-d", "Run as daemon");

    const char* argv[] = {"test_prog", "-d"};
    auto result = parse(parser, 2, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(has_flag(result, 0));
}

TEST_CASE("absent flag returns false", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_flag(parser, "--daemon", "-d", "Run as daemon");

    const char* argv[] = {"test_prog"};
    auto result = parse(parser, 1, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(!has_flag(result, 0));
}

TEST_CASE("missing positional returns error", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_positional(parser, "db_path", "Path to database");

    const char* argv[] = {"test_prog"};
    auto result = parse(parser, 1, const_cast<char**>(argv));

    REQUIRE(!result.ok);
    REQUIRE(!result.help_requested);
}

TEST_CASE("unknown option returns error", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");

    const char* argv[] = {"test_prog", "--unknown"};
    auto result = parse(parser, 2, const_cast<char**>(argv));

    REQUIRE(!result.ok);
}

TEST_CASE("unexpected positional returns error", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");

    const char* argv[] = {"test_prog", "extra"};
    auto result = parse(parser, 2, const_cast<char**>(argv));

    REQUIRE(!result.ok);
}

TEST_CASE("--help sets help_requested", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_positional(parser, "db_path", "Path to database");

    const char* argv[] = {"test_prog", "--help"};
    auto result = parse(parser, 2, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(result.help_requested);
}

TEST_CASE("-h sets help_requested", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_positional(parser, "db_path", "Path to database");

    const char* argv[] = {"test_prog", "-h"};
    auto result = parse(parser, 2, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(result.help_requested);
}

TEST_CASE("flags and positionals mixed", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "A test program");
    add_positional(parser, "path", "File path");
    add_flag(parser, "--verbose", "-v", "Enable verbose output");
    add_flag(parser, "--dry-run", "", "Dry run mode");

    const char* argv[] = {"test_prog", "--verbose", "myfile.txt"};
    auto result = parse(parser, 3, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(has_flag(result, 0));
    REQUIRE(!has_flag(result, 1));
    REQUIRE(get_positional(result, 0) == "myfile.txt");
}

TEST_CASE("option long form is parsed", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "");
    add_option(parser, "--port", "-p", "Port number", "8080");

    const char* argv[] = {"test_prog", "--port", "9090"};
    auto result = parse(parser, 3, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(get_option(result, 0) == "9090");
}

TEST_CASE("option short form is parsed", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "");
    add_option(parser, "--port", "-p", "Port number", "8080");

    const char* argv[] = {"test_prog", "-p", "1234"};
    auto result = parse(parser, 3, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(get_option(result, 0) == "1234");
}

TEST_CASE("option default value is used when absent", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "");
    add_option(parser, "--port", "-p", "Port number", "8080");

    const char* argv[] = {"test_prog"};
    auto result = parse(parser, 1, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(get_option(result, 0) == "8080");
}

TEST_CASE("option without value returns error", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "");
    add_option(parser, "--port", "-p", "Port number", "8080");

    const char* argv[] = {"test_prog", "--port"};
    auto result = parse(parser, 2, const_cast<char**>(argv));

    REQUIRE(!result.ok);
}

TEST_CASE("option mixed with positionals and flags", "[plexdb.argparse]") {
    auto parser = make_parser("test_prog", "");
    add_positional(parser, "db_path", "Database path");
    add_option(parser, "--port", "-p", "Port number", "8080");
    add_flag(parser, "--repl", "-r", "Run REPL");

    const char* argv[] = {"test_prog", "mydb.db", "--port", "3000", "--repl"};
    auto result = parse(parser, 5, const_cast<char**>(argv));

    REQUIRE(result.ok);
    REQUIRE(get_positional(result, 0) == "mydb.db");
    REQUIRE(get_option(result, 0) == "3000");
    REQUIRE(has_flag(result, 0));
}
