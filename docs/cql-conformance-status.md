## CQL conformance gaps

Baseline: 25 / 313 passing, 41 xfailed, 2 xpassed, 13 skipped (scylladb ref: master, 2026-06-14, after parser fixes).

Parser fixes (2026-06-14): +1 pass (testRandomDeletions; 25/313). Assert fire count rose from ~130 to 592 because
eight parser bugs were fixed, converting ~55 previously-silent parse failures into engine asserts. Fixes:
BATCH termination (whitespace before APPLY BATCH), function_selector far-ahead lookahead (→ local peek),
COUNT(1), (col,col) tuple IN/= in WHERE, LIMIT/PER PARTITION LIMIT bind markers, PER PARTITION LIMIT
stored in wrong field, UDT literal {ident: val} parsing. Parse-failure count dropped from ~70 to ~15.

Previous baseline (2026-06-14, after Phase 2): 24 / 313 passing, 41 xfailed, 2 xpassed, 13 skipped.
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

Assert fires per conformance run (including re-fires after server restart). From server log after parser fixes.
Unique affected tests are roughly half the fire count. Fire count is higher than Phase 2 because parser fixes
convert former parse-failures into engine asserts (BATCH, tuple relations, PER PARTITION LIMIT).

| Fires | Symptom (assert string) | Site |
|------:|-------------------------|------|
| 172 | DELETE on table with clustering key is not implemented | `engine.cpp:999` |
|  88 | Secondary indexes are not implemented | `engine.cpp:1139` |
|  60 | UPDATE on table with clustering key is not implemented | `engine.cpp:799` |
|  56 | ORDER BY on clustering key requires reverse iterator | `planner.cpp:234` |
|  44 | SELECT clause type (function/cast/term) is not implemented | `planner.cpp:253` |
|  40 | BATCH is not implemented | `engine.cpp:1151` |
|  20 | SELECT DISTINCT/JSON is not implemented | `engine.cpp:455` |
|  20 | counter column expressions (col = col + n) are not implemented | `planner.cpp:293` |
|  20 | User-defined types are not implemented (CREATE/DROP TYPE) | `engine.cpp:1142,1148` |
|  12 | subscript/field access in UPDATE SET is not implemented | `planner.cpp:276` |
|  12 | key serialization for this type is not implemented | `key.cppm:226` |
|   8 | tuple column type is not implemented | `schema.cpp:45` |
|   8 | PER PARTITION LIMIT is not implemented | `engine.cpp` |
|   8 | token relations | `planner.cpp:141` |
|   8 | tuple expression relations | `planner.cpp` |
|   4 | writing null column values is not implemented | `io.cpp:157` |
|   4 | writing integer value to this dtype is not implemented | `io.cpp:115` |
|   4 | GROUP BY is not implemented | `engine.cpp:456` |
|   4 | aggregate SELECT (COUNT(*), etc.) is not implemented | `engine.cpp:468` |

### Non-crash failures (server returns an error, test still fails)

| Hits | Server `message=` | Notes |
|-----:|-------------------|-------|
| ~15 | `Failed to parse CQL` | Remaining parser gaps: `CREATE TYPE`, `CREATE INDEX`, `INSERT ... USING TTL AND TIMESTAMP`, various `ALTER` shapes. Down from ~70 after parser fixes. |
|  10 | `Insert is missing a value for a clustering key` | Tests inserting with optional clustering columns and unset bind values. |
|   8 | `Insert is missing a value for a primary key` | Same shape for PK columns. |
|   6 | `INSERT USING TTL is not implemented` | Returned as `Invalid` (not a crash). |
|   4 | `Incompatible literal for column type` | smallint/tinyint and reversed type cases. |
|   3 | `USING TIMESTAMP` on ALTER DROP | Tests expecting timestamp-filtered column visibility. |
|   2 | `Table does not exist` | Test exercises error shape on dropped table. |


---

## Design / structural issues to flag

These are the items that block large slabs of tests and need a design decision, not just a code edit.

- **DELETE/UPDATE on clustering tables (~172+60=232 assert fires, ~37 unique tests directly).** Schema layer
  supports clustering BTrees; SELECT already walks them. DELETE/UPDATE only handle the non-clustering path.
  Phase 3 introduces `apply_mutation`, a single read-modify-write entry point for all DML on both path
  variants. The planner already captures CK equality in `RowLocator.ck_begin`; `plan_mutation` needs a
  `MissingClusteringKey` error for clustering tables where ck_is_equality is false.

- **Static columns on writes (~12 unique tests, subsumed by CK crashes).**
  Schema and read path support static columns; the write path does not. Phase 3 adds `rewrite_static`.
  Can be split from the CK mutation work if needed.

- **ORDER BY / CLUSTERING ORDER BY (~56 fires, ~9 unique tests in select_order_by_test.py).**
  Requires reverse iteration in `btree::Iterator` and `RowIterator`. `RowLocator` already has
  `reverse_partitions` and `reverse_clustering` fields; `plan_select` asserts instead of setting them.
  Phase 5 fills these in. Depends on Phase 3 since valid ORDER BY columns are clustering keys.

- **Secondary indexes (~88 fires, ~11 unique tests).** New storage object, planner change, DDL. Phase 4.

- **SELECT COUNT / scalar functions (~44+4=48 fires, ~8 unique tests).** Aggregation stage above
  `RowIterator` plus scalar function registry. Phase 6.

- **Counter columns (~20 fires, ~5 unique tests).** `col = col + n` assignment needs row context at apply
  time. Phase 7 extends `apply_mutation` to resolve column references.

- **Collection subscript / append DML (~12 fires, subscript/field access asserts).** `col[k] = v` and
  `col = col + {…}` require read-modify-write on the collection bytes. Phase 8.

- **BATCH (~40 fires, ~11 unique tests).** Single engine entry point wrapping child statements in one
  transaction. Phase 9. Conditional BATCH (LWT) deferred indefinitely.

- **TTL (~6 "INSERT USING TTL" non-crash errors, ~6 unique tests).** Row blob metadata header required.
  Phase 10.

- **PER PARTITION LIMIT (~8 fires).** Engine needs to enforce per-partition row cap during SELECT iteration.

- **Tuple expression relations (~8 fires).** Planner needs to handle `(c1,c2) IN (...)` / `(c1,c2) = (...)` in `build_row_locator`.

- **Token relations (~8 fires).** Planner needs token-range scan in `build_row_locator`.
