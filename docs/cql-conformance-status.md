## CQL conformance gaps

Baseline: 24 / 313 passing, 41 xfailed, 2 xpassed, 13 skipped (scylladb ref: master, 2026-06-14, after Phase 2).

Phase 2 (2026-06-14): 0 new passes (still 24/313). Assert fire count reduced from ~290 to ~130 per run.
Phase 2 eliminated: WITH-option crashes (converted to log+ignore), default_time_to_live crash (silently skipped),
collection literal write crashes in UPDATE (plan_update and engine UPDATE now call evaluate() directly), and
several stale entries (composite PK and frozen types were already handled by the parser). Tests that Phase 2
was expected to unblock are also blocked by clustering-key mutations — Phase 3 is the actual unlock.

Previous baseline (2026-06-14, after Phase 1): 24 / 313 passing, 41 xfailed, 2 xpassed, 13 skipped.
Phase 1 gains: +1 pass (testDeleteColumnNoClustering); planner module; column-level DELETE; non-PK WHERE →
RequiresAllowFiltering; compound PK equality; LIMIT implemented.

Previous baseline (2026-06-13, pre-Phase 1): 23 / 313 passing, 39 xfailed, 1 xpassed.

### Unimplemented-CQL aborts

Assert fires per conformance run (including re-fires after server restart). From server log after Phase 2.
Unique affected tests are roughly half the fire count.

| Fires | Symptom (assert string) | Site |
|------:|-------------------------|------|
| 39 | DELETE on table with clustering key is not implemented | `engine.cpp:999` |
| 22 | Secondary indexes are not implemented | `engine.cpp:1139` |
| 15 | UPDATE on table with clustering key is not implemented | `engine.cpp:799` |
| 14 | ORDER BY on clustering key requires reverse iterator | `planner.cpp:234` |
| 11 | SELECT clause type (function/cast/term) is not implemented | `planner.cpp:253` |
|  5 | SELECT DISTINCT/JSON is not implemented | `engine.cpp:455` |
|  5 | counter column expressions (col = col + n) are not implemented | `planner.cpp:293` |
|  5 | User-defined types are not implemented (CREATE/DROP TYPE) | `engine.cpp:1142,1148` |
|  3 | subscript/field access in UPDATE SET is not implemented | `planner.cpp:276` |
|  3 | key serialization for this type is not implemented | `key.cppm:226` |
|  2 | tuple column type is not implemented | `schema.cpp:45` |
|  1 | writing null column values is not implemented | `io.cpp:157` |
|  1 | writing integer value to this dtype is not implemented | `io.cpp:115` |
|  1 | token relations | `planner.cpp:141` |
|  1 | GROUP BY is not implemented | `engine.cpp:456` |
|  1 | BATCH is not implemented | `engine.cpp:1151` |
|  1 | aggregate SELECT (COUNT(*), etc.) is not implemented | `engine.cpp:468` |

### Non-crash failures (server returns an error, test still fails)

| Hits | Server `message=` | Notes |
|-----:|-------------------|-------|
| ~70 | `Failed to parse CQL` | Parser rejects: `CREATE TYPE`, `CREATE INDEX`, `INSERT ... USING TTL AND TIMESTAMP`, `SELECT distinct, json FROM ...` (reserved-word column names), various `ALTER` shapes. Collection literal `INSERT VALUES` now parses correctly. |
|  10 | `Insert is missing a value for a clustering key` | Tests inserting with optional clustering columns and unset bind values. |
|   8 | `Insert is missing a value for a primary key` | Same shape for PK columns. |
|   4 | `INSERT USING TTL is not implemented` | Returned as `Invalid` (not a crash). |
|   4 | `Incompatible literal for column type` | smallint/tinyint and reversed type cases. |
|   3 | `USING TIMESTAMP` on ALTER DROP | Tests expecting timestamp-filtered column visibility. |
|   2 | `Table does not exist` | Test exercises error shape on dropped table. |


---

## Design / structural issues to flag

These are the items that block large slabs of tests and need a design decision, not just a code edit.

- **DELETE/UPDATE on clustering tables (~39+15=54 assert fires, ~37 unique tests directly).** Schema layer
  supports clustering BTrees; SELECT already walks them. DELETE/UPDATE only handle the non-clustering path.
  Phase 3 introduces `apply_mutation`, a single read-modify-write entry point for all DML on both path
  variants. The planner already captures CK equality in `RowLocator.ck_begin`; `plan_mutation` needs a
  `MissingClusteringKey` error for clustering tables where ck_is_equality is false.

- **Static columns on writes (~12 unique tests, ~24 fires in pre-Phase-2 data; now subsumed by CK crashes).**
  Schema and read path support static columns; the write path does not. Phase 3 adds `rewrite_static`.
  Can be split from the CK mutation work if needed.

- **ORDER BY / CLUSTERING ORDER BY (~14 fires, ~9 unique tests in select_order_by_test.py).**
  Requires reverse iteration in `btree::Iterator` and `RowIterator`. `RowLocator` already has
  `reverse_partitions` and `reverse_clustering` fields; `plan_select` asserts instead of setting them.
  Phase 5 fills these in. Depends on Phase 3 since valid ORDER BY columns are clustering keys.

- **Secondary indexes (~22 fires, ~11 unique tests).** New storage object, planner change, DDL. Phase 4.

- **SELECT COUNT / scalar functions (~11+1=12 fires, ~8 unique tests).** Aggregation stage above
  `RowIterator` plus scalar function registry. Phase 6.

- **Counter columns (~5 fires, ~5 unique tests).** `col = col + n` assignment needs row context at apply
  time. Phase 7 extends `apply_mutation` to resolve column references.

- **Collection subscript / append DML (~3 fires, subscript/field access asserts).** `col[k] = v` and
  `col = col + {…}` require read-modify-write on the collection bytes. Phase 8.

- **BATCH (~1 fire, ~11 unique tests).** Single engine entry point wrapping child statements in one
  transaction. Phase 9. Conditional BATCH (LWT) deferred indefinitely.

- **TTL (~4 "INSERT USING TTL" non-crash errors, ~6 unique tests).** Row blob metadata header required.
  Phase 10.
