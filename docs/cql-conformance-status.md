## CQL conformance gaps

Baseline: 73 / 313 passing, 40 xfailed, 3 xpassed, 13 skipped (scylladb ref: master, 2026-06-15, after Phase 3).

Phase 3 (2026-06-15): +48 passes (73/313). Delivered: clustering-table DELETE (equality, range,
partial-CK bounds, empty-value bounds); textAsBlob/blobAsText/intAsBlob/bigintAsBlob builtins;
empty-blob crash fix in io.cpp; static-only partition SELECT (no clustering rows, static page
non-null) with PK value injection; SELECT DISTINCT via advance_partition(); SELECT * column
ordering (PK → CK → static → regular).

Phase 3 gaps (not yet delivered):
- Static column writes (rewrite_static) — INSERT/UPDATE to static cols in clustering tables.
- CK prefix equality DELETE on 3-CK tables — `DELETE WHERE k=? AND c1=0` where c1 is the
  first of three CK cols should range-delete all rows with c1=0; currently treated as full
  equality (MissingClusteringKey when c2/c3 are absent).
- Tuple-syntax range DELETE — `WHERE (ck_col) > (?)` sets only filter predicates, not
  locator bounds; DELETE gets MissingClusteringKey.
- USING TIMESTAMP on DELETE — causes crash in static-column deletion tests.

Previous baseline (2026-06-14, after parser fixes): 25 / 313 passing, 41 xfailed, 2 xpassed, 13 skipped.

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

Assert fires per conformance run (post-Phase-3 baseline, 2026-06-15). Phase 3 eliminated
the DELETE-on-clustering-key fires (~172) and SELECT DISTINCT fires (~20).

| Fires | Symptom (assert string) | Site |
|------:|-------------------------|------|
|  88 | Secondary indexes are not implemented | `engine.cpp` |
|  60 | UPDATE on table with clustering key is not implemented | `engine.cpp` |
|  56 | ORDER BY on clustering key requires reverse iterator | `planner.cpp` |
|  44 | SELECT clause type (function/cast/term) is not implemented | `planner.cpp` |
|  40 | BATCH is not implemented | `engine.cpp` |
|  20 | counter column expressions (col = col + n) are not implemented | `planner.cpp` |
|  20 | User-defined types are not implemented (CREATE/DROP TYPE) | `engine.cpp` |
|  12 | subscript/field access in UPDATE SET is not implemented | `planner.cpp` |
|  12 | key serialization for this type is not implemented | `key.cppm` |
|   8 | tuple column type is not implemented | `schema.cpp` |
|   8 | PER PARTITION LIMIT is not implemented | `engine.cpp` |
|   8 | token relations | `planner.cpp` |
|   8 | tuple expression relations | `planner.cpp` |
|   4 | GROUP BY is not implemented | `engine.cpp` |
|   4 | aggregate SELECT (COUNT(*), etc.) is not implemented | `engine.cpp` |

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

- **UPDATE on clustering tables (~60 fires, ~12 unique tests).** DELETE on clustering tables is
  now implemented (Phase 3). UPDATE still asserts. Phase 3 gaps include tuple-syntax range
  UPDATE and CK prefix equality UPDATE (same gaps as DELETE).

- **Static columns on writes (~12 unique tests).** Read path supports static columns (including
  static-only partition SELECT). Write path (INSERT/UPDATE to static cols) not yet implemented;
  rewrite_static deferred to Phase 3b.

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
