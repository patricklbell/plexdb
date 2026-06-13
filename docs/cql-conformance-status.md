## CQL conformance gaps

Baseline: 23 / 313 passing, 39 xfailed, 1 xpassed (scylladb ref: master, 2026-06-13).

### Unimplemented-CQL aborts

Engine-side asserts that fire when the server is asked to execute a CQL shape it does not yet support. Each line below is one *call site* and one bucket of test cases that depends on it.

| Hits | Symptom (engine assert string) | Site |
|-----:|--------------------------------|------|
| 38 | DELETE on table with clustering key is not implemented | `engine.cpp:928` |
| 23 | non-PK column expression relations are not implemented | `engine.cpp:438` |
| 16 | UPDATE on table with clustering key is not implemented | `engine.cpp:739` |
| 13 | ORDER BY is not implemented | `engine.cpp:389` |
| 10 | SELECT clause type (count/function/cast/term) is not implemented | `engine.cpp:467` |
| 10 | column-level DELETE is not implemented | `engine.cpp:917` |
|  6 | non-constant/non-bind UPDATE assignment is not implemented | `engine.cpp:799,876` |
|  4 | SELECT DISTINCT/JSON is not implemented | `engine.cpp:386` |
|  4 | key serialization for this type is not implemented | `key.cppm:185` |
|  3 | subscript/field access in UPDATE SET is not implemented | `engine.cpp:792,868` |
|  3 | default_time_to_live is not implemented | `engine.cpp:217` |
|  2 | writing null column values is not implemented | `io.cpp:129` |
|  1 | writing integer value to this dtype is not implemented | `io.cpp:93` |
|  1 | token relations are not implemented | `engine.cpp:443` |
|  1 | column expression relation operator not implemented for PK | `engine.cpp:434` |
|  1 | BATCH is not implemented | `engine.cpp:1008` |

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

### Failures by upstream test file (260 total)

| Fails | File |
|------:|------|
| 76 | `select_test.py` |
| 47 | `delete_test.py` |
| 26 | `collections_test.py` |
| 17 | `update_test.py` |
| 17 | `select_order_by_test.py` |
| 17 | `alter_test.py` |
| 13 | `batch_test.py` |
| 12 | `create_test.py` |
|  9 | `insert_test.py` |
|  8 | `timestamp_test.py` |
|  7 | `counters_test.py` |
|  4 | `type_test.py` |
|  4 | `select_limit_test.py` |
|  1 each | `use_test.py`, `truncate_test.py`, `drop_test.py` |

---

## Smallest fixes (work-now todo list)

Ordered by effort:offset ratio. Items at the top are mechanical or single-call-site; items further down require new code paths but no architectural rethink. The assert-firing items above are *not* candidates here — they are by-design contract guards and need the underlying feature implemented (see Design section).

1. **Combined `USING TTL ... AND TIMESTAMP ...` in INSERT/UPDATE.** Standalone `USING TTL` already parses; tests use the combined form. Parser-only change in `parsers.cpp`; engine can keep returning `Invalid` for TTL until support lands.
2. **`CREATE INDEX` parser stub.** Parse the statement and have the engine return `Invalid: "secondary indexes are not implemented"`. ~15 tests stop hitting the parser and start hitting a deterministic engine error.
3. **`CREATE TYPE` parser stub.** Same pattern as `CREATE INDEX`. Affects `type_test.py` and several `alter_test.py` cases that prelude with a UDT.
4. **Collection literals in `INSERT ... VALUES (...)`.** The parser already handles map/list/set literal *expressions* for `UPDATE SET`; extend the values-list rule to accept the same expression production. Engine already has `write_typed_collection_as_column_value` paths.
5. **`SELECT distinct FROM ...` / `SELECT json FROM ...` where `distinct`/`json` are column names.** The current rule `dsl::opt(p<json> | p<distinct>)` is greedy; backtrack when the next token is not whitespace+identifier. Pure parser fix.
6. **`assert_invalid_syntax_message` tests (`testUseStatementWithBindVariable` etc.) want a specific error string.** Generic "Failed to parse CQL" is rejected by the test regex. Adding a single targeted message for `USE ?` (and a handful of similar `assert_invalid_syntax_message` callers) is cheap; the parser can match the bad shape explicitly and emit the expected text. Worth ~5 tests.
7. **`testNonExistingOnes` (drop_test.py, type_test.py).** Each is a single-statement test against missing entities. Likely already returns the right error code but with the wrong message format; cheap message tweak.

## Design / structural issues to flag

These are the items that block large slabs of tests and need a design decision, not just a code edit.

- **DELETE/UPDATE on clustering tables (~54 failures combined).** Schema layer already supports clustering BTrees (`ClusteringBTree` in `cql.engine.schema`), and SELECT already walks them via the two-level `RowIterator`. DELETE/UPDATE only handle the single-level (non-clustering) path. Need clustering-key-aware mutation paths that: (a) resolve the clustering position from the WHERE clause, (b) remove the inner-BTree row blob (DELETE) or rewrite it (UPDATE), and (c) tombstone the outer entry only when the inner BTree becomes empty.

- **ORDER BY / CLUSTERING ORDER BY (~13 failures, plus blocks reverse-iteration semantics).** Two options: (i) implement reverse iteration in `btree::Iterator` and plumb it through `RowIterator`, or (ii) buffer the rowset into a scratch arena and sort. (i) is more work but matches Scylla/Cassandra's behavior and avoids unbounded memory for large partitions. The decision interacts with how `PER PARTITION LIMIT` and paging will be implemented.

- **ALLOW FILTERING / non-PK WHERE (~23 failures).** Requires a post-iteration filter operator. Today `evaluator.cpp` only translates relations into start/stop iterators on PK; non-PK relations have no place to land. The smallest design that fits is a thin `Filter` wrapper around `RowIterator` that evaluates the relation against each `ColumnRange`. Interacts with future predicate pushdown work.

- **Secondary indexes (~15 failures).** Need a new storage object (mapping indexed value → partition/clustering key bytes), a planner change to choose between table scan and index lookup, and parser support for `CREATE INDEX`. Touches schema persistence (indexes are catalog entries) and DDL replay on open.

- **SELECT COUNT / scalar functions / TOKEN() (~10 failures).** Requires (a) a result-aggregator stage that runs after `RowIterator` and (b) a scalar-function registry for the simple cases (`token`, `writetime`, `ttl`, type casts, `blobAs*` / `*AsBlob`). The aggregator probably wants the same shape as the prospective ORDER BY buffer.

- **Counter columns (~6 failures, "non-constant/non-bind UPDATE assignment").** Today `UPDATE SET col = <constant|bind>` is the only supported RHS. Counters need `col = col + n` parsing (already a relation/expression in the AST) plus a counter-aware write path that performs read-modify-write inside the engine transaction. The storage layer already serializes integers fine — this is purely an engine path.

- **Collection DML (~26 failures in `collections_test.py`, plus updates).** Two sub-issues: (i) collection literals in `INSERT VALUES` (parser, see smallest-fix #5); (ii) subscript / element update `col[k] = v` and `col = col + {…}` (`engine.cpp:792,868` asserts). The latter needs in-place modification of the collection bytes inside the row blob, which currently has fixed columns. Decide whether collections live in the row blob or in a side-table.

- **BATCH statements (~13 failures).** One assert site (`engine.cpp:1008`). Needs an engine-level entry point that begins a single transaction, dispatches each child statement, and commits. Probably also needs idempotency-vs-conditional semantics. Small surface area, but touches the transaction-borrowing pattern noted in memory `project_transaction_borrowing_bug.md`.

- **TTL / `default_time_to_live` (~7 failures).** Requires expiration metadata per cell (or per row) plus a tombstone-expiry sweep. This is a row-blob layout change; defer until row encoding is otherwise being touched (collection DML is a natural cohort).

- **Static columns on writes (subset of `delete_test.py` / `update_test.py`).** Schema and read-path support exist (see memory `project_iterator_architecture.md`); write path currently does not address the static blob in many DELETE/UPDATE shapes. Small, but easy to do as part of the DELETE/UPDATE-on-clustering-tables work above.

- **`CREATE TABLE` parser coverage (~12 `create_test.py` fails, plus knock-on test setup failures in unrelated files).** Many tests are blocked at *schema setup* because the parser rejects the `CREATE TABLE` shape (composite PK declarations, `WITH compaction = ...` options, static-column declarations in some forms). These count as one test each in `create_test.py` but cascade into the `select_test.py` / `update_test.py` failure counts because their fixtures rely on the same shapes. Worth running a parser-only pass against the upstream test bodies to enumerate the shapes that need to round-trip.
