---
applyTo: "**/*.test.cpp"
---

# Test File Guidelines

When writing or modifying test files, follow these guidelines:

## Framework
- Tests use the Catch2 testing framework
- Reference: https://github.com/catchorg/Catch2/blob/devel/docs/

## Conventions
- Test files are named `*.test.cpp` and placed alongside the code they test
- Tests may use STL but should prefer the `plexdb` library where possible
- Use descriptive test case and section names

## Test Structure
```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("descriptive name", "[tag]") {
    SECTION("specific scenario") {
        // Arrange
        // Act
        // Assert with REQUIRE, CHECK, etc.
    }
}
```

## Running Tests
- Build: `cmake -B build -G Ninja -DBUILD_TESTS=ON -DPLEXDB_LOG_ENABLED=ON -DPLEXDB_DEBUG=ON && ninja -C build`
- Run all: `./build/plexdb/plexdb_tests --skip-benchmarks` or `./build/objstore/objstore_tests --skip-benchmarks`
- Run specific tags: `./build/objstore/objstore_tests --skip-benchmarks "[tagname]"`

## Debugging Test Failures
- Build with `-DPLEXDB_LOG_ENABLED=ON` so log messages appear in failing tests.
- All `plexdb::log` messages are routed to Catch2 `UNSCOPED_INFO` by the test log consumer (`test/log_consumer.test.cpp`). When a test fails, all log messages emitted during that test case are printed alongside the failure.
- To add diagnostic logging in production code, use `plexdb::log::fire_message(producer.id, Level::Debug, text, len)`. This is zero-overhead when logging is disabled at build time.
- For numeric metrics without string formatting overhead, use `plexdb::log::fire_stat(producer.id, stat_id, value)`.
- Register stat metadata with `plexdb::log::fire_stat_meta(producer.id, stat_id, "name")` so consumers display human-readable stat names on failure.
- Custom log consumers can be registered via the plugin ABI (`plexdb_log_register_consumer` in `plexdb/log/log_abi.h`). Write a consumer to filter by producer ID, log level, or stat ID.
- The stat plugin (`objstore/plugins/log_stat/log_stat_plugin.cpp`) writes stat events to a file. Graph them with `python extra/plot_stats.py plexdb.stats`.
- Parse errors are reported via `UNSCOPED_INFO` through `objstore/test/parsers_error_reporter.helper.cppm` and also through the log system (`objstore::log::cql_parse_error` at `Level::Error`).

## Benchmarks
- Catch2 supports benchmarks but use them sparingly
- Always use `--skip-benchmarks` for normal test runs
- Carefully consider before adding new benchmarks

## Code Style in Tests
- Follow the same minimal commenting style as production code
- Do not explain obvious test setup or assertions
- Do not add tests which will obviously pass. Do not check static constants or configuration value. Focus on testing behavior and edge cases.
- Decouple tests from implementation details where possible to allow for refactoring without breaking tests.
