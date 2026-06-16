# CQL Engine Roadmap

## Current Status

Phases 1–3b complete. Score: 79/313 passing (2026-06-16).

---

## End-State Architecture

```
native.cpp          — CQL binary protocol: frames in, frames out
engine.cpp          — orchestration: tx lifecycle, plan→validate→execute dispatch,
                      protocol error translation
─────────────────────────────────────────────────────────────────
planner.cppm/cpp    — query analysis: WHERE → RowLocator, SELECT → Projection,
                      SET/selections → MutationSpec, ORDER BY → IterOrder
it.cppm/cpp         — physical row iteration: forward/reverse RowIterator,
                      ColumnIterator, ColumnRange
io.cppm/cpp         — row blob encoding/decoding (column mask + column data)
evaluator.cppm/cpp  — expression evaluation: Terms, TermWithIdentifiers,
                      predicate evaluation, aggregate accumulators
─────────────────────────────────────────────────────────────────
schema.cppm/cpp     — schema persistence: keyspaces, tables, columns, indexes
parsers/            — CQL text → Statement AST (lexy grammar)
```

### Key design principles

**The planner is shared across all DML.** `build_row_locator(where, tbl, ctx)` is called
identically for SELECT, UPDATE, and DELETE. It produces a `RowLocator` describing partition
key bounds, clustering key bounds, and residual filter predicates. What the statement *does*
with the located rows is separate (`ProjectionPlan` for SELECT, `MutationSpec` for mutations).

**plan → validate → execute are three separate steps.**
- `plan_*` functions do pure analysis (no I/O, no error message strings).
- `validate_plan` converts plan-level error codes into protocol `ExecutionResult` values.
- `execute` trusts the plan is valid and does only I/O.

**`apply_mutation` is the single mutation entry point.** INSERT, UPDATE, DELETE, and BATCH
child statements all funnel through one function:
`apply_mutation(engine, tbl, locator, spec, ctx)`.
This is the only place that reads existing rows, applies updates, writes new blobs, and
maintains secondary indexes. Counter evaluation (Phase 7) and index maintenance (Phase 4)
are added to this function without altering its callers.

**Column-level DELETE is a mutation with null assignments.**
`DELETE col1, col2 WHERE pk = ?` produces `MutationSpec{updates=[{col1,Null},{col2,Null}]}`.
This flows through the same `apply_mutation` path as UPDATE.

**`needed_cols` optimization is deferred.** `MutationSpec` and `ProjectionPlan` do not yet
carry `needed_cols: DynamicArray<U64>`. All paths pass an empty set (meaning "all columns").
Add `needed_cols` fields and wire `ColumnIterator::load` to skip unreferenced columns when
implementing Phase 7 (counters) or as a standalone optimization pass.

**Aggregation sits above iteration.** `SELECT COUNT(*)` consumes `RowIterator` inside
the engine and returns a synthesized `VirtualRows` row, avoiding deferred-tx lifetime issues.

**`locator.index` slot does not exist yet.** Phase 4 must add `Optional<U64> index_col_idx`
to `RowLocator`.

---

## Phase 4 — Secondary Indexes

**Impact: ~22 unique tests (secondary index scans, CREATE/DROP INDEX). Must come before
any optimization pass; no dependencies on Phase 5 or later.**

### Schema changes

```
// Index BTree key: indexed_col_bytes ++ pk_bytes (length-prefixed if variable-length)
// Index BTree value: empty (key encodes everything needed)
struct Index:
    idx:         U64
    tombstone:   bool
    name:        String8
    col_idx:     U64          // column this index covers
    btree:       IndexBTree   // BTreePaged<VarlenKeyPolicy, EmptyValuePolicy>

Table gains:
    indexes:     DynamicArray<Index>
```

**Index key format.** Variable-length column values (text, blob) need a length prefix to
avoid ambiguous prefix boundaries: `[len: U16][col_bytes][pk_bytes]`. Fixed-length types
(int, bigint, uuid, etc.) need no prefix since their width is known from the column type.
The planner's `index_lookup` strips the prefix to extract pk_bytes.

`SchemaHeader` gains `indexes_page: U64`. `schema::create_index` and `schema::load` persist
and reload the index catalog alongside keyspaces/tables/columns.

### Parser + engine

The `CreateIndex` AST node gains `column_name`. The parser handles
`CREATE INDEX [name] ON table (column)`. The engine handler calls `schema::create_index`,
then backfills the index by walking the table BTree and calling `update_indexes`.

`DropIndex` follows the tombstone pattern from `DROP TABLE`.

### DML index maintenance

```
update_indexes(engine, tbl, pk_bytes,
               old_col_values, old_col_present,
               new_col_values, new_col_present):
    for each Index in tbl.indexes where not tombstone:
        old_key = make_index_key(old_col_values[idx.col_idx], pk_bytes)  if old_col_present
        new_key = make_index_key(new_col_values[idx.col_idx], pk_bytes)  if new_col_present
        if old_present and old_key != new_key:
            co_await btree::remove(idx.btree, old_key)
        if new_present and old_key != new_key:
            co_await btree::insert(idx.btree, new_key, empty_value)
```

### Planner integration

Add `Optional<U64> index_col_idx` to `RowLocator`. `build_row_locator` sets it when an equality
relation on a non-PK column matches an indexed column and `!needs_allow_filtering`. Engine SELECT
path: if `locator.index_col_idx` is set, do a prefix range scan on the index BTree to collect
pk_bytes, then run pk equality lookups.

---

## Phase 5 — ORDER BY (Reverse Iterator)

**Impact: ~14 unique tests in select_order_by_test.py. Depends on Phase 3 (clustering
tables) since all valid ORDER BY columns are clustering keys.**

`create_table_range_it` already exists in engine.cpp and handles PK and CK bounds for the
SELECT path. Phase 5 extends it with reverse direction support.

### btree::Iterator reverse flag

Add `bool reverse` to `Iterator`. In `advance()`:

```
if reverse: btree::prev(this)
else:        btree::next(this)    // existing behavior
```

Factory functions:
```
rbegin_it<BTree, V>(btree) → Iterator   // last key, reverse=true
rend_it<BTree, V>(btree)   → Iterator   // sentinel
```

A runtime bool is acceptable; the branch cost is negligible vs page I/O.

### RowIterator direction

`RowIterator` gains `reverse_partitions: bool` and `reverse_clustering: bool` (both default
false). `advance()` branches on these flags identically to the scalar iterator case.
The existing `fix_clustering_btree_ptr` invariant is unaffected — it applies on copy/move
regardless of direction.

### Planner integration

`plan_select` sets `locator.reverse_clustering = (col_order.sort == DESC)` instead of
`assert_not_implemented`. The existing `assert_not_implemented("ORDER BY on clustering key")`
becomes dead code and is removed.

`create_table_range_it` consults `locator.reverse_partitions` and `locator.reverse_clustering`
to choose `rbegin_it`/`rend_it` vs `begin_it`/`end_it` when constructing the iterator pair.

---

## Phase 6 — Aggregation / SELECT COUNT

**Impact: ~10 unique tests (COUNT, simple scalar functions). No dependency on Phase 5.**

### Execution path

When `projection.is_aggregate`:

```
(start_it, stop_it) = co_await create_table_range_it(locator, pager, tbl)
count = 0U64
it = start_it
while it != stop_it:
    col_range = co_await it.deref()
    row_ctx = build_row_ctx(tbl, col_range, projection.needed_cols)
    if evaluate_where(filter.predicates, row_ctx): count += 1
    co_await it.advance()
co_return ExecutionResult{VirtualRows = synthesize_count_row(count)}
```

The transaction closes before returning (no deferred-tx). `synthesize_count_row` builds
a single-row `VirtualRows` matching the existing system table helper signature.

### Supported operations

- `COUNT(*)` → `needed_cols = {}` (no column deserialization needed, just advance iterator)
- `COUNT(col)` → `needed_cols = {col_idx}`, increment only when `col_present[col_idx]`
- Other aggregate functions (`SUM`, `AVG`, `MIN`, `MAX`) → `assert_not_implemented`
- Scalar functions in SELECT (`writetime`, `ttl`, `now`, `token`, type casts) → `assert_not_implemented`

The SelectOp types `FunctionCall` and `Cast` from the end-state spec are added to
`ProjectionPlan` but branch to `assert_not_implemented` until dedicated phases address them.

---

## Phase 7 — Counter Columns (non-constant/non-bind UPDATE assignments)

**Impact: ~7 unique tests in counters_test.py. Depends on Phase 3 (`apply_mutation`).**

The parser already produces `TermWithIdentifiers` with `TOIArithmeticOperation` for
`col = col + n`. The evaluator's `evaluate_toi` already resolves `AutoString8` column refs
via `ctx.row_values`. The only missing piece: `apply_mutation` must provide a row context.

### Changes to MutationSpec / ColumnUpdate

```
struct ColumnUpdate:
    col_idx:   U64
    new_value: TaggedUnion<Evaluated, TermWithIdentifiers>
    // Evaluated  → constant/bind; applied directly
    // TermWithIdentifiers → counter expression; resolved at apply time with row_ctx
```

`plan_mutation` stores `TermWithIdentifiers` in `new_value` when the assignment RHS has
column references (detected by `type_matches_tag<AutoString8>` anywhere in the term).
`needed_cols` is extended to include the source column of the counter expression.

### Changes to apply_updates_to_row

```
apply_updates(col_values, col_present, updates, row_ctx):
    for each ColumnUpdate(col_idx, new_value) in updates:
        if type_matches_tag<TermWithIdentifiers>(new_value):
            evaluated = evaluate_toi(get<TermWithIdentifiers>(new_value), row_ctx)
        else:
            evaluated = get<Evaluated>(new_value)
        // then same as before
```

`row_ctx` is built inside `apply_mutation` after reading `col_values`:
```
row_ctx = ctx
row_ctx.row_values = col_values.ptr
```

---

## Phase 8 — Collection DML

**Impact: ~22 unique tests in collections_test.py. Complex; isolated from other phases.**

Two independent sub-problems:

**Sub-problem A: collection literals in INSERT/UPDATE** (~8 fires for list, ~5 for set, ~1 for map).
Parser produces a literal constant (e.g., `[1, 2, 3]`). `io.cpp` needs writers:
`write_list_value`, `write_set_value`, `write_map_value` that serialise element count +
element bytes using the same format that `read_column_value` already expects.

**Sub-problem B: collection subscript update** (`col[k] = v`, `col = col + {…}`, 8+ fires).
`planner.cpp` currently asserts when `assign.target.access != null`. Handling:
- `col[k] = v` → read existing collection blob, find element at key k, overwrite it, rewrite blob.
- `col = col + {1, 2}` → read existing, append new elements (list) or merge (set/map), rewrite.

**Storage decision.** Keep collections in the row blob as a contiguous byte range.
Element-level update is O(collection_size) read-modify-write, which matches the correctness
requirement. A side-table per-element design (Cassandra's wide-row SSTable model) would give
O(1) updates but is a major storage-layer change; defer unless profiling shows it is needed.

`ColumnUpdate.new_value` gains a third variant: `CollectionPatch` describing the delta
(subscript set, append, merge). `apply_updates` materializes the existing collection value,
applies the patch, and writes back.

---

## Phase 9 — BATCH

**Impact: ~11 unique tests in batch_test.py. Depends on Phase 3 (`apply_mutation`).**

### Execution model

BATCH is a single `execute_inside_transaction` call that dispatches each child statement:

```
execute BATCH:
    assert_true_not_implemented(!stmt.if_, "conditional BATCH (LWT) not implemented")
    for each child in stmt.statements:
        mp = plan_mutation(child, *tbl, ctx)   // child is Update|Delete|Insert
        if err = validate_plan(mp.result): co_return err
        co_await apply_mutation(engine, tbl, mp.locator, mp.spec, ctx)
    co_return create_void_success()
```

The outer transaction wraps all child mutations atomically; no per-child commit.

### Limitation

Conditional BATCH (`IF` clauses, light-weight transactions) requires compare-and-swap
semantics that interact with Cassandra's Paxos protocol. Mark with `assert_not_implemented`
and defer indefinitely.

---

## Phase 10 — TTL / Row Blob Metadata

**Impact: ~6 unique tests. Breaking on-disk format change — database must be recreated.**

### New row blob layout

Prefix every row blob with a metadata header:

```
[ row_flags: U8 ][ expiry_unix_ms: S64 ][ col_count: U64 ][ mask_words... ][ column data ]
```

`row_flags` bit 0 = `HAS_TTL`. The header is always present (9 bytes) for uniform reading.
`io.cppm` adds `RowMetadata` struct and `write_row_metadata` / `read_row_metadata`.
All blob writers (`write_row`, INSERT path) write the header. `ColumnIterator::load` reads
and skips it before the existing column mask.

### INSERT USING TTL

Remove the `ExecutionResult::Invalid` return for TTL in the INSERT handler:

```
if param.kind == TTL:
    metadata.flags |= ROW_FLAG_HAS_TTL
    metadata.expiry_unix_ms = os::unix_ms_now() + ttl_seconds * 1000
```

Pass metadata to `write_row`.

### `default_time_to_live`

Add `default_ttl_ms: S64 = 0` to `TableHeader`. Set from `CREATE TABLE ... WITH default_time_to_live = N`
(removes the Phase 2 silent-skip). Apply to every INSERT on that table that doesn't supply
explicit USING TTL.

### TTL sweep (deferred, not in this phase)

TTL expiry is a scan+tombstone operation. Trigger candidates: on open, after N mutations,
or on explicit COMPACT statement. Design separately; the row blob format change here is
the prerequisite.
