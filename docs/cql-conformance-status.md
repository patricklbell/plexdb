## CQL conformance gaps

Baseline: 24 / 313 passing, 41 xfailed, 2 xpassed, 13 skipped (scylladb ref: master, 2026-06-14, after Phase 1).

Previous baseline (2026-06-13, pre-Phase 1): 23 / 313 passing, 39 xfailed, 1 xpassed.

Phase 1 gains: +1 pass (`testDeleteColumnNoClustering`); column-level DELETE implemented; non-PK WHERE predicates and compound PK equality now handled by planner instead of crashing.

### Unimplemented-CQL aborts

Engine-side asserts that fire when the server is asked to execute a CQL shape it does not yet support. Counts are assert fires per conformance run (including re-fires after server restart); unique affected tests are roughly half the fire count. Sites marked `planner.cpp` were moved there in Phase 1.

| Fires | Symptom (assert string) | Site |
|------:|-------------------------|------|
| 78 | DELETE on table with clustering key is not implemented | `engine.cpp:1018` |
| 44 | Secondary indexes are not implemented | `engine.cpp:1160` |
| 30 | UPDATE on table with clustering key is not implemented | `engine.cpp:807` |
| 28 | ORDER BY on clustering key requires Phase 2 reverse iterator | `planner.cpp:242` |
| 24 | static column storage is not implemented | (various write paths) |
| 18 | SELECT clause type (function/cast/term) is not implemented | `planner.cpp:261` |
| 18 | collection type bind parameters are not implemented | (native/parsers) |
| 17 | composite partition key in standalone PRIMARY KEY | (parser) |
| 16 | frozen types | (parser) |
| 15 | CREATE TABLE WITH options are not implemented | (parser/engine) |
| 14 | non-constant/non-bind UPDATE assignment (counter columns require Phase 6) | `planner.cpp:302` |
| 11 | SELECT DISTINCT/JSON is not implemented | `engine.cpp:465` |
| 10 | User-defined types are not implemented | `engine.cpp:1163-1169` |
|  8 | writing list/vector literal as column value is not implemented | `io.cpp` |
|  8 | subscript/field access in UPDATE SET is not implemented | `planner.cpp:284` |
|  6 | key serialization for this type is not implemented | `key.cppm:226` |
|  6 | INSERT USING TIMESTAMP/TTL is not implemented | (engine) |
|  5 | writing set literal as column value is not implemented | `io.cpp` |
|  4 | tuple column type is not implemented | (parser) |
|  4 | default_time_to_live is not implemented | `engine.cpp:276` |
|  3 | writing null column values is not implemented | `io.cpp:157` |
|  3 | LIMIT is not implemented | (engine) |
|  2 | token relations | `planner.cpp:145` |
|  2 | writing integer value to this dtype is not implemented | `io.cpp:115` |
|  2 | BATCH is not implemented | `engine.cpp:1172` |
|  2 | aggregate SELECT (COUNT, etc.) requires Phase 5 | `engine.cpp:478` |

### Non-crash failures (server returns an error, test still fails)

| Hits | Server `message=` | Notes |
|-----:|-------------------|-------|
| 87 | `Failed to parse CQL` | Parser rejects: `CREATE TYPE`, `CREATE INDEX`, `INSERT ... USING TTL AND TIMESTAMP`, collection literals in `INSERT VALUES`, `SELECT distinct, json FROM ...` (reserved-word column names), various `ALTER` shapes. |
| 10 | `Insert is missing a value for a clustering key` | Triggered by tests inserting with optional clustering columns and unset bind values. |
|  8 | `Insert is missing a value for a primary key` | Same shape as above for PK columns. |
|  4 | `INSERT USING TTL is not implemented` | Returned as `Invalid` (not a crash) — distinct from `default_time_to_live`. |
|  4 | `Incompatible literal for column type` | Tests deliberately exercise narrow-int (smallint/tinyint) and reversed type cases. |
|  3 | `USING TIMESTAMP` on ALTER DROP | Tests that re-add columns and expect timestamp-filtered visibility of old data. |
|  2 | `Table does not exist` | One test issues query against a dropped table to check error shape. |
|  1 each | `Unknown table option`, `Keyspace does not exist`, `Failed to create table` | Tail of long-tail validation cases. |

### Failures by upstream test file (233 total)

| Fails | File |
|------:|------|
| 70 | `select_test.py` |
| 46 | `delete_test.py` |
| 22 | `collections_test.py` |
| 17 | `select_order_by_test.py` |
| 17 | `update_test.py` |
| 15 | `alter_test.py` |
| 12 | `create_test.py` |
| 11 | `batch_test.py` |
|  7 | `counters_test.py` |
|  6 | `timestamp_test.py` |
|  4 | `select_limit_test.py` |
|  3 | `insert_test.py` |
|  2 | `type_test.py` |
|  1 | `truncate_test.py` |

---

## Design / structural issues to flag

These are the items that block large slabs of tests and need a design decision, not just a code edit.

- **DELETE/UPDATE on clustering tables (~54 failures combined, ~108 assert fires).** Schema layer already supports clustering BTrees (`ClusteringBTree` in `cql.engine.schema`), and SELECT already walks them via the two-level `RowIterator`. DELETE/UPDATE only handle the single-level (non-clustering) path. Need clustering-key-aware mutation paths that: (a) resolve the clustering position from the WHERE clause (planner already captures CK equality in `RowLocator.ck_begin`), (b) remove the inner-BTree row blob (DELETE) or rewrite it (UPDATE), and (c) tombstone the outer entry only when the inner BTree becomes empty.

- **ORDER BY / CLUSTERING ORDER BY (~13 failures, ~28 assert fires).** Two options: (i) implement reverse iteration in `btree::Iterator` and plumb it through `RowIterator`, or (ii) buffer the rowset into a scratch arena and sort. (i) is more work but matches Scylla/Cassandra's behavior and avoids unbounded memory for large partitions. The decision interacts with how `PER PARTITION LIMIT` and paging will be implemented.

- **ALLOW FILTERING / non-PK WHERE (Phase 1 planner complete; execution works for non-clustering tables).** The planner now routes non-PK predicates to `FilterPlan.predicates` and sets `needs_allow_filtering`; the engine already passes `filter_predicates` + `filter_ctx` to the native layer which calls `evaluate_where`. For non-clustering tables this is fully functional. Most ALLOW FILTERING test failures are caused by the table having a clustering key (UPDATE/DELETE on clustering tables crash before SELECT can run). Once clustering-key mutations are implemented, these tests should largely pass without additional work.

- **Secondary indexes (~22 affected tests, 44 assert fires).** Need a new storage object (mapping indexed value → partition/clustering key bytes), a planner change to choose between table scan and index lookup, and parser support for `CREATE INDEX`. Touches schema persistence (indexes are catalog entries) and DDL replay on open.

- **Static columns on writes (~12 affected tests, 24 assert fires).** Schema and read-path support exist; write path does not populate the static blob in DELETE/UPDATE shapes. Subsumes the note from the previous iteration — this is now a top-5 blocker by fire count. Natural cohort with the clustering-key mutation work above.

- **SELECT COUNT / scalar functions / TOKEN() (~10 failures, ~4 assert fires for aggregate path).** Requires (a) a result-aggregator stage that runs after `RowIterator` and (b) a scalar-function registry for the simple cases (`token`, `writetime`, `ttl`, type casts, `blobAs*` / `*AsBlob`). The aggregator probably wants the same shape as the prospective ORDER BY buffer.

- **Counter columns (~7 failures, 14 assert fires).** Today `UPDATE SET col = <constant|bind>` is the only supported RHS. Counters need `col = col + n` parsing (already a relation/expression in the AST) plus a counter-aware write path that performs read-modify-write inside the engine transaction.

- **Collection DML (~22 failures in `collections_test.py`, 18+ assert fires).** Two sub-issues: (i) collection literals in `INSERT VALUES` (parser + io write path); (ii) subscript / element update `col[k] = v` and `col = col + {…}` (`planner.cpp:284` asserts). The latter needs in-place modification of the collection bytes inside the row blob. Decide whether collections live in the row blob or in a side-table.

- **BATCH statements (~11 failures, 2 assert fires).** One assert site (`engine.cpp:1172`). Needs an engine-level entry point that begins a single transaction, dispatches each child statement, and commits. Probably also needs idempotency-vs-conditional semantics. Small surface area, but touches the transaction-borrowing pattern noted in memory.

- **TTL / `default_time_to_live` (~6 failures).** Requires expiration metadata per cell (or per row) plus a tombstone-expiry sweep. This is a row-blob layout change; defer until row encoding is otherwise being touched (collection DML is a natural cohort).

- **`CREATE TABLE` parser coverage (~12 `create_test.py` fails, 17+ assert fires for composite PK).** Many tests are blocked at *schema setup* because the parser rejects the `CREATE TABLE` shape (composite PK declarations, `WITH compaction = ...` options, static-column declarations in some forms). These count as one test each in `create_test.py` but cascade into the `select_test.py` / `update_test.py` failure counts because their fixtures rely on the same shapes.

- **`CREATE TABLE` / `ALTER TABLE` WITH options (~15 + 2 assert fires).** Parser accepts `WITH` options but the engine asserts on unrecognised keys. Silently ignoring unknown table/keyspace options (as Cassandra does) would unblock many setup sequences.
