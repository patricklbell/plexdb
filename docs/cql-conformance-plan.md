## CQL conformance plan — single-node scope

Companion to [`cql-conformance-status.md`](cql-conformance-status.md).

Covers remaining failing test categories that do **not** require multi-replica
coordination. Multi-node items (standalone LWT, conditional BATCH) are listed
at the bottom as out of scope.

**Current state:** 194 / 313 passing, 63 failing, 13 skipped, 35 xfailed, 8 xpassed.
**Mustpass coverage:** 186 / 186.

Phases are independently shippable. Done phases are kept here for context.

---

### Phase 1 — Murmur3 token order for partitions  *(done)*

Cassandra orders partitions by the Murmur3 token of the partition key; the
BTree partition key encoding now prepends an 8-byte sign-bit-XOR'd token so
byte-wise lex compare reproduces signed token order. A vendored PeterScott
Murmur3 reference is checked into `cql/third_party/murmur3/` and is exercised
by `cql/engine/token.test.cpp` against canonical and Cassandra reference
vectors; the production implementation in `cql/engine/token.cpp` matches the
Cassandra-flavoured Murmur3_x64_128 (tail bytes sign-extended through Java's
`byte` widening).

`token(...)` is plumbed in three positions: SELECT projection (new
`SelectOp::Token`, routed through `execute_select_with_meta`), WHERE
(`TokenRelation` evaluates the row's token via `key::compute_partition_token`
and applies the operator), and term position (registered in the evaluator
registry so `token(<literal>)` works on the RHS).

PK `IN (...)` no longer walks the BTree in Murmur3 order; the new
`execute_select_pk_in` iterates `pk.in_values` in input order to match
Cassandra semantics, supports DISTINCT and LIMIT, and uses the same
`make_virtual_rows_shell` plumbing as the index and ORDER-BY paths.

---

### Phase 2 — Scalar function dispatch (term-evaluation path)  *(partially done)*

**Done.**
- `blobAsInt` / `blobAsBigint` decode 4-byte / 8-byte BE blobs (previously
  stubbed to `Null`).
- `token(...)` registered in the term registry; computes the Murmur3
  partition token over the active table's partition key column types.
- Planner `token()` error wording aligned with upstream — unblocks the
  three `testTokenFct*` rejection tests.

**Remaining.**
- Planner fallback so SELECT projections can dispatch to the term registry
  for general functions (e.g. `SELECT uuid()`, `SELECT now()` — these
  currently error with `Unknown function`).
- `Select::Cast` planner branch (still `assert_not_implemented` —
  `SELECT CAST(c AS bigint)` style).
- Term-position `TypeHint` validation (`UPDATE SET t = (text)X` type
  mismatch). Needed for `testTypeCasts`.

**Files.** `cql/engine/planner.cpp`, `cql/engine/evaluator.cpp`.

---

### Phase 3 — Wire-protocol BATCH opcode  *(done)*

`cql/native/native.cpp` parses CQL v4 and v5 BATCH frames inline: type byte,
query count, per-child `<kind><string-or-id><n_values><values>`, then
consistency + flags + optional serial CL / default timestamp / keyspace (v5)
/ now_in_seconds (v5). Each child's positional binds are applied via the
newly-exported `engine::bind_values_to_statement` before the children are
packaged into a `Batch{}` Statement and dispatched through `engine::execute`.
Conditional children continue to be rejected by the engine's existing batch
path; non-mutation children (e.g. SELECT) return `Invalid`. Coverage in
`cql/native/native.test.cpp` ([cql.native][batch]).

None of the upstream cassandra-tests use the cassandra-driver
`BatchStatement` API — they send inline `BEGIN BATCH ... APPLY BATCH`
strings via QUERY, which already worked. Phase 3 unblocks driver-level
batch usage end-to-end but does not move the conformance count directly.

---

### Phase 4 — User-defined types

10 aborts across `testNonExistingOnes`, `testAlterTypeUsedInPartitionKey`,
`testFieldSelectionOrderSingleClustering`,
`testInsertingCollectionsWithInvalidElements`, several
`testFilteringOn*WithDurations`. Strictly schema + type binding — no replica
coordination.

**Scope.**
- Parser grammar for `CREATE TYPE`, `ALTER TYPE` (`ADD` / `RENAME`),
  `DROP TYPE`.
- Schema representation: per-keyspace type table; serialize alongside table
  schema using the existing schema-page layout.
- Type resolution: when a column type names an unknown identifier, look it
  up in the keyspace's UDT table before erroring.
- Wire encoding for UDT literals (currently aborts at `io.cpp:771` and
  `io.cpp:896`).
- Populate `system_schema.types` virtual table.
- Replace the three `engine.cpp:3420-3426` asserts with real handlers.

**Files.** `cql/parsers/parsers.cpp`, `cql/engine/schema.{cpp,cppm}`,
`cql/engine/io.cpp`, `cql/engine/system_schema.cpp`,
`cql/engine/engine.cpp`.

---

### Phase 5 — Variable-width DESC clustering + missing key dtypes  *(done)*

The `key.cppm:274` default-arm `assert_not_implemented` had stopped firing
in conformance well before this phase, and round-trip tests in
`cql/engine/key.test.cpp` now confirm that variable-width clustering keys
under DESC already round-trip correctly under the existing byte-wise
inversion (the previous `@note` claiming otherwise was outdated and has
been rewritten).

**Done.**
- `date` (4-byte BE, sign-XOR like `int`) and `time` (8-byte BE like
  `bigint`) added to `append_component` / `decode_component` /
  `cv_to_const_eval` / `append_wire_partition_component` /
  `index_prefix_len` so they work in partition keys, clustering keys,
  and secondary-index prefixes.
- New round-trip unit tests cover fixed-width (bigint, int, smallint,
  time, date, timestamp), boolean, uuid, and variable-width composite
  (text, blob with embedded NULs and prefix relationships) under both
  ASC and DESC.

**Remaining.** `inet`, `decimal`, `varint`, `duration` as keys are still
unimplemented; no current conformance tests reach them as PK / CK columns.

---

### Phase 6 — Remaining parser / validator gaps

**Parser strictness.**
- `testDoubleWith` (alter_test, create_test) — parser accepts
  `CREATE KEYSPACE WITH WITH DURABLE_WRITES = true` by binding the second
  `WITH` as a keyspace identifier. Needs `kw_with` / other reserved keywords
  rejected from `identifier` in keyspace-name and option-key positions, or
  a post-parse check for keyword collisions on `ks_name`.
- 4 `DID NOT RAISE SyntaxException` paths from select/update/where shapes.

**Validation.**
- ~10 `DID NOT RAISE InvalidRequest` — concentrated in `select_test.py`
  restriction rules. Phase 8 cleared the clustering-key chain rules; the
  remainder are mostly partition-key and ALLOW FILTERING shapes
  (`testAllowFilteringOnPartitionKey*`).

**Error shape / cosmetic.**
- `testAdderNonCounter` — wants exact message
  `Invalid operation (a = a + 1) for non counter column a`. Needs a
  `to_str(const Term&)` pretty-printer (none exists today) so the engine
  can embed the offending assignment text.
- ~6 `Regex pattern did not match` — align rejection messages with upstream
  where the fix is a single string.

**Files.** `cql/parsers/parsers.cpp`, `cql/engine/engine.cpp`,
`cql/engine/planner.cpp`, new `cql/engine/term_to_str.{cpp,cppm}` (or
extension of existing pretty-printers).

---

### Phase 7 — SELECT alias propagation through the native layer  *(done)*

`ExecutionResult.select_col_aliases` is a new
`DynamicArray<Optional<AutoString8>>` parallel to `select_col_indices`. The
SELECT planner populates it from `stmt.select.clauses[i].as` for every
clause that carries one. The raw-Rows path in `native.cpp` and the three
VirtualRows callers (`execute_select_index`,
`execute_select_pk_in_ordered`, `execute_select_pk_in`) override the
schema column name when an alias is present. The cell-meta and aggregate
paths already honored aliases.

A test using aliases may still fail for unrelated reasons
(`testSelectWithAlias` and `testAlias` depend on `USING TIMESTAMP` /
specific WHERE-alias error wording — both outside Phase 7).

---

### Phase 8 — Specific errors for broken clustering-key restriction chains  *(done)*

Previously the planner rejected all CK chain violations with the generic
`RequiresAllowFiltering` message; Cassandra emits one of two specific
errors. `KeyConstraints` now tracks per-position `range_seen` alongside
`eq_vals` / `in_seen`. The CK chain validator in `build_row_locator`
scans positions and finds the lowest restricted position whose preceding
positions include either a range slot or an unrestricted slot:

- Range case → `ClusteringRestrictedAfterNonEq`, message
  `Clustering column "X" cannot be restricted (preceding column "Y" is
  restricted by a non-EQ relation)`.
- Unrestricted case → `ClusteringRestrictedWithoutPrefix`, message
  `PRIMARY KEY column "X" cannot be restricted as preceding column "Y"
  is not restricted`.

Both wordings match Cassandra exactly. Both are bypassable by
`ALLOW FILTERING` (the chain check gates on `stmt.allow_filtering`). The
tuple-range case `(c1, c2) > (v1, v2)` is one multi-column restriction,
not k separate ones, so only the leading column is marked `range_seen`.

Unblocks `testFilteringOnClusteringColumns` in conformance.

---

### Phase 9 — Remaining feature gaps (low individual counts)

- **Tuple column type** (4 aborts, `schema.cpp:45`). Tuple bind values
  already work end-to-end (per recent commits); this is column-type
  acceptance + key encoding + storage roundtrip.
- **PER PARTITION LIMIT** (2 aborts, 2 unique tests including
  `testPerPartitionLimitWithStaticDataAndPaging`). Engine row-iteration
  cap; integrates with `RowIterator` partition transitions.
- **GROUP BY** (2 aborts, 2 unique tests). Projection aggregation grouped
  by a clustering-key prefix.
- **Bind-metadata column count** (5 `Too many arguments to bind`).
  Remaining cases are large-collection / SSTable-flush paths where PREPARE
  metadata reports the wrong column count.
- **`testMapBulkRemoval` wire type.** Server returns a set where a map is
  expected on the wire.
- **`testAlterIndexInterval`** — index option round-trip.
- **`testInOrderByWithoutSelecting`** — `c2 IN (…)` + ORDER BY where the
  ordered column is not in the projection.
- **Frozen collection equality filtering** (e.g.
  `testFilteringWithoutIndicesWithFrozenCollections`) — engine-side: the
  WHERE evaluator does not yet equality-compare frozen collection columns.

---

### Engine cleanup (uncovered during earlier phases)

These aren't directly blocking conformance tests today, but were flushed
out during the pass and should be tracked.

- **`schema.cpp:783`** — `delete_table` rollback path called after a
  partial `create_table` (e.g. column duplicate hit late) writes a
  tombstone byte to blob storage that hasn't been allocated yet, causing
  SIGSEGV. Currently avoided by hoisting duplicate-name detection into
  the engine layer (`engine.cpp:2524`). Fix would be a proper "remove
  last-added partial table from `ks.tbls` and skip blob writes" rollback;
  remaining call sites still reach this path on `create_column` failures.
- **drain_columns trailing-Null fill** — drain_columns now fills trailing
  positions with `Null{}` instead of leaving them as default-constructed
  TaggedUnions. `project_virtual_row` mirrors this for absent slots. Both
  guards exist because the native serializer's `visit(ColumnValue)`
  crashes on an empty union; a stronger invariant (e.g. a constructor
  that defaults to `Null{}`) would be safer.

---

## Out of scope — multi-node

These remain deferred. Engine should continue returning `Invalid` with a
clear message; do not invest in partial implementations.

- **Standalone LWT** — `UPDATE … IF`, `DELETE … IF`. Requires Paxos
  consensus across replicas. Single-replica semantics are well-defined
  but ship no tests of value, and the surface area for a sound
  implementation is large.
- **Conditional BATCH** — `BATCH … APPLY` with any child carrying `IF`
  or `IF NOT EXISTS`. Same rationale.
- Tracked in `TODO.md` under "Conditional BATCH and standalone LWT".

---

## Suggested sequencing

Phases 1, 3, 5, 7, 8 are done. Phase 2 is partially done; complete the
SELECT-projection fallback and `Select::Cast` wiring before Phase 6.
Phase 4 (UDTs) is the largest remaining unit; Phase 9 picks up the long
tail opportunistically.

Regenerate `mustpass.txt` after each phase so subsequent regressions are
caught against the larger baseline.
