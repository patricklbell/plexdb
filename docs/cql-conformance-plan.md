## CQL conformance plan — single-node scope

Companion to [`cql-conformance-status.md`](cql-conformance-status.md).

Covers remaining failing test categories that do **not** require multi-replica
coordination. Multi-node items (standalone LWT, conditional BATCH) are listed
at the bottom as out of scope.

Baseline: **176 / 313 passing, 81 failing, 13 skipped, 37 xfailed, 6 xpassed**.

Phases are ordered by estimated test-count impact, with prerequisites pulled
earlier where they enable later work. Each phase is independently shippable.

---

### Phase 1 — Murmur3 token order for partitions

**Why first.** The largest test-side bucket (~32 wrong-rows / wrong-order
assertions) is dominated by cross-partition ordering. Cassandra orders
partitions by the Murmur3 token of the partition key; plexdb orders by raw
partition-key bytes. Driver tests routinely scan multiple partitions and
`assertRows` on the materialized order. Fixing this also enables the `token()`
selector and `WHERE token(pk) > ?` term shape, which are dependencies for
several other tests (`testTokenRange`, `testTokenFct*`, `testSelectWithToken`).

**Scope.**
- Implement Cassandra's Murmur3 64-bit partitioner (the seeded LongHash
  variant) as a free function. Either extend `cql.engine.key` or add a new
  `cql.engine.token` module.
- Change partition-tier BTree key encoding to prepend the 8-byte big-endian
  token to the raw partition-key bytes. Byte-wise lex order over
  `token || raw_pk` is identical to token order with PK as tiebreaker, so the
  BTree comparator stays unchanged.
- This is an on-disk schema-format change; existing DBs are recreated
  (consistent with the project's "no migration" policy).
- Plumb `token(...)` as a term-evaluation built-in returning `bigint`, usable
  in SELECT, WHERE, and ORDER BY positions.

**Files.** `cql/engine/key.cppm`, `cql/engine/io.cpp` (partition write/read),
`cql/engine/engine.cpp` (partition iteration cursor), `cql/engine/planner.cpp`
(token selector + WHERE), new `cql/engine/token.{cppm,cpp}`.

**Tests.** Add a C++ unit test covering the partition-key encoding round trip
and lex-order equivalence. Re-run `testIndexQueryWithCompositePartitionKey`,
`testTokenRange`, `testCompositeRowKey`, `testTruncate`, and a sample of
`select_test.py` multi-partition cases.

**Estimated impact.** ~15–20 unique tests cleared from the wrong-rows bucket,
plus removes the `Unknown function token()` term-side hits and unblocks the
4 `testTokenFct*` tests.

---

### Phase 2 — Scalar function dispatch (term-evaluation path)

**Why now.** A large cluster of failures all reduce to one missing piece:
a function registry that resolves a name + argument types to an evaluator.
The existing SELECT-only path (`parse_typed_conversion_name`) only covers
`<T>As<U>` casts; everything else dies in one of two ways: an
`assert_not_implemented` for `Select::Function`/`Select::Cast`, or an
`Undefined column name Unknown function <fn>` from the term path.

Tests blocked: `testDateCompatibility`, `testTimestampTTL`,
`testInsertWithUnset` (uses `blobAsInt(?)`), `testTypeCasts`, and several
others that wrap `writetime()` / `ttl()` in `blobAsBigint(bigintAsBlob(...))`.

**Scope.**
- Registry keyed by `(lowercase_name, arg_dtypes)` returning a typed evaluator
  callback.
- Term-evaluation entry point in `evaluator.cpp` so functions work in
  `VALUES`, `WHERE`, and bound subexpressions — not just SELECT.
- Wire the `Select::Cast` planner branch through the same dispatch so it stops
  asserting.
- Initial built-ins: `blobAsInt`, `blobAsBigint`, `intAsBlob`, `bigintAsBlob`,
  `dateof`, `unixtimestampof`, `toUnixTimestamp`, `now`, `currentTimestamp`,
  `currentTimeUUID`, `uuid`, plus the symmetric `<T>As<U>` set already covered
  in the SELECT path. `token()` lands in Phase 1.

**Files.** `cql/engine/planner.cpp`, `cql/engine/evaluator.{cpp,cppm}`, new
`cql/engine/functions.{cpp,cppm}`.

**Estimated impact.** ~12 unique tests directly; unblocks `testTimestampTTL`
and the remaining TTL paths called out in the status doc.

---

### Phase 3 — Wire-protocol BATCH opcode

**Why.** Status doc reports 12 BATCH "not implemented" fires. The engine-side
BATCH executor already exists at `engine.cpp:3428`; the real block is the
native-protocol opcode handler at `cql/native/native.cpp:1110`. This is pure
frame parsing plus a `Batch` construct + dispatch.

**Scope.**
- Parse the BATCH frame body per CQL v4: `<type> <n> <queries> <consistency>
  <flags> [<serial CL>] [<default-timestamp>]`. Each `<query>` is
  `<kind=0|1> <id-or-string> <n> <values>`.
- Build `Batch{kind, using_parameters, statements: [Insert|Update|Delete]}`
  from the parsed body — reuse the prepared-statement lookup path.
- Dispatch through `engine::execute(Batch)`.
- BATCH with any conditional child continues returning `Invalid` with the
  existing message (`engine.cpp:3452` — already correct).

**Files.** `cql/native/native.cpp`, `cql/native/native.cppm` (helpers for
batched-value decoding).

**Estimated impact.** ~11 unique BATCH tests (`testBatch`,
`testBatchWithInRestriction`, `testCounterBatch`, etc.); removes the BATCH
abort row entirely.

---

### Phase 4 — User-defined types

**Why.** 10 aborts across multiple unique tests
(`testNonExistingOnes`, `testAlterTypeUsedInPartitionKey`,
`testFieldSelectionOrderSingleClustering`, `testInsertingCollectionsWithInvalidElements`,
several `testFilteringOn*WithDurations`). Strictly schema + type binding — no
replica coordination.

**Scope.**
- Parser grammar for `CREATE TYPE`, `ALTER TYPE` (`ADD` / `RENAME`), `DROP TYPE`.
- Schema representation: per-keyspace type table; serialize alongside table
  schema using the existing schema-page layout.
- Type resolution: when a column type names an unknown identifier, look it up
  in the keyspace's UDT table before erroring.
- Wire encoding for UDT literals (currently aborts at `io.cpp:771` and
  `io.cpp:896`).
- Populate `system_schema.types` virtual table.
- Replace the three `engine.cpp:3420-3426` asserts with real handlers.

**Files.** `cql/parsers/parsers.cpp`, `cql/engine/schema.{cpp,cppm}`,
`cql/engine/io.cpp`, `cql/engine/system_schema.cpp`, `cql/engine/engine.cpp`.

**Estimated impact.** ~10 aborts removed.

---

### Phase 5 — Variable-width DESC clustering + missing key dtypes

**Why.** 2 DESC-specific abort fires + 4 generic `key serialization for this
type` fires in `key.cppm:274`. Status doc already identifies the exact code
sites and approach.

**Scope.**
- Inverted-escape scheme in `append_escaped_terminated` and `append_component`
  for `text` / `blob` / `hex` clustering columns under DESC order, so byte-wise
  lex compare descends through the column.
- Fill the missing dtype branches in the default arm (identify from
  `server.log` of the four failing tests).
- Round-trip unit tests in `cql/engine/key.test.cpp` covering ASC and DESC for
  every supported clustering dtype.

**Files.** `cql/engine/key.cppm`.

**Estimated impact.** ~6 aborts removed (including `testFunctionsWithClusteringDesc`).

---

### Phase 6 — Remaining parser/validator gaps

A small grab bag still pending after the first conformance pass.

**Parser strictness.**
- `testDoubleWith` (alter_test, create_test) — parser accepts
  `CREATE KEYSPACE WITH WITH DURABLE_WRITES = true` by binding the second
  `WITH` as a keyspace identifier. Needs `kw_with` / other reserved keywords
  rejected from `identifier` in keyspace-name and option-key positions, or a
  post-parse check for keyword collisions on `ks_name`.
- 4 `DID NOT RAISE SyntaxException` paths from select/update/where shapes.

**Validation.**
- ~13 `DID NOT RAISE InvalidRequest` — concentrated in `select_test.py`
  restriction rules and type checking. Investigate per test; many likely
  share a shape (e.g. mixing `ALLOW FILTERING` with explicit restriction
  shapes).

**Error shape / cosmetic.**
- `testAdderNonCounter` — wants exact message
  `Invalid operation (a = a + 1) for non counter column a`. Needs a
  `to_str(const Term&)` pretty-printer (none exists today) so the engine can
  embed the offending assignment text.
- ~8 `Regex pattern did not match` — align rejection messages with upstream
  where the fix is a single string.

**Files.** `cql/parsers/parsers.cpp`, `cql/engine/engine.cpp`,
`cql/engine/planner.cpp`, new `cql/engine/term_to_str.{cpp,cppm}` (or
extension of existing pretty-printers).

**Estimated impact.** ~10 cumulative test-side failures.

---

### Phase 7 — SELECT alias propagation through the native layer

**Why.** `testSelectWithAlias`, `testAlias`, `testSelectDistinct` (and others
using `AS` in selectors) fail because the response column metadata uses the
underlying schema column name instead of the alias. The engine's `Select::SelectColumn`
already carries `as : Optional<AutoString8>`; the native layer just doesn't
see it when it builds the result frame header at `native.cpp:629`.

**Scope.**
- Pipe alias overrides alongside `result.select_col_indices` (e.g. parallel
  `select_col_aliases : DynamicArray<Optional<AutoString8>>`).
- When present, emit the alias instead of `tbl->cols[ci].name` in the columns
  spec.
- Also covers `writetime`/`ttl`/cast aliases once Phase 2 lands.

**Files.** `cql/engine/engine.cppm` (ExecutionResult), `cql/engine/engine.cpp`,
`cql/native/native.cpp`.

**Estimated impact.** ~3 tests.

---

### Phase 8 — Planner: suppress filtering gate when an index serves

**Scope.** 3 `Cannot execute this query as it might involve data filtering …
use ALLOW FILTERING` hits where an existing collection or value index could
answer the predicate directly (`testFilteringWithoutIndicesWithFrozenCollections`,
`testFilteringOnClusteringColumns` shape variants). Identify the predicate
shapes from the failing tests, then route them through index lookup before the
filtering gate fires.

**Files.** `cql/engine/planner.cpp`.

**Estimated impact.** 3 hits; small but improves user-visible behavior.

---

### Phase 9 — Remaining feature gaps (low individual counts)

- **Tuple column type** (4 aborts, `schema.cpp:45`). Tuple bind values
  already work end-to-end (per recent commits); this is column-type acceptance
  + key encoding + storage roundtrip.
- **PER PARTITION LIMIT** (2 aborts, 2 unique tests including
  `testPerPartitionLimitWithStaticDataAndPaging`). Engine row-iteration cap;
  integrates with `RowIterator` partition transitions.
- **GROUP BY** (2 aborts, 2 unique tests). Projection aggregation grouped by
  a clustering-key prefix.
- **Bind-metadata column count** (5 `Too many arguments to bind`). Remaining
  cases are large-collection / SSTable-flush paths where PREPARE metadata
  reports the wrong column count.
- **`testMapBulkRemoval` wire type.** Server returns a set where a map is
  expected on the wire.
- **`testAlterIndexInterval`** — index option round-trip.
- **`testInOrderByWithoutSelecting`** — `c2 IN (…)` + ORDER BY where the
  ordered column is not in the projection.

---

### Engine cleanup (uncovered during Phase 6)

These aren't directly blocking conformance tests today, but were flushed out
during the pass and should be tracked.

- **`schema.cpp:783`** — `delete_table` rollback path called after a partial
  `create_table` (e.g. column duplicate hit late) writes a tombstone byte to
  blob storage that hasn't been allocated yet, causing SIGSEGV. Currently
  avoided by hoisting duplicate-name detection into the engine layer
  (`engine.cpp:2524`). Fix would be a proper "remove last-added partial table
  from `ks.tbls` and skip blob writes" rollback; remaining call sites still
  reach this path on `create_column` failures.

---

## Out of scope — multi-node

These remain deferred. Engine should continue returning `Invalid` with a clear
message; do not invest in partial implementations.

- **Standalone LWT** — `UPDATE … IF`, `DELETE … IF`. Requires Paxos
  consensus across replicas. Single-replica semantics are well-defined but
  ship no tests of value, and the surface area for a sound implementation is
  large.
- **Conditional BATCH** — `BATCH … APPLY` with any child carrying `IF` or
  `IF NOT EXISTS`. Same rationale.
- Tracked in `TODO.md` under "Conditional BATCH and standalone LWT".

---

## Suggested sequencing

1. **Phase 1** — biggest single-phase impact (~15–20 tests) and unblocks
   `token()`-dependent work.
2. **Phase 3** — wire-protocol BATCH; large win for a small, contained change.
3. **Phase 2** — prerequisite for the long tail of "Unknown function" hits
   and a reusable registry the rest of the engine benefits from.
4. **Phase 4–5** — schema work and key encoding; mostly independent.
5. **Phase 6–9** — small fixes; pick up opportunistically once the larger
   phases are in.

After Phase 1–3, regenerate `mustpass.txt` so subsequent regressions are
caught against the larger baseline.
