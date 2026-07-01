## CQL conformance plan — single-node scope

Companion to [`cql-conformance-status.md`](cql-conformance-status.md).

Covers remaining failing test categories that do **not** require multi-replica
coordination. Multi-node items are listed at the bottom as out of scope.

### Feature — User-defined types

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

### Remaining feature gaps (low individual counts)

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
