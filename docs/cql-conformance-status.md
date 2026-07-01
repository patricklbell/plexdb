## CQL conformance gaps

Baseline: **194 / 313 passing**, 62 failed, 35 xfailed, 8 xpassed, 15 skipped
(scylladb ref: master, 2026-07-01, after Murmur3 partitioner ordering, UDT
storage, and the sorting-library refactor). Two `testDoubleWith` shapes are
deselected in `tools/cql_tests/conftest.py::skiplist.txt` — permissive-parser
gap (see hack §H4).

The 62 failures partition by primary symptom into **10 server crashes** (5
observed aborts, 5 silent SIGSEGVs — the assert message is missing on the
segfault paths), **17 non-crash server errors** (server returned an error but
the test's assertion rejected the *content*), and **35 test-side check
failures** (server ran to completion but the test's own row/count/exception
assertion tripped).

### Server crashes (10 unique tests)

Observed aborts (from `server.log` under `-L`):

| Fires | Symptom (assert string) | Site |
|------:|-------------------------|------|
|   1 | `writing integer value to this dtype is not implemented` | `io.cpp:117` |
|   2 | `PER PARTITION LIMIT is not implemented` | `engine.cpp:2883` |
|   2 | `GROUP BY is not implemented` | `engine.cpp:2882` |

Silent SIGSEGVs (server-process died without printing an assert — the
`not_implemented` guard is missing on these paths, so a nullptr or malformed
buffer flows through):

| Test | Suspected trigger |
|------|-------------------|
| `testAlterKeyspaceWithMultipleInstancesOfSameDCThrowsSyntaxException` | Native writer emits a truncated frame after rejecting duplicate map keys — driver crashes with `struct.error: unpack requires a buffer of 2 bytes` (see hack §H7). |
| `testDeletionWithContainsAndContainsKey` | `b frozen<map<int,int>>` as clustering key. |
| `testOrderByForInClauseWithCollectionElementSelection` | `SELECT v[k] ... WHERE pk IN (...) ORDER BY c`. |
| `testContainsFilteringForClusteringKeys`, `testContainsOnPartitionKey`, `testContainsOnPartitionKeyPart` | `frozen<map<int,int>>` as partition key (`CONTAINS KEY` on PK). |
| `testUpdateWithContainsAndContainsKey` | Same `frozen<map>` PK/CK shape from the UPDATE side. |
| `testFilteringOnUdtContainingDurations` | UDT-typed column wire encoding: `system_schema.columns` returns an empty type string, driver decodes `''` and dies with `ValueError: Don't know how to parse type string ''` (see hack §H8). |

### Non-crash server errors (17 hits, 15 unique tests — some tests trip both an error and a content assertion)

| Hits | Server `message=` | Notes |
|-----:|-------------------|-------|
|   6 | `error: while parsing parsers::grammar::<rule> at 1:N: <detail>` / `Failed to parse CQL` | Parser gaps + rule-name leakage. Concrete shapes that fail today: `c IN (integer_literal)` (`testClusteringOrder`, `testAllowFilteringOnPartitionKey`), integer overflow in `token(large_int)` (`testTokenRange`), a decimal shape in `testCounterBatch`, a `USING TIMESTAMP` shape in `testBatchUpdate`, `BEGIN BATCH ... APPLY BATCH` in `testStaticTable`, and a UPDATE shape in `testSetWithTwoSStables`. Also see hack §H1 — the leaked rule name breaks upstream regex checks. |
|   3 | `Conditional statements (IF / IF NOT EXISTS) inside BATCH are not supported` | Correct rejection; short-circuits before `testInvalidCustomTimestamp[tablets/vnodes]` and `testBatchWithInRestriction` can validate their expected messages. Multi-node feature. |
|   3 | `Column X is assigned twice in UPDATE` | Over-strict — Cassandra collapses `col = col - {…}, col = col + {…}` shape (`testList`, `testMultipleOperationOnMapWithinTheSameQuery`, `testMultipleOperationOnSetWithinTheSameQuery`). See hack §H3. |
|   2 | `Incompatible literal for column type` | `testDateCompatibility` (date/time literal coercion), `testFilteringOnCollectionsWithNull` (NULL element inside collection literal). |
|   1 | `Cannot execute this query as it might involve data filtering ... use ALLOW FILTERING` | `testAlias` — generic; upstream regex looks for the column name `user_id` in the message. |
|   1 | `Cannot apply counter operations on non-counter column a` | `testAdderNonCounter` — regex mismatch; upstream wants `Invalid operation (a = a + 1) for non counter column a`. See hack §H1. |
|   1 | `Cannot add new field to a user type used in a primary key` | `testAlterTypeUsedInPartitionKey` — regex expects `keyspace.type_name` prefix. See hack §H1. |
|   1 | `Order by is currently only supported on the clustered columns of the PRIMARY KEY, got ` | `testAllowFilteringOnPartitionKeyWithCounters` — literal `got ` with no context (see hack §H1); upstream also wants a different message text entirely. |

### Test-side check failures (35 tests — server returned success, test's own assertion tripped)

| Tests | Category | Notes |
|------:|----------|-------|
|  18 | `AssertionError` — wrong rows / count / order | `testSet`, `testMap`, `testBatch`, `testEmptyRestrictionValue{,WithMultipleClusteringColumns,WithOrderBy,WithMultipleClusteringColumnsAndOrderBy}`, `testOrderByForInClauseWithNullValue`, `testInOrderByWithoutSelecting`, `testDrop{,Static,Multiple}WithTimestamp`, `testFilteringOnDurationColumn`, `testFilteringOnTupleContainingDurations`, `testFilteringWithoutIndicesWithFrozenCollections`, `testAlterOnlyColumnBehaviorWithFlush`, `testReverseQueryWithRangeTombstoneOnMultipleBlocks`, `testSelectWithAlias` (writetime returned non-zero for a column the test says should have writetime 0). |
|  10 | `Failed: DID NOT RAISE InvalidRequest` | Planner accepts what upstream rejects: `testInsertingCollectionsWithInvalidElements` (tuple/set element type mismatch), `testInsertWithUnset`, `testSelectSliceFromComposite`, `testAllowFiltering`, `testMultiSelects`, `testSelectWithToken`, `testSelectDistinct`, `testAllowFilteringOnPartitionKeyWithDistinct`, `testFilteringOnListContainingDurations`, `testFilteringOnMapContainingDurations` (see hack §H2). |
|   4 | `AssertionError: Regex pattern did not match` | Bundled into the non-crash-error table above — server rejected correctly but with a different message than upstream. |
|   1 | `ValueError: Too many arguments provided to bind()` | `testFieldSelectionOrderSingleClustering` — prepared-statement metadata reports 2 columns but the test binds 3. |
|   1 | `TypeError: object of type 'int' has no len()` | `testAllowSkippingEqualityAndSingleValueInRestrictedClusteringColumns` — result metadata says List<Int32> for a column the test hands an int; either the metadata or the projection is wrong. |
|   1 | `TypeError: Got a non-map object for a map value` | `testMapBulkRemoval` — server serialized a set on the wire where the map type descriptor claims a map. |

### Unexpected passes (8 — upstream marks `@pytest.mark.xfail`, plexdb passes)

These are cases the ScyllaDB test suite tags as expected-to-fail for its own
implementation; plexdb happens to satisfy them today. Two categories:

- **No 64 KiB element / key limit**: `testMapsWithElementsBiggerThan64K`,
  `testSetsWithElementsBiggerThan64K`, `testCKQueryWithValueOver64K` — plexdb's
  key and cell encodings do not enforce upstream's 64 KiB cap. Whether this is
  a bug or an intentional relaxation is undecided; note that upstream tests
  *require* the failure, so any future clamp will need to xpass these.
- **Genuine features that landed**: `testInsertWithTtl` (TTL support),
  `testSelectDistinctWithWhereClauseOnStaticColumn`,
  `testFilteringWithMultiColumnSlices`, `testFilteringWithOrderClause`,
  `testCreateKeyspaceWithSimpleStrategyNoOptions` (accepted without an explicit
  `replication_factor`).

---

## Current implementation hacks being hit

These are places where the code takes a shortcut that a specific failing test
exercises. Each entry names the site and the tests it affects.

### §H1 — Error-message strings drift from Cassandra's format

The engine wires literal strings that upstream regex-matches against. Any
drift trips a `Regex pattern did not match` even when the rejection itself is
correct.

- `engine.cpp:1477` — `"Cannot apply counter operations on non-counter column X"` vs. upstream `"Invalid operation (X = X + 1) for non counter column X"`. Affects `testAdderNonCounter`.
- `engine.cpp:1426` — `"Order by is currently only supported on the clustered columns of the PRIMARY KEY, got " + result.context`, and `result.context` is empty on the code path that runs (`testAllowFilteringOnPartitionKeyWithCounters`). Two problems: empty context, and the whole string is not the message upstream expects for this case.
- `engine.cpp:1384` — `"Cannot execute this query as it might involve data filtering..."` is generic; upstream test `testAlias` expects the offending column name (`user_id`) in the message.
- `schema.cpp:1251` — `"Cannot add new field to a user type used in a primary key"` — upstream `testAlterTypeUsedInPartitionKey` expects the keyspace-qualified type name (e.g. `cqltest1234.mytype`).
- **Parser rule-name leakage.** `parsers.cpp` errors surface as `"error: while parsing parsers::grammar::primary_term at 1:87: exhausted choice"` — the internal rule names are exposed to the client. Upstream tests that match on Cassandra-flavoured error text (`SyntaxException` regex) can never pass with these strings.

### §H2 — Evaluator placeholder returns silently accept invalid queries

The WHERE-clause evaluator has stub arms that fall through to `false` (which
means "row does not match") instead of raising. When the planner routes an
invalid restriction through the residual filter, the query succeeds with zero
rows rather than being rejected.

- `evaluator.cpp:775` — `apply_operator` returns `false` for `Operator::contains` and `Operator::contains_key` unconditionally. Downstream: `testFilteringOnListContainingDurations`, `testFilteringOnMapContainingDurations` (should raise "Duration"-family errors — plexdb silently returns empty). Interacts with duration-column filtering (`testFilteringOnDurationColumn`, `testFilteringOnTupleContainingDurations`) since Cassandra outright forbids equality on `duration` and plexdb doesn't.
- `evaluator.cpp:1067` — `number_bind_markers_in_term` has `@todo Update, Delete, Select WHERE clause`. Bind-marker numbering is only complete for INSERT. Direct fallout: `testFieldSelectionOrderSingleClustering` (`ValueError: Too many arguments provided to bind()`), and the last remaining `bind()`-mismatch cases in `select_test.py` large-collection paths.
- `evaluator.cpp:1078` — `to_str(Evaluated, type::Basic)` returns the literal `"@todo"_as` when the input is not a `Constant`. Any diagnostic surface that renders a non-constant `Evaluated` will read `"@todo"` (grep the client output on failing tests for `@todo` to spot this).
- `engine.cpp:1990` — `assert_not_implemented("CountStar reached project_row_via_ops; aggregate path should handle it")`. If a query is ever routed to `project_row_via_ops` with a `COUNT(*)` selector, this SIGABRTs. Not currently hit but tagged as a hack because it converts a planner bug into a crash rather than an error.

### §H3 — Over-strict "column assigned twice in UPDATE"

`engine.cpp:1452` raises `"Column X is assigned twice in UPDATE"` any time
the same column name appears more than once in the SET list. Cassandra
collapses valid same-column combinations like `m = m - {…}, m = m + {…}`
(delete-then-append on a collection). Affects `testList`,
`testMultipleOperationOnMapWithinTheSameQuery`,
`testMultipleOperationOnSetWithinTheSameQuery`.

### §H4 — Permissive parser: accepts more than Cassandra

Two shapes are deselected from the suite (`skiplist.txt`) rather than fixed:
- `testDoubleWith` × 2 — `WITH WITH` is silently accepted on `CREATE`/`ALTER KEYSPACE`.

The following also-permissive shapes still run and show up as
`DID NOT RAISE` failures:
- `testInsertWithUnset` — plexdb accepts `unset()` where Cassandra rejects.
- `testSelectSliceFromComposite` — accepts contradictory clustering restrictions (`ctime=X AND ctime<=Y`).
- `testAllowFiltering` — accepts queries that Cassandra flags with the ALLOW FILTERING gate; the gate lives in `engine.cpp:1384` but does not fire for these shapes.
- `testMultiSelects` — accepts an equality on a non-key non-indexed column without ALLOW FILTERING.
- `testSelectWithToken` — accepts `token(int(...))` where Cassandra rejects (`int` is not a valid function name).
- `testSelectDistinct`, `testAllowFilteringOnPartitionKeyWithDistinct` — accepts `SELECT DISTINCT` on a subset of the partition key.
- `testInsertingCollectionsWithInvalidElements` — accepts a tuple value whose element types don't match the declared `frozen<tuple<int, text, double>>`.

### §H5 — Parser gaps: rejects shapes Cassandra accepts

Six tests get `Failed to parse CQL` or `exhausted choice` from `parsers.cpp`
grammar (`testSetWithTwoSStables`, `testStaticTable`, `testCounterBatch`,
`testBatchUpdate`, `testTokenRange`, `testClusteringOrder`,
`testAllowFilteringOnPartitionKey`). Concrete shapes flagged:
- `c IN (integer_literal)` — parser fails inside `IN(...)` when the RHS is a bare integer literal (`primary_term at 1:87` / `1:73`).
- `token(<large_int>)` — `integer_literal at 1:76: integer overflow` for the token-range boundary constants (`testTokenRange`).
- Float/decimal literal shape at column 316 in `testCounterBatch`.
- `USING TIMESTAMP` shape at column 458 in `testBatchUpdate`.
- `BEGIN BATCH ... APPLY BATCH` in `testStaticTable` (batch grammar path).
- A UPDATE shape in `testSetWithTwoSStables`.

Distinct from §H1: even where these tests carry a regex, the failure fires
before the message is produced.

### §H6 — Silent SIGSEGVs from missing `not_implemented` guards

Six tests kill the server without printing an assert message. Common shape:
`frozen<map<int, int>>` used as PK or CK column type. `schema.cpp` accepts
the column declaration, `key.cppm` reaches the collection branch and derefs
something that isn't there. Because there is no explicit
`assert_not_implemented`, the conformance harness sees "server crashed" but
`server.log` is silent. This is worse than an abort — the crash is
undiagnosable from the log alone.

Affected tests: `testDeletionWithContainsAndContainsKey`,
`testContainsFilteringForClusteringKeys`, `testContainsOnPartitionKey`,
`testContainsOnPartitionKeyPart`, `testUpdateWithContainsAndContainsKey`,
`testOrderByForInClauseWithCollectionElementSelection`.

### §H7 — Native-protocol writer emits a truncated frame

`testAlterKeyspaceWithMultipleInstancesOfSameDCThrowsSyntaxException`:
- CREATE KEYSPACE with a `NetworkTopologyStrategy` map succeeds.
- ALTER KEYSPACE with duplicate DC keys in the replication map should return `SyntaxException`.
- Instead, the driver aborts frame parsing with `struct.error: unpack requires a buffer of 2 bytes`.

The check at `schema.cpp:420` returns a `SyntaxOptions` error correctly, so
the bug is on the wire-encoding side: the error/response frame written for
this specific rejection path is missing its trailing 2 bytes. No CQL assert
fires; the client sees a dead connection and the harness records a crash.

### §H8 — UDT-in-column-type wire representation is broken

`testFilteringOnUdtContainingDurations`:
```
CREATE TYPE oudt (i int, d duration);
CREATE TABLE t (k int PRIMARY KEY, u oudt);
```
The `system_schema.columns` row for column `u` serializes an empty type
string on the wire. The cassandra-driver's `lookup_casstype('')` throws
`IndexError: list index out of range`, marks the host down, and the harness
records a crash. Concrete gap: UDT column types are stored (per the recent
UDT commit) but the projection into the driver-visible type-name string is
missing.

### §H9 — Dropped-then-re-added column with USING TIMESTAMP returns stale rows

`testDropWithTimestamp`, `testDropStaticWithTimestamp`,
`testDropMultipleWithTimestamp`: DROP a column at ts=T1, then INSERT with
USING TIMESTAMP T2 into the same table (T2 > T1). Cassandra's per-cell
writetime tracking against the dropped-column tombstone ts=T1 decides
whether the new cell survives. plexdb's `AssertionError` output shows the
new rows are missing or overwritten with the dropped state — the
dropped-column tombstone / per-cell writetime interaction is not honored.

### §H10 — Wire-encoding stubs that will SIGABRT if reached

Not fired by the current suite, but each is a live `assert_not_implemented`
in the encoder — anything landing on it kills the server without a
diagnosable message:
- `native.cppm:180` — hex type native encoding.
- `native.cppm:236/240/244/247` — varint, decimal, duration, nested-collection-or-null-element encoding (native protocol response path).
- `native.cppm:390/392/394/396` — same list on the row-metadata path.
- `native.cpp:153` — CQL value decoding for a basic-type slot that hasn't been wired.
- `native.cpp:251` / `native.cpp:493` — vector collection type bind / encoding.

Duration filtering (§H2) is the closest active surface to these — several
duration tests currently pass through the evaluator without hitting the
encoder, but any change that routes a duration value through the response
path will land here.

### §H11 — `system_schema` stubs

`system_schema.cpp` marks these as `@todo`: materialized views (`:470`),
triggers (`:527`), tracking dropped columns (`:544`, related to §H9), UDFs
(`:605`), UDAs (`:624`). The driver-side handshake is satisfied by the empty
tables, but any test that queries them for content will get zero rows.

### §H12 — Latent guards around ColumnValue's empty union

`drain_columns` (row projection) and `project_virtual_row` (virtual-table
projection) both fill trailing/absent slots with `Null{}` because the native
serializer's `visit(ColumnValue)` SIGABRTs on a default-constructed empty
TaggedUnion. The invariant is enforced *twice* at the call site rather than
once at the type — any third code path that produces a `ColumnValue` without
picking a variant will crash the server. Not currently fired, but every new
projection path is one bug away from re-introducing the crash.

### §H13 — Schema mutation rollback SIGSEGV

`schema.cpp:958-971` (`delete_table`) writes a tombstone byte into
`schema.tables_blob` at `offset_in_blob_bytes + offsetof(tombstone)`. If
`create_table` failed *before* the blob was resized (e.g. `create_column`
raised on a late-detected duplicate), `offset_in_blob_bytes` points past the
end of the blob and the write SIGSEGVs. Presently masked by hoisting
duplicate-name detection into `engine.cpp:2524` so `delete_table` is only
reached on fully-materialized tables, but every non-hoisted failure path in
`create_table` still trips it.

### §H14 — Remaining single-node feature gaps

Not covered elsewhere in this doc — these are `not_implemented` guards fired
by exactly one or two tests each:
- **PER PARTITION LIMIT** (`engine.cpp:2883`) — `testPerPartitionLimitWithStaticDataAndPaging` and one other. Engine row-iteration cap; must integrate with `RowIterator` partition transitions.
- **GROUP BY** (`engine.cpp:2882`) — `testIndexOnRegularColumnWithPartitionWithoutRows` and one other. Projection aggregation grouped by a clustering-key prefix.
- **Frozen collection equality on the residual filter** — `testFilteringWithoutIndicesWithFrozenCollections`. Engine returns wrong rows because the WHERE evaluator does not equality-compare frozen collection columns; distinct from §H2 which is about CONTAINS.
- **`testInOrderByWithoutSelecting`** — `c2 IN (…)` combined with `ORDER BY` when the ordered column is not in the projection returns wrong ordering.
- **`testMapBulkRemoval`** — response encoder emits a set-shape where the column descriptor claims a map; symptom of type-descriptor / value-encoder divergence (see §H1/plan R1).

---

## Single-node considerations (plexdb intentionally more permissive)

- **LWT (`UPDATE ... IF`, `DELETE ... IF`, `INSERT ... IF NOT EXISTS`) — decided *not* to implement.** Requires Paxos consensus across replicas. On a single replica the semantics collapse to "always applies", so there's nothing meaningful to enforce. Engine returns `Invalid` cleanly (`engine.cpp:3339`, `3397`, `3099`). Some upstream tests assume LWT and can never pass here.
- **Conditional BATCH (child `IF` / `IF NOT EXISTS`) — same rationale.** Engine rejects with `"Conditional statements (IF / IF NOT EXISTS) inside BATCH are not supported"`; three tests (`testInvalidCustomTimestamp` × 2, `testBatchWithInRestriction`) short-circuit on this before they can validate their expected error.
- **Consistency levels** — no-op on a single replica. The driver still sends `ONE` in the harness, no test currently exercises `LOCAL_QUORUM` etc.
- **Partitioner ordering** — Murmur3 is implemented (`key.cppm:652`); the previous doc's note about raw-byte order is stale as of the sorting-library refactor.
- **`ALLOW FILTERING` gate** — the gate exists (`engine.cpp:1384`) but does not fire for several restriction shapes; that's a planner-completeness gap, not a single-node choice. See §H4.
- **64 KiB element / key size cap** — plexdb does not enforce Cassandra's cap. The three `over64K` xpasses (`testCK…`, `testMaps…`, `testSets…`) may be a bug or a deliberate relaxation; the choice is not documented in the code today.

---

Multi-node items are documented in `TODO.md` under "Conditional BATCH and
standalone LWT". Single-node fixes are enumerated by phase in
[`cql-conformance-plan.md`](cql-conformance-plan.md); each `§Hn` above cross-
references a specific site the plan can target.
