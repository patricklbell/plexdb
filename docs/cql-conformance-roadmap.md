# CQL Engine Roadmap

## End-State Architecture

When all phases below are complete the engine is organized into three clean layers:

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

**needed_cols avoids deserializing unreferenced columns.** Every plan computes a
`needed_cols: Array<U64>` — the union of columns referenced in SELECT, WHERE filter
predicates, and assignment RHS expressions. `ColumnIterator::load` skips bytes for columns
outside this set. This is important for counter updates (`col = col + n`) and aggregates.

**Aggregation sits above iteration.** `SELECT COUNT(*)` consumes `RowIterator` inside
the engine and returns a synthesized `VirtualRows` row, avoiding deferred-tx lifetime issues.

**Parser and engine are equally critical.** Many tests fail at CREATE TABLE setup before the
engine is reached. Parser gaps block entire test files and must be fixed systematically.
Unknown table/keyspace/index options must be silently ignored (Cassandra behavior).

---

## Phase 1 — Query Planner Module ✓ COMPLETE

**Delivered (2026-06-14):** `cql.engine.planner` module; `build_row_locator`, `plan_select`,
`plan_update`, `plan_delete`; `validate_plan` in engine; `RowLocator`, `FilterPlan`,
`SelectPlan`, `MutationPlan` structs; compound PK equality; non-PK WHERE → RequiresAllowFiltering
error instead of crash; column-level DELETE for non-clustering tables; ORDER BY on non-CK
column → proper error. Score: 24/313 passing (was 23).

---

## Phase 2 — Parser & Engine Option Handling

**Impact: ~20 unique tests, unblocks cascading fixture failures across all test files.**

Many test functions fail at `CREATE TABLE` or `CREATE KEYSPACE` setup before the DML under test
is reached. Fixing the parser and option handling unblocks entire fixture chains.

### Items (approximately by fire count)

**Composite partition key syntax** (`engine.cpp`/parser, 17 fires). Parser does not produce
a `Table` with `partition_key_col_indices.length > 1` for `PRIMARY KEY ((a, b), c)`.
Update the grammar to accept the compound `(col, col, ...)` form inside PRIMARY KEY and
populate `partition_key_col_indices` accordingly. No engine change needed beyond this.

**Ignore unknown `CREATE TABLE WITH` options** (`engine.cpp`, 15 fires). Currently asserts.
Replace with a loop that skips unknown option keys and warns. Cassandra accepts (and ignores)
compaction, compression, caching, gc_grace_seconds, etc. The `Options` visitor in the ALTER
TABLE handler already tolerates unknown options via `handle_table_option_pair`; apply the same
tolerance in CREATE TABLE.

**Frozen collection types** (parser, 16 fires). `FROZEN<LIST<INT>>` is parsed as a distinct
type in some grammars. The schema layer already stores frozen and non-frozen identically
(invariant documented in AGENTS.md). Ensure the parser maps `frozen<T>` to the same
`TypeDescriptor` as `T` before creating the column.

**Collection literal INSERT values** (parser + io, 8+ fires for list/set/map writes).
`INSERT INTO t (col) VALUES ([1, 2, 3])` fails parse. Grammar needs `collection_literal`
production. `io.cpp` needs `write_list_literal`, `write_set_literal`, `write_map_literal`
that serialize via the existing collection byte format (size-prefixed elements).

**Ignore `ALTER TABLE/KEYSPACE WITH` options** (2 fires each). Same pattern as CREATE TABLE.

**`default_time_to_live`** (4 fires). Silently skip at table creation; defer storage to
Phase 10. Currently asserts.

### What this does NOT touch

Engine DML paths, `RowIterator`, `io.cpp` column value read/write for non-collection types,
`planner.cpp`. These changes are grammar + option-handling only.

---

## Phase 3 — `apply_mutation` + Clustering-Table DELETE/UPDATE + Static Columns

**Impact: ~54 unique tests directly; also unblocks ALLOW FILTERING tests that were blocked
only because their fixtures use clustering-table mutations (~20 additional).**

This is the largest single unlock in the roadmap. The conformance log shows 78+30 = 108
assert fires from CK DELETE/UPDATE alone.

### Design: `apply_mutation`

Replace the `execute Update` and `execute Delete` branches in engine.cpp (≈200 lines each)
with:

```
(locator, filter, spec, plan_err) = plan_mutation(stmt, *tbl, ctx)
if err = validate_plan(plan_err): co_return err
co_await apply_mutation(engine, tbl, locator, spec, ctx)
co_return create_void_success()
```

`apply_mutation` is a new free function in engine.cpp (not exported, not in planner):

```
apply_mutation(engine, tbl, locator, spec, ctx):
    pk_bytes = locator.pk_begin   // always an equality; validated by plan_mutation

    if tbl has clustering keys:
        partition_entry = co_await btree::tfind<PartitionEntry>(tbl.btree, pk_bytes)
        if not partition_entry: co_return  // non-existent partition: no-op

        clustering_bt = load ClusteringBTree from partition_entry.data_page
        ck_bytes = locator.ck_begin   // equality required by plan_mutation

        if spec.is_full_delete:
            row_page = co_await btree::tfind<U64>(clustering_bt, ck_bytes)
            if row_page:
                co_await blob::remove(load_blob(*row_page))
                co_await btree::remove(clustering_bt, ck_bytes)
            if btree::is_empty(clustering_bt):
                if partition_entry.static_page != 0:
                    co_await blob::remove(load_blob(partition_entry.static_page))
                co_await btree::remove(tbl.btree, pk_bytes)
        else:
            // read-modify-write (UPDATE and column-level DELETE share this path)
            row_page = co_await btree::tfind<U64>(clustering_bt, ck_bytes)
            col_values, col_present = co_await read_row(pager, tbl, row_page,
                                                         partition_entry.static_page,
                                                         needed_cols)
            apply_updates(col_values, col_present, spec.updates)

            new_row_page = co_await write_row(pager, tbl, col_values, col_present)
            if row_page:
                co_await blob::remove(load_blob(*row_page))
                co_await btree::remove(clustering_bt, ck_bytes)
            co_await btree::tinsert(clustering_bt, ck_bytes, new_row_page)

            if any static column was updated:
                co_await rewrite_static(pager, *partition_entry, tbl,
                                        col_values, col_present)
                co_await btree::tupdate(tbl.btree, pk_bytes, *partition_entry)

    else:  // non-clustering table (also covers the existing UPDATE/DELETE code)
        partition_entry = co_await btree::tfind<PartitionEntry>(tbl.btree, pk_bytes)

        if spec.is_full_delete:
            if partition_entry:
                co_await blob::remove(load_blob(partition_entry.data_page))
                co_await btree::remove(tbl.btree, pk_bytes)
        else:
            existing = partition_entry ?? {new empty blob, static_page=0}
            col_values, col_present = co_await read_row(pager, tbl,
                                                         existing.data_page,
                                                         existing.static_page,
                                                         needed_cols)
            apply_updates(col_values, col_present, spec.updates)
            new_row_page = co_await write_row(pager, tbl, col_values, col_present)

            if partition_entry:
                co_await blob::remove(load_blob(partition_entry.data_page))
                co_await btree::remove(tbl.btree, pk_bytes)
            new_entry = PartitionEntry{new_row_page, existing.static_page}
            if any static column was updated:
                co_await rewrite_static(pager, new_entry, tbl, col_values, col_present)
            co_await btree::tinsert(tbl.btree, pk_bytes, new_entry)
```

### needed_cols

Both `MutationSpec` and `ProjectionPlan` gain a `needed_cols: DynamicArray<U64>` field.
The planner computes it as the union of:
- Column indices referenced in `spec.updates` (for mutations)
- Column indices referenced in filter predicates
- Column indices referenced in SELECT ops (for queries)

`ColumnIterator::load` receives `needed_cols`. When non-empty, it returns `Null` for
columns outside the set, skipping their blob bytes. Pass `{}` (empty = all) for cases where
the full row must be materialized (e.g., `SELECT *`, full-column UPDATE).

### apply_updates

```
apply_updates(col_values, col_present, updates):
    for each ColumnUpdate(col_idx, new_value) in updates:
        if new_value is Null:
            col_present[col_idx] = false
        else:
            col_values[col_idx] = cast_to_column_value(new_value, tbl.cols[col_idx].type)
            col_present[col_idx] = true
```

Phase 7 (counters) adds `row_ctx` to this function so column references in `new_value`
can be resolved against the just-read `col_values`. No other callers change.

### Static column handling

`rewrite_static(pager, entry, tbl, col_values, col_present)` updates the static blob:
reads any existing static row, merges in updated static columns, writes a new blob,
updates `entry.static_page`.

### Index maintenance placeholder

Add to `apply_mutation` at the end (no-op for now, filled in by Phase 4):

```
// co_await update_indexes(engine, tbl, pk_bytes, old_col_values, col_values, col_present)
```

### INSERT update

`execute Insert` currently duplicates read-modify-write logic. After Phase 3, restructure it
to produce a `MutationSpec` (all value-columns as ColumnUpdates) and call `apply_mutation`.

### Planner update

`plan_mutation` currently handles only single-row equality (pk_is_equality required).
For clustering tables, add: if `ck_is_equality` is also false, return
`PlanError::MissingClusteringKey`. `validate_plan` returns the appropriate message.

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

### DML index maintenance (fills the Phase 3 placeholder)

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

`build_row_locator` already has the `locator.index` slot (currently unused). Fill it when
an equality relation on a non-PK column matches an indexed column, and `!needs_allow_filtering`.
Engine SELECT path: if `locator.index != null`, do a prefix range scan on the index BTree
to collect pk_bytes, then run pk equality lookups.

---

## Phase 5 — ORDER BY (Reverse Iterator)

**Impact: ~14 unique tests in select_order_by_test.py. Depends on Phase 3 (clustering
tables) since all valid ORDER BY columns are clustering keys.**

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

### create_table_range_it refactor (can be done in Phase 3 or 5)

Replace the six `create_table_{eq,lt,le,gt,ge,begin}_it` call sites in engine.cpp with:

```
create_table_range_it(pager, tbl, locator) → Task<pair<RowIterator, RowIterator>>
```

This function consults `locator.{pk,ck}_{has_begin,has_end,begin_inclusive,...}` and
`locator.{reverse_partitions,reverse_clustering}` to build both ends of the range in
one place. Callers become a single call per SELECT path.

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

### Changes to apply_updates

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
