# RESP Conformance Tests

Runs redis-py's command-level test suite against `resp_server`, plus
`redis-benchmark` for supported commands.

## Quick start

```bash
# Build the server
cmake -B build -G Ninja
ninja -C build resp_server

# Run all tests
bash extra/resp_tests/run.sh

# Run only the tests whose names match a pattern
bash extra/resp_tests/run.sh -t "test_ping or test_set or test_get"

# Stop on first failure
bash extra/resp_tests/run.sh -x

# List available tests
bash extra/resp_tests/run.sh -l

# Write server log to a directory
bash extra/resp_tests/run.sh -L /tmp/resp_logs
cat /tmp/resp_logs/server.log
```

## Options

| Flag | Description | Default |
|------|-------------|---------|
| `-b <path>` | resp_server binary | `./build/resp/resp_server` |
| `-P <port>` | TCP port | `6399` |
| `-r <ref>` | redis-py git ref/tag for test source | `v4.6.0` |
| `-t <expr>` | pytest `-k` filter expression | run all |
| `-x` | Stop on first failure | off |
| `-l` | List tests only | off |
| `-L <dir>` | Write server stdout/stderr to `<dir>/server.log` | off |

## What passes / what fails

`resp_server` implements a string-only key-value store. Tests for
unimplemented commands fail with an `ERR unknown command` response — this
is expected and shows what has not been implemented yet.

**Passes** (approx): `test_ping`, `test_set`, `test_get`, `test_delete`,
`test_exists`, `test_type`, `test_keys`, `test_scan`, `test_mset`, `test_mget`,
`test_dbsize`, `test_client_*`, `test_select`, `test_command`.

**Fails** (approx): `test_incr`, `test_expire`, `test_ttl`, any list/set/
sorted-set/hash/pubsub/scripting/transaction test.

Run with `-t "test_set or test_get or test_ping or test_mset or test_mget or
test_delete or test_exists or test_keys or test_scan or test_dbsize"` to run
only the tests most likely to pass.

## Architecture

```
extra/resp_tests/
  run.sh              ← fetches upstream tests, sets up venv, runs pytest
  conftest.py         ← server lifecycle, redis fixtures, skip-helper shims
  test_benchmark.py   ← redis-benchmark test (skipped if not in PATH)
  pytest.ini          ← minimal pytest config
  .gitignore          ← ignores redis_tests/, .venv/, logs
```

After `run.sh` runs once, the fetched redis-py tests live in `redis_tests/`
(gitignored).  The directory is structured as a Python package:

```
redis_tests/
  __init__.py
  tests/
    __init__.py
    conftest.py       ← copy of our conftest.py
    test_benchmark.py ← copy of our test_benchmark.py
    test_commands.py  ← fetched from redis/redis-py at the specified ref
```

`redis_tests/` is added to `sys.path` so that `from tests.conftest import
skip_if_server_version_lt` (used in `test_commands.py`) resolves to our
conftest shim rather than redis-py's original conftest.

## Automated agent workflow

### 1. Identify what's failing

```bash
bash extra/resp_tests/run.sh -x -L /tmp/resp_logs
cat /tmp/resp_logs/server.log
```

### 2. Focus on high-impact additions

Priority order for increasing pass rate:

1. **String extras** — INCR, DECR, INCRBY, DECRBY, INCRBYFLOAT, APPEND,
   STRLEN, GETSET, GETEX, GETDEL, SETNX, SETEX, PSETEX
2. **Key-level TTL** — EXPIRE, PEXPIRE, EXPIREAT, PEXPIREAT, TTL, PTTL,
   PERSIST — enables many cross-cutting tests
3. **RENAME / RANDOMKEY / OBJECT**
4. **Transactions** — MULTI, EXEC, DISCARD, WATCH
5. **List / Set / Hash / Sorted-set** — each unlocks a large test class

### 3. Build-and-test cycle

```bash
ninja -C build resp_server
bash extra/resp_tests/run.sh -t "test_incr" -L /tmp/resp_logs
# check /tmp/resp_logs/server.log for crash details
```

### 4. Rules

- **Do not remove existing passing tests.** Run `./build/resp/resp_tests` after
  every change to check for regressions.
- **Unknown commands should return ERR, not crash.** The server should never
  assert-fail on well-formed but unrecognised input.
