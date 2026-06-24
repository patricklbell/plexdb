## CQL conformance gaps

Baseline: 149 / 313 passing, 39 xfailed, 4 xpassed, 13 skipped, 108 failed
(scylladb ref: master, 2026-06-24, after counter gating, UNSET, basic TTL, and the
collection-patch DML / collection-index rework).

The 108 failures partition by primary symptom into 13 server crashes (NoHostAvailable
from server-side aborts), 27 server-returned errors (non-crash), and 68 test-side
check failures. Fire counts in the abort/error tables are higher than the
unique-test count because a single test can trigger multiple aborts or error hits
during setup or teardown.

### Unimplemented-CQL aborts (13 unique tests, 58 fires)

| Fires | Symptom (assert string) | Site |
|------:|-------------------------|------|
|  15 | SELECT clause type (function/cast/term) is not implemented | `planner.cpp` |
|  12 | BATCH is not implemented | `engine.cpp` |
|  10 | User-defined types are not implemented (CREATE/DROP TYPE) | `engine.cpp` |
|   4 | tuple column type is not implemented | `schema.cpp` |
|   4 | PER PARTITION LIMIT is not implemented | `engine.cpp` |
|   4 | GROUP BY is not implemented | `engine.cpp` |
|   4 | key serialization for this type is not implemented | `key.cppm` |
|   2 | DESC ordering on variable-width clustering column is not implemented | `key.cppm` |
|   2 | writing integer value to this dtype is not implemented | `io.cpp` |
|   1 | writing null column values is not implemented | `io.cpp` |

The previous "subscript/field access in UPDATE SET" (4 fires) line is gone — collection
patches landed and the `col[k] = v` / `col = col + {…}` planner branches no longer abort.

### Non-crash failures (server returns an error, test still fails)

| Hits | Server `message=` | Notes |
|-----:|-------------------|-------|
|   8 | `Undefined column name Unknown function <fn>` | Conversion built-ins (`blobAsInt`, `blobAsBigint`, `dateof`) and `token()` are unregistered. See roadmap "Type-conversion built-ins". |
|   6 | `Incompatible literal for column type` | smallint/tinyint and reversed-type coercion in `cast_write_evaluated_as_column_value`. |
|   3 | `Failed to parse CQL` | Remaining parser gaps: a few `ALTER` shapes and `INSERT ... USING TTL AND TIMESTAMP`. |
|   3 | `Conditional statements (IF / IF NOT EXISTS) inside BATCH are not supported` | `testInvalidCustomTimestamp` paths — BATCH LWT rejection is correct, but it short-circuits before the test's expected validation fires. |
|   3 | `Cannot execute this query as it might involve data filtering ... use ALLOW FILTERING` | Index path should serve some of these directly; needs planner work to suppress the gate. |
|   2 | `Column <name> is assigned twice in UPDATE` | Over-strict; Cassandra collapses duplicate assignments in some shapes the parser is rejecting. |
|   2 | `Failed to create table` (code=0x0001 Unknown) | `testInvalidCreateTableStatements`, `testTable` — engine returns the wrong error code for these CREATE TABLE shapes. |
|   1 | `Cannot apply counter operations on non-counter column` | Down from 7 — counter-gating per Cassandra spec landed. The one remaining hit is a regex-mismatch where upstream phrases the error differently (see "wrong error message" in the test-side table). |
|   1 | `Keyspace 'with' does not exist` | Parser/keyword collision: bareword `with` is consumed as a keyspace identifier. |

### Test-side check failures (server did not crash or return an error)

These tests run to completion against the server but fail their own assertions. They are
not visible in server logs and need conformance-driven planner/executor work to fix.

| Tests | Category | Notes |
|------:|----------|-------|
|  36 | `AssertionError` — wrong rows, count, or order | The server returned a result, but content or order differs from Cassandra. Concentrated in `select_test.py`, `delete_test.py`, `static_column` semantics, and `collections_test.py` UNSET/large-payload paths. |
|  13 | `Failed: DID NOT RAISE InvalidRequest` | Missing validation in planner (restriction rules, type checking). |
|   8 | `AssertionError: Regex pattern did not match` | Server rejected the query but with a different message than upstream. Cosmetic for most; affects test-suite alignment. |
|   5 | `ValueError: Too many arguments provided to bind()` | Down from 13. Remaining cases are large-collection / SSTable-flush paths where prepared-statement metadata still reports the wrong column count. |
|   4 | `Failed: DID NOT RAISE SyntaxException` | Parser accepts malformed shapes it should reject. |
|   3 | `Failed: DID NOT RAISE ConfigurationException` | Replication-strategy validation in `ALTER KEYSPACE` / `CREATE KEYSPACE` (NTS DC names, missing options). |
|   2 | Other assertions | `testAlterIndexInterval` (option round-trip), `testInOrderByWithoutSelecting` (timestamp ordering), `testTokenRange` (row count). |
|   1 | `TypeError: Got a non-map object for a map value` | `testMapBulkRemoval` — server returns a set where a map is expected on the wire. |

---

## Summary of structural issues

- **SELECT scalar functions (~15 fires).** Function-call selectors in SELECT clauses
  (`writetime`, `ttl`, `blobAsInt`, `intAsBlob`, etc.) still abort in `planner.cpp`'s
  `Select::Function`/`Select::Cast` branch. Aggregation (`COUNT(*)`, `COUNT(1)`) landed in
  Phase 6; the scalar function registry remains unscheduled. Distinct from the
  non-crash "Unknown function" hits (8) which arrive via the term-evaluation path.

- **BATCH (~12 fires, ~11 unique tests).** Single engine entry point wrapping child
  statements in one transaction. Phase 9. Conditional BATCH (LWT) deferred indefinitely;
  the engine already rejects BATCH IF cleanly, which is what the 3 `Conditional
  statements ... inside BATCH` hits show.

- **User-defined types (~10 fires, up from 5).** `CREATE TYPE` / `ALTER TYPE` /
  `DROP TYPE` are parser+schema work not currently scheduled in a phase. The fire
  count grew because more tests reach the UDT setup before failing.

- **Tuple column type (~4 fires).** `schema.cpp` rejects `tuple<...>` as a column type.

- **PER PARTITION LIMIT (~4 fires, 2 unique tests).** Engine needs to enforce
  per-partition row cap during SELECT iteration.

- **GROUP BY (~4 fires, 2 unique tests).** Not scheduled.

- **Variable-width DESC clustering (~2 fires + 4 key-serialization fires).** `key.cppm`'s
  `append_escaped_terminated` and `append_component` need the inverted-escape scheme
  for `text`/`blob`/`hex` under DESC, plus a missing dtype branch in the default arm.
  See the roadmap "Variable-width DESC clustering columns" entry.

- **Counter columns — fixed.** The "non-counter column" error count dropped from 7 to 1
  after the counter-gating commit. The remaining hit is a regex-message mismatch.

- **Collection subscript / append DML — landed.** `col[k] = v` and `col = col + {…}`
  no longer abort; `testUpdateWithStaticList` and a few static-column variants still
  fail with wrong-row assertions but no crash.

- **Collection-column secondary indexes — landed.** All `testMapKeyContainsWithIndex` /
  `testListContainsWithIndex` / `testContainsKeyAndContainsWithIndexOnMap*` etc. now
  PASS; the prior 14 `cannot create index on collection column` hits are gone.

- **TTL — partially landed.** `testInsertWithTtl` XPASSes; `testMixedTTLOnColumns(Wide)`
  and `testUpdateWithTtl` PASS. The previous 6 `INSERT USING TTL is not implemented`
  hits are gone. Remaining TTL work is exposed through the scalar-function path
  (`testTimestampTTL` chains `blobAsBigint(bigintAsBlob(writetime(c)))`).

- **UNSET — landed.** `testTimestampsOnUnsetColumns(Wide)` PASS. The remaining
  `testSetWithUnsetValues` / `testMapWithUnsetValues` / `testListWithUnsetValues`
  failures are now `Incompatible literal` (collection-literal coercion), not the prior
  Unset-handling crashes.

- **`Incompatible literal for column type` (~6 hits, up from 4).** Mostly smallint/tinyint
  coercion and reversed types, with three additional hits from UNSET-in-collection-literal
  paths — `cast_write_evaluated_as_column_value` needs to accept more shapes.

- **Partitioner ordering.** `testIndexQueryWithCompositePartitionKey` and similar tests
  expect Murmur3 token order on partitions; plexdb sorts by raw partition-key bytes. See
  the "Token-based partition ordering" entry in `cql-conformance-roadmap.md`.
