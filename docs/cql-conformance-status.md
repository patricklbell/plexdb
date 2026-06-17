## CQL conformance gaps

Baseline: 97 / 313 passing, 40 xfailed, 3 xpassed, 13 skipped, 160 failed
(scylladb ref: master, 2026-06-17, after Phase 5 follow-ups).

The 160 failures partition by primary exception into ~50 server crashes (aborts), ~37
server-returned errors (non-crash), and ~73 test-side check failures. Fire/hit counts in
the first two tables are higher than the unique-test count because a single test can
trigger multiple aborts or error hits during setup or teardown.

### Unimplemented-CQL aborts

| Fires | Symptom (assert string) | Site |
|------:|-------------------------|------|
|  15 | SELECT clause type (function/cast/term) is not implemented | `planner.cpp` |
|  12 | BATCH is not implemented | `engine.cpp` |
|   5 | User-defined types are not implemented (CREATE/DROP TYPE) | `engine.cpp` |
|   4 | subscript/field access in UPDATE SET is not implemented | `planner.cpp` |
|   2 | tuple column type is not implemented | `schema.cpp` |
|   2 | PER PARTITION LIMIT is not implemented | `engine.cpp` |
|   2 | GROUP BY is not implemented | `engine.cpp` |
|   2 | aggregate SELECT (COUNT(*), etc.) is not implemented | `engine.cpp` |
|   2 | key serialization for this type is not implemented | `key.cppm` |
|   1 | writing null column values is not implemented | `io.cpp` |
|   1 | writing integer value to this dtype is not implemented | `io.cpp` |

### Non-crash failures (server returns an error, test still fails)

| Hits | Server `message=` | Notes |
|-----:|-------------------|-------|
|  14 | `cannot create index on collection column` | Phase 4 rejects collection-column indexes; lands in Phase 8 with the per-element iteration that DML needs. |
|   8 | `Failed to parse CQL` | Remaining parser gaps: a few `ALTER` shapes and `INSERT ... USING TTL AND TIMESTAMP`. |
|   7 | `Cannot apply counter operations on non-counter column <name>` | Tests that use `col = col + n` against non-counter columns; the planner type-checks the assignment instead of falling through. |
|   6 | `INSERT USING TTL is not implemented` | Returned as `Invalid` (not a crash). Phase 10. |
|   4 | `Incompatible literal for column type` | smallint/tinyint and reversed type cases. |
|   4 | `Cannot execute this query as it might involve data filtering ... use ALLOW FILTERING` | Index path should serve some of these directly; needs additional planner work to suppress the gate. |
|   2 | `Failed to create table` | Schema setup error for invalid CREATE TABLE shapes (testInvalidCreateTableStatements, testTable). |
|   1 | `Keyspace 'with' does not exist` | Parser/keyword collision: bareword `with` is consumed as a keyspace identifier. |

### Test-side check failures (server did not crash or return an error)

These tests run to completion against the server but fail their own assertions. They are
not visible in server logs and need conformance-driven planner/executor work to fix.

| Tests | Category | Notes |
|------:|----------|-------|
|  36 | `AssertionError` — wrong rows, wrong count, wrong order, or wrong error regex | The server returned a result, but content or order differs from Cassandra. Includes `Expected regex:` mismatches where we reject a query with a different message than upstream. Concentrated in `*_test.py` files for `select`, `filtering`, `allow_filtering`, and `static_column` semantics. |
|  23 | `Failed: DID NOT RAISE` — server accepted a query it should have rejected | 15 expected `InvalidRequest`, 4 expected `SyntaxException`, 3 expected `ConfigurationException`, 1 either. Missing validation in planner (restriction rules, type checking) and parser (reject malformed shapes). |
|  13 | `ValueError: Too many arguments provided to bind()` | Driver-side bind mismatch — the server returns prepared-statement metadata with a different column count than the test binds. Likely from misreporting `?` placeholders in DML returning columns. |
|   2 | `cassandra.protocol.ErrorMessage: Failed to create table` | Two schema setup failures: `testInvalidCreateTableStatements`, `testTable`. Generic `Unknown` (code=0x0001) — engine returns the wrong error code for these CREATE TABLE shapes. |
|   1 | `TypeError: object of type 'int' has no len()` | `testFilterWithIndexForContains` — server returns an int where a collection is expected, driver crashes during deserialization. |

---

## Summary of structural issues

- **SELECT scalar functions / aggregates (~17+2=19 fires, ~12 unique tests).** Aggregation stage
  above `RowIterator` plus scalar function registry (`blobAsInt`, `intAsBlob`, etc.). Phase 6.
  The fire count grew after Phase 5 because tests that previously aborted on ORDER BY now
  reach the function-call selection codepath.

- **Counter columns (~7 hits as "non-counter column" errors).** The reject-on-wrong-type
  path now works; the actual counter-evaluation path (`col = col + n` on a real counter column) still
  needs `apply_mutation` to provide a row context. Phase 7.

- **Collection subscript / append DML (~4 fires, subscript/field access asserts).** `col[k] = v` and
  `col = col + {…}` require read-modify-write on the collection bytes. Phase 8.

- **Collection-column secondary indexes (~14 hits as `cannot create index on collection column`).**
  Bundled with Phase 8 — the per-element iteration is the same machinery.

- **BATCH (~12 fires, ~11 unique tests).** Single engine entry point wrapping child statements in one
  transaction. Phase 9. Conditional BATCH (LWT) deferred indefinitely.

- **TTL (~6 "INSERT USING TTL" non-crash errors, ~6 unique tests).** Row blob metadata header required.
  Phase 10.

- **User-defined types (~5 fires).** `CREATE TYPE` / `ALTER TYPE` / `DROP TYPE` are parser+schema work
  not currently scheduled in a phase.

- **PER PARTITION LIMIT (~2 fires).** Engine needs to enforce per-partition row cap during SELECT iteration.

- **GROUP BY (~2 fires).** Not scheduled.

- **Tuple column type (~2 fires).** `schema.cpp` rejects `tuple<...>` as a column type.

- **`Incompatible literal for column type` (~4 hits).** Mostly smallint/tinyint coercion and reversed
  types — `cast_write_evaluated_as_column_value` needs to accept more shapes.

- **Partitioner ordering.** `testIndexQueryWithCompositePartitionKey` and similar tests expect
  Murmur3 token order on partitions; plexdb sorts by raw partition-key bytes. See the Phase 4
  follow-ups in `cql-conformance-roadmap.md`.
