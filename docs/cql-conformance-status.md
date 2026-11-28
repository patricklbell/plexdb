## CQL conformance gaps

> **2026-11-29 update.** Against the current upstream mirror (760 collected tests) the suite
> passes 290 (was 286 on the previous build); `mustpass.txt` is updated accordingly. Fixed since
> the baseline below: bind-marker numbering (§H2), row liveness / row markers and DROP COLUMN
> liveness (§H9), typed filtering on collection/tuple/UDT columns, sorted set/map output, slice
> rejection on types containing `duration`, non-positive `LIMIT`. `USING TIMESTAMP` is now
> rejected instead of ignored (see "Single-node considerations"), which turns eight tests that
> only passed because arrival order matched their timestamps into failures. The sections below
> still describe the 2026-07-13 baseline except where marked.

Baseline: **212 / 313 passing**, 48 failed, 13 skiplisted, 32 xfailed, 8 xpassed
(scylladb ref: master; plexdb, 2026-07-13). Run: `bash tools/cql/conformance_tests/run.sh -b
build-conformance/cql/plexdb_cql_server`.

`tools/cql/conformance_tests/skiplist.txt` deselects 13 tests in two categories:
- **Permissive parser** (2): `testDoubleWith` × 2 — `WITH WITH` is silently accepted on `CREATE`/`ALTER KEYSPACE`. See §H4.
- **Still-crashing paths** (6 skiplist.txt patterns covering 7 tests, plus 4 UDF-porting-shim skips — see below): `frozen<map<int,int>>` as a PK/CK column (§H6) and the UDT-column-type wire gap (§H8) still abort the server if run; skiplisted so one bad test doesn't destabilize the shared-server harness. `testCreatingUDFWithSameNameAsBuiltin_*` is also skipped here — not a plexdb bug, see the table below.

`testNowToUUIDCompatibility`/`testReversedTypeCompatibility` (upstream `@skip_bug`,
flaky against ScyllaDB itself per scylladb/scylladb#9300) pass reliably against
this engine and are not skipped — `conftest.py` only auto-skips the
`skip_not_implemented` marker (missing porting-shim helpers), not `skip_bug`
(an upstream implementation's own flakiness, which says nothing about this one).

Of the 48 failures: **1 crash** (`testFilteringOnDurationColumn`), and the rest
are non-crash server errors or test-side check failures, categorized below.

### Crash (1)

| Test | Site | Notes |
|------|------|-------|
| `testFilteringOnDurationColumn` | `native.cppm:394` — `assert_not_implemented("CQL native protocol encoding for duration is not implemented")` | A duration value reaches the response encoder; the encoder stub is real and fires. See §H10. |

### Non-crash server errors (server rejected the query, wrong content — §H1)

`testAlias`, `testAllowFilteringOnPartitionKeyWithCounters`,
`testAlterTypeUsedInPartitionKey`, `testAdderNonCounter`,
`testBatchWithInRestriction` (masked by the conditional-BATCH rejection
firing first) — all correct rejections with upstream-mismatched wording.
`testInvalidCustomTimestamp` × 2, `testBatchTTLConditionalInteraction`,
`testBatchStaticTTLConditionalInteraction` — correctly rejected as
conditional-BATCH (LWT, single-node choice), which short-circuits before
these tests can validate their own expected message.
`testDateCompatibility`, `testFilteringOnCollectionsWithNull` —
`"Incompatible literal for column type"` (date/time and NULL-in-collection
literal coercion).

### Test-side check failures (server accepted/completed, test's own assertion tripped)

| Category | Tests |
|----------|-------|
| `DID NOT RAISE InvalidRequest` (§H4 permissive parser/decoder) | `testInsertingCollectionsWithInvalidElements`, `testInvalidInputForList/Set/Map`, `testInsertWithUnset`, `testAllowFiltering`, `testMultiSelects`, `testSelectDistinct`, `testSelectSliceFromComposite`, `testSelectWithToken`, `testAllowFilteringOnPartitionKeyWithDistinct`, `testFilteringOnListContainingDurations`, `testFilteringOnMapContainingDurations` (§H2) |
| Wrong rows / count / order | `testMap`, `testSet`, `testBatch`, `testEmptyRestrictionValue` × 4, `testOrderByForInClause` × 2, `testAllowFilteringOnPartitionKey`, `testAllowFilteringOnPartitionKeyOnStaticColumnsWithRowsWithOnlyStaticValues`, `testFilteringOnTupleContainingDurations`, `testFilteringWithoutIndicesWithFrozenCollections` (§H14), `testInOrderByWithoutSelecting` (§H14), `testSelectWithAlias` (last assertion only — §H1) |
| Dropped-column / row-liveness (§H9, expanded below) | `testDropWithTimestamp`, `testDropStaticWithTimestamp`, `testDropMultipleWithTimestamp`, `testAlterOnlyColumnBehaviorWithFlush` |
| Range-tombstone timestamp reconciliation (new — see §H9) | `testReverseQueryWithRangeTombstoneOnMultipleBlocks` |
| Bind-marker numbering (§H2) | `testFieldSelectionOrderSingleClustering` |
| PER PARTITION LIMIT / GROUP BY unimplemented (clean rejection) | `testPerPartitionLimitWithStaticDataAndPaging`, `testIndexOnRegularColumnWithPartitionWithoutRows` |

### Unexpected passes (8 — upstream marks `@pytest.mark.xfail`, plexdb passes)

- **No 64 KiB element limit inside collections**: `testMapsWithElementsBiggerThan64K`, `testSetsWithElementsBiggerThan64K`, `testCKQueryWithValueOver64K` — deliberately not capped; see §H4's note on the (narrower, stranger) upstream behavior.
- **Genuine features that landed**: `testInsertWithTtl`, `testSelectDistinctWithWhereClauseOnStaticColumn`, `testFilteringWithMultiColumnSlices`, `testFilteringWithOrderClause`, `testCreateKeyspaceWithSimpleStrategyNoOptions`.

---

## Current implementation hacks being hit

### §H1 — Error-message strings drift from Cassandra's format

The engine wires literal strings that upstream regex-matches against. Parser
rule-name leakage is now fully gone (the one remaining case, `testCounterBatch`,
was actually a parser bug — see §H5 — not a message issue).

- `engine.cpp:1521` — `"Cannot apply counter operations on non-counter column X"` vs. upstream `"Invalid operation (X = X + 1) for non counter column X"`. Affects `testAdderNonCounter`.
- `engine.cpp:1490` — `"Order by is currently only supported on the clustered columns of the PRIMARY KEY, got " + result.context`, empty context on the code path that runs (`testAllowFilteringOnPartitionKeyWithCounters`).
- `engine.cpp:1462` — the ALLOW FILTERING message is generic; upstream `testAlias` expects the offending column name (`user_id`) in the message.
- `schema.cpp:1253` — `"Cannot add new field to a user type used in a primary key"` — upstream `testAlterTypeUsedInPartitionKey` expects the keyspace-qualified type name.

### §H2 — Evaluator placeholder returns silently accept invalid queries

- `evaluator.cpp:796-797` — `apply_operator` returns `false` for `Operator::contains`/`contains_key` unconditionally. Downstream: `testFilteringOnListContainingDurations`, `testFilteringOnMapContainingDurations` should raise a "Duration"-family error; plexdb silently returns empty instead.
- ~~Bind-marker numbering~~ (fixed): one walker (`engine.cpp`, `BindWalker`) numbers, types and names every marker in query-text order for both PREPARE metadata and value substitution; markers it cannot type are rejected.

### §H3 — ~~Over-strict "column assigned twice in UPDATE"~~ (fixed)

`planner.cpp`'s duplicate-assignment check now only rejects scalar/counter
columns; collection columns (`m[2] = 7, m[2] = 8`, `s = s + {…}, s = s - {…}`)
are merged with Cassandra's real same-timestamp conflict rule — delete beats
write, and among writes the larger encoded value wins
(`apply_merged_map_patches`/`apply_merged_set_patches`, `engine.cpp`). Fixed
`testList`, `testMultipleOperationOnMapWithinTheSameQuery`,
`testMultipleOperationOnSetWithinTheSameQuery`, and incidentally
`testMapBulkRemoval`.

### §H4 — Permissive parser/decoder: accepts more than Cassandra

Deselected outright (`skiplist.txt`): `testDoubleWith` × 2 (`WITH WITH`).

Still running and failing as `DID NOT RAISE`:
- `testInsertWithUnset`, `testSelectSliceFromComposite`, `testAllowFiltering`, `testMultiSelects`, `testSelectWithToken`, `testSelectDistinct`, `testAllowFilteringOnPartitionKeyWithDistinct`, `testInsertingCollectionsWithInvalidElements` — planner accepts shapes Cassandra rejects.
- `testInvalidInputForList/Set/Map` — the wire decoder doesn't validate a collection's declared length against the bytes available.

Deliberately *not* fixed: a **64 KiB cap on partition/clustering key values**
now exists (`io.evaluator.cpp:evaluated_exceeds_max_key_size`, wired into
INSERT's per-column check and WHERE-clause PK equality — fixes
`testPKInsertWithValueOver64K`, `testCKInsertWithValueOver64K`,
`testPKQueryWithValueOver64K`), but **collection element size is
deliberately left uncapped**. Upstream's own limit here is narrower and
stranger than "reject everything over 64K": scylladb/scylladb#7745 says map
keys and set elements over 64K are rejected *only in unprepared statements*;
list elements and prepared-statement bind values aren't limited at all
(`testListWithElementsBiggerThan64K` has no `xfail` marker upstream — it
must keep passing). Replicating that exact prepared/unprepared distinction
wasn't judged worth the complexity; the three xpass tests
(`testMapsWithElementsBiggerThan64K`, `testSetsWithElementsBiggerThan64K`,
`testCKQueryWithValueOver64K`) reflect that choice.

### §H5 — ~~Parser gaps: rejects shapes Cassandra accepts~~ (fixed)

All three remaining shapes now parse:
- `testSetWithTwoSStables`, `testStaticTable` — `float_literal`'s exponent-detection lookahead (`parsers.cpp`) scanned forward for a bare `e`/`E`/`.` before a terminator instead of checking the immediate next character, so any bare identifier containing one of those letters before whitespace (`properties`, `abcde`, ...) false-triggered the float-literal branch, which then fatally failed instead of backtracking to the identifier fallback. Fixed with an anchored peek (`dsl::sign + digits + '.'|'e'|'E'`).
- `testStaticTable`'s `BEGIN BATCH ... APPLY BATCH` — the `;` between BATCH child statements was mandatory; Cassandra's grammar doesn't require it (INSERT/UPDATE/DELETE are reserved keywords that unambiguously start the next statement). Made optional.
- `testCounterBatch` — was the float-literal bug above; also fixed.

### §H6 — Silent SIGSEGVs: `frozen<map<...>>` as a PK/CK column

`schema.cpp` accepts the column declaration; the clustering-key path derefs
something that isn't there for a map-typed key column. Skiplisted, not
fixed. See the new plan-doc item "Frozen collections" for the properly-scoped
fix (this is one of several related frozen-type gaps, not an isolated bug).
Affected: `testDeletionWithContainsAndContainsKey`,
`testUpdateWithContainsAndContainsKey`, `testContainsFilteringForClusteringKeys`,
`testContainsOnPartitionKey`, `testContainsOnPartitionKeyPart`,
`testOrderByForInClauseWithCollectionElementSelection`.

### §H7 — ~~Native-protocol writer emits a truncated frame~~ (fixed)

`testAlterKeyspaceWithMultipleInstancesOfSameDCThrowsSyntaxException` passes.

### §H8 — UDT-in-column-type wire representation is broken

`testFilteringOnUdtContainingDurations`: the `system_schema.columns` row for
a UDT-typed column still serializes an empty type string on the wire.
Skiplisted alongside §H6.

### §H9 — Cell-writetime, dropped-column, and row-liveness tracking

> **Update:** row markers are tracked (`ROW_FLAG_NO_MARKER` / `ROW_FLAG_MARKER_TTL`), rows with
> neither marker nor regular cells are deleted, expired cells read as absent, and DROP COLUMN
> rewrites rows so dropped cells no longer keep rows alive. Timestamp-based reconciliation and
> tombstones remain unimplemented, so `USING TIMESTAMP` is rejected.

Three related gaps, all rooted in the same missing concept — this engine
doesn't track *why* a row/cell is or isn't visible beyond "does storage have
bytes for it":

- **Dropped-then-re-added column with USING TIMESTAMP returns stale rows.**
  `testDropWithTimestamp`, `testDropStaticWithTimestamp`,
  `testDropMultipleWithTimestamp`. `schema::Column` has no `drop_timestamp`
  field, so read/write paths can't shadow cells against a DROP tombstone.
- **Row liveness after a column drop.** `testAlterOnlyColumnBehaviorWithFlush`:
  a row written by `UPDATE` (setting only column `x`, never `y`) should
  vanish once `x` is dropped — Cassandra rows are only "alive" via an INSERT
  row marker or a live regular-column cell, and this row has neither once
  `x` is gone. plexdb currently returns the row anyway (`x` and `y` both
  read as NULL) because row presence is purely "does the BTree entry exist,"
  with no row-marker concept at all. **Not a one-line fix**: `RowMetadata`
  would need a marker bit set unconditionally by INSERT (not UPDATE), and —
  the hard part — every `UPDATE`-driven `recompute_row_metadata` call
  (`engine.cpp:2689`, `2737`, `3447`, `3488`) would need to preserve a
  pre-existing marker instead of rebuilding `RowMetadata` from scratch, or a
  row created by INSERT then touched by UPDATE would silently lose its
  marker. Left unfixed rather than risk a half-correct implementation.
- **Range-tombstone timestamp reconciliation across flush boundaries.**
  `testReverseQueryWithRangeTombstoneOnMultipleBlocks`: a ranged `DELETE ...
  USING TIMESTAMP T1 WHERE i >= ? AND i <= ?` must correctly shadow only the
  cells with `writetime <= T1` within that range, checked against cells
  written both before *and* after the delete (at higher or lower
  timestamps), and the result must be identical whether or not a flush
  happened in between. This needs real range-tombstone storage with
  timestamp-aware reconciliation — a bigger, dedicated feature, not a spot
  fix.

### §H10 — Wire-encoding stubs that will SIGABRT if reached

26 `assert_not_implemented` sites remain, concentrated in
`native.cpp`/`native.cppm` (hex, varint, decimal, duration, nested-collection,
vector-collection wire arms — 10 sites) plus `engine.cpp` (8),
`schema.cppm` (2), `column_value.cpp`, `evaluator.cpp`, and
`generated/token_wire.cpp` (1 each). The duration arm (`native.cppm:394`) is
the one crash the conformance suite currently hits
(`testFilteringOnDurationColumn`).

### §H11 — `system_schema` stubs

`system_schema.cpp` marks these as `@todo`: materialized views (`:432`),
triggers (`:481`), tracking dropped columns (`:493`, feeds §H9), UDFs
(`:544`), UDAs (`:556`). `CREATE MATERIALIZED VIEW`/`CREATE TRIGGER`/`CREATE
FUNCTION`/`CREATE AGGREGATE` don't have grammar rules at all, so they fail
to parse cleanly (`SyntaxException`, no crash risk) — verified directly, not
just by absence from the grammar source. `testCreatingUDFWithSameNameAsBuiltin_*`
failures are a missing helper in the upstream test-porting shim
(`porting.py`), not this gap; skiplisted (see the top of this doc).

### §H12 — ~~Latent guards around ColumnValue's empty union~~ (centralized, not eliminated)

`drain_columns` and `project_virtual_row` (`engine.cpp`) both used to
independently re-implement "fill absent/never-written slots with `Null{}`
because `visit(ColumnValue)` SIGABRTs on the default-constructed empty
union." Both now call one shared `column_value_or_null(cv, present)`
helper. This is *not* the full fix R2 originally described (making
`ColumnValue{}` itself default to `Null` at the type level) — that would
require either changing `TaggedUnion`'s generic default-index behavior
(affects every other tagged union in the codebase) or turning `ColumnValue`
from a type alias into a wrapper struct (breaks the `get<T>`/`type_matches_tag<T>`/`visit`
call sites that assume it literally *is* a `TaggedUnion`, a change spanning
hundreds of sites). The centralization at least means a third call site adds
one function call instead of re-deriving the invariant.

### §H13 — ~~Schema mutation rollback SIGSEGV~~ (fixed)

`create_table` commits the table's blob row before the column-creation loop
runs, so every `delete_table` rollback call inside that loop operates on an
already-materialised row.

### §H14 — Remaining single-node feature gaps

- **PER PARTITION LIMIT** / **GROUP BY** (`engine.cpp:3033`, `:3030`) — clean `Invalid` rejection, not implemented. `testPerPartitionLimitWithStaticDataAndPaging`, `testIndexOnRegularColumnWithPartitionWithoutRows`.
- **Frozen collection equality on the residual filter** — `testFilteringWithoutIndicesWithFrozenCollections`. See the plan doc's "Frozen collections" item.
- **`testInOrderByWithoutSelecting`** — `c IN (…)` combined with `ORDER BY` on a column outside the projection returns wrong ordering.

---

## Single-node considerations (plexdb intentionally more permissive)

- **LWT (`UPDATE ... IF`, `DELETE ... IF`, `INSERT ... IF NOT EXISTS`) — decided *not* to implement.** Single-replica semantics collapse to "always applies." Cleanly rejected (`"INSERT ... IF NOT EXISTS is not supported"` etc., `engine.cpp`) rather than crashing — standalone LWT used to hit an `assert_true_not_implemented` with nothing upstream of it to catch the case; fixed.
- **Conditional BATCH — same rationale.** Rejected with `"Conditional statements (IF / IF NOT EXISTS) inside BATCH are not supported"`; five tests short-circuit on this before validating their own expected message (see the error-message table above).
- **Consistency levels** — no-op on a single replica; not exercised by the harness (always `ONE`).
- **Write timestamps** — writes apply in arrival order without timestamp reconciliation or tombstones, so explicit `USING TIMESTAMP` is rejected (it would only be honoured when it happens to agree with arrival order). The protocol-level default timestamp that drivers attach to every request is accepted and treated as arrival time when it is within 5 minutes of the server clock, and rejected otherwise.
- **Partitioner ordering** — Murmur3, token-only partition BTree key (`schema.cppm`, `PartitionBTree = FixedKeyPolicy<S64>`); no partition-key bytes stored redundantly in the BTree.
- **`ALLOW FILTERING` gate** — exists (`engine.cpp:1462`) but doesn't fire for several restriction shapes; planner-completeness gap, not a single-node choice. See §H4.
- **64 KiB partition/clustering key value cap** — enforced (see §H4). Collection element size is deliberately not capped; see §H4's note.

---

Multi-node items (standalone LWT, conditional BATCH) are documented in
`TODO.md`. Single-node fixes are enumerated by phase in
[`cql-conformance-plan.md`](cql-conformance-plan.md); each `§Hn` above
cross-references a specific site the plan can target.
