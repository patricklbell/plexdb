# CQL Conformance Tests

Runs Apache Cassandra CQL tests (ported to Python by ScyllaDB) against `objstore`.

## Quick start

```bash
# Build the server
cmake -B build -G Ninja -DBUILD_TESTS=ON -DPLEXDB_LOG_ENABLED=ON -DPLEXDB_DEBUG=ON
ninja -C build objstore_server

# Run all tests
bash extra/cql_tests/run.sh

# Run a single test (pytest -k expression)
bash extra/cql_tests/run.sh -t "testInsert"

# Stop on first failure
bash extra/cql_tests/run.sh -x -t "testInsert"

# List available tests
bash extra/cql_tests/run.sh -l

# Write server log to a directory
bash extra/cql_tests/run.sh -L /tmp/cql_logs -t "testInsert"
cat /tmp/cql_logs/server.log
```

## Options

| Flag | Description | Default |
|------|-------------|---------|
| `-b <path>` | objstore_server binary | `./build/objstore/objstore_server` |
| `-P <port>` | Native protocol port | `9042` |
| `-r <ref>` | ScyllaDB git ref for test source | `master` |
| `-t <expr>` | pytest `-k` filter expression | run all |
| `-x` | Stop on first failure | off |
| `-l` | List tests only | off |
| `-L <dir>` | Write server stdout/stderr to `<dir>/server.log` | off |

## Error categories

Tests fail when the server crashes on an `assert_not_implemented` or
`assert_true_not_implemented` call. The conftest.py detects the crash, restarts
the server, and the next test continues. The server log (`-L <dir>`) shows the
assert message for each crash.

Common assert messages (check `server.log`):

| Assert message | Meaning | Fix area |
|----------------|---------|----------|
| `"clustering columns in standalone PRIMARY KEY"` | Clustering columns not yet supported | `schema.cpp` table creation |
| `"PREPARE not implemented"` | Prepared statements not supported | `native.cppm` PREPARE handler |
| `"frozen types"` | Frozen collection types not supported | `parsers.cpp` type grammar |
| `"not implemented" at create_table` | Table options not supported | `schema.cpp` create_table |
| `"only constant terms are supported"` | Non-constant terms (e.g. function calls) | `statements.cppm` |
| `"composite partition key"` | Multi-column partition keys not supported | `schema.cpp` get_primary_key_col_idx |
| `"not implemented" at Update/Delete/...` | Statement type not implemented | `engine.cpp` execute |
| `"Failed to parse CQL"` | CQL syntax not handled by parser (non-crash) | `parsers/parsers.cpp` lexy grammar |

## Architecture

```
extra/cql_tests/
  run.sh          ← Fetches upstream tests, sets up venv, runs pytest
  conftest.py     ← Pytest fixtures: server lifecycle, CQL session, keyspace
  util.py         ← Helper functions expected by upstream test porting layer
  nodetool.py     ← No-op shims for nodetool operations
  pytest.ini      ← Minimal pytest config
  .gitignore      ← Ignores fetched tests, venv, pycache
```

Tests are fetched from `scylladb/scylladb` and placed in `scylla_cql_tests/`.
The `conftest.py`, `util.py`, and `nodetool.py` shims are copied into the test
directory to provide the fixtures and helpers the upstream tests expect.

## Automated agent workflow

For agents working on improving CQL conformance pass rate:

### 1. Identify the failure category

```bash
# Run a specific failing test with verbose output
bash extra/cql_tests/run.sh -x -t "testInsert" -L /tmp/cql_logs
cat /tmp/cql_logs/server.log
```

### 2. Focus on high-impact fixes

Priority order:
1. **Parser fixes** (`parsers.cpp`): Many tests fail because the CQL parser doesn't
   handle certain syntax. Adding grammar rules unblocks whole categories of tests.
2. **Engine features** (`engine.cpp`): Implement UPDATE, DELETE, ALTER TABLE, BATCH
   to unblock data manipulation tests.
3. **PREPARE support** (`native.cppm`): Many tests use prepared statements via the
   cassandra-driver. Implementing PREPARE/EXECUTE enables parameterized queries.
4. **WHERE clause** (`engine.cpp`): SELECT currently returns all rows. Adding WHERE
   clause filtering enables most SELECT tests.

### 3. Build and test cycle

```bash
# Build only the server (fast)
ninja -C build objstore_server

# Run the specific test you're fixing
bash extra/cql_tests/run.sh -x -t "testName" -L /tmp/cql_logs

# Check server logs for crash details
cat /tmp/cql_logs/server.log

# Run all tests to measure progress
bash extra/cql_tests/run.sh

# Run C++ unit tests to check for regressions
./build/objstore/objstore_tests --skip-benchmarks "[objstore.parser],[objstore.tcp]"
```

### 4. Rules

- **Never remove existing passing tests.** Run the C++ test suite after every change.
- **Keep `assert_not_implemented` calls.** The server should crash with a meaningful
  message when it encounters input it cannot handle. The conftest.py detects crashes
  and restarts the server for each test. To fix a test, implement the feature so the
  assert is no longer reached — do not convert asserts to error responses.
- **Check `server.log` for crash reasons.** Use `-L /tmp/cql_logs` and read
  `server.log` to see which assert fired for each failing test.
- **Test with `--no-uring`.** There is a known io_uring async write issue; all
  conformance tests run with `--no-uring` (set in `conftest.py`).
