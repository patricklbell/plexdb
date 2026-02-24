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
- Build: `ninja -C build`
- Run all: `./build/plexdb/plexdb_tests --skip-benchmarks` or `./build/objstore/objstore_tests --skip-benchmarks`
- Run specific tags: `./build/objstore/objstore_tests --skip-benchmarks "[tagname]"`

## Benchmarks
- Catch2 supports benchmarks but use them sparingly
- Always use `--skip-benchmarks` for normal test runs
- Carefully consider before adding new benchmarks

## Code Style in Tests
- Follow the same minimal commenting style as production code
- Do not explain obvious test setup or assertions
- Do not add tests which will obviously pass. Do not check static constants or configuration value. Focus on testing behavior and edge cases.
- Decouple tests from implementation details where possible to allow for refactoring without breaking tests.
