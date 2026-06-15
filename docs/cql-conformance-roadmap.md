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

## Phase 2 — Parser & Engine Option Handling ✓ COMPLETE

**Delivered (2026-06-14):** `handle_table_option_pair` / `handle_table_options` converted to `void`
(warn-and-ignore); `default_time_to_live` logs and returns instead of asserting; unknown CREATE TABLE /
ALTER TABLE / ALTER KEYSPACE WITH options all warn-and-ignore; collection literal assignment in UPDATE fixed
(`plan_update` and engine UPDATE handler now call `evaluate(assign.value, ctx)` directly, supporting
`ListOrVectorLiteral`, `SetLiteral`, `MapLiteral`); `assert_true_not_implemented` guards added for
`TOIArithmeticOperation` and `AutoString8` in UPDATE assignments. Score: still 24/313 — Phase 2 eliminated
~160 assert fires per run but the tests also require clustering-key mutations (Phase 3) to actually pass.

Notes on pre-existing items: composite partition key grammar and frozen collection type parsing were already
implemented before Phase 2; collection literal writes in INSERT were already working via `io.cpp`. The fire
counts listed in the original Phase 2 spec were accurate for the pre-Phase-1 codebase but overstated the
Phase 2 delta.

---

## Phase 2b — Parser Completion ✓ COMPLETE

**Delivered (2026-06-14):** All remaining "Failed to parse CQL" errors eliminated (0 parse failures, down
from ~70 before Phase 1 and ~15 after Phase 2). Score: 25/313 (+1 from testRandomDeletions).

**Session 1 fixes (8 parser bugs):** BATCH termination whitespace (`APPLY BATCH` preceded by spaces);
`function_selector` far-ahead lookahead → local `peek`; `COUNT(1)` alongside `COUNT(*)`; `(col,col) IN/=`
tuple WHERE relations; LIMIT and PER PARTITION LIMIT bind markers; PER PARTITION LIMIT stored in wrong
field (`s.limit` → `s.per_partition_limit`); UDT literal `{ident: val}` parsing with correct
letter-vs-digit first-char disambiguation.

**Session 2 fixes (4 remaining gaps):**
- **UUID literals** — `uuid_literal` production using `n_digits<N, dsl::hex>` with five captures
  (8-4-4-4-12 groups), peek condition verifies 8 hex chars + `-` before committing. `UUID` was already
  in `ConstantTypes`; value callback decodes hex nibbles to `Array<U8, 16>`.
- **Duration literals** — `duration_literal` with sub-productions per unit (`mo`/`ms`/`us`/`ns` before
  single-char `m`/`s`), list-based component accumulation via `as_dyn_arr<Duration>`, fold to `Duration`.
  `Duration` added to `ConstantTypes`; `write_typed_basic_as_column_value` gains Duration case.
- **Empty IN clause** — `tuple_or_paren` wraps `term_args_list` in `dsl::opt(peek_not(')') >> ...)`;
  `lexy::nullopt` branch yields empty `TupleLiteral`.
- **Empty BATCH body** — `batch_modifications_list` split into `batch_modifications_inner` (sink-based
  list) and `batch_modifications_list` (`dsl::opt(peek_not(APPLY) >> inner)` with callback), allowing
  `BEGIN BATCH APPLY BATCH;`.

**Baseline post-Phase-2b (2026-06-14):** 25/313 passing, 41 xfailed, 2 xpassed, 13 skipped.
All remaining failures are engine asserts (1319 fires/run); zero parse failures. Fire counts are higher
than post-Phase-2 because former silent parse failures now reach the engine.

---

## Phase 3 — `apply_mutation` + Clustering-Table DELETE/UPDATE + Static Columns ✓ SUBSTANTIALLY COMPLETE

**Delivered (2026-06-15):** Clustering-table DELETE (equality, range, partial-CK bounds, empty
blob values); textAsBlob/blobAsText/intAsBlob/bigintAsBlob builtins; empty-blob crash fix in
io.cpp; static-only partition SELECT with PK value injection; SELECT DISTINCT
(`advance_partition()`); SELECT * column ordering (PK → CK → static → regular).
Score: 73/313 (was 25/313).

**Known Phase 3 gaps (see Phase 3b below):**
1. Static column writes — INSERT/UPDATE to static cols in clustering tables (`rewrite_static`).
2. CK prefix equality DELETE — `DELETE WHERE k=? AND c1=0` on a 3-CK table should range-delete
   all rows with c1=0; currently MissingClusteringKey because partial equality is not treated as
   a range (needs `plan_mutation` to detect partial CK equality and convert it to a range locator).
3. Tuple-syntax range DELETE — `WHERE (ck_col) > (?)` sets only filter predicates (not locator
   bounds); DELETE gets MissingClusteringKey. Requires `TupleExpressionRelation` with non-EQ/IN
   operators to populate `ck_begin`/`ck_end` in `build_row_locator`.
4. USING TIMESTAMP on DELETE — causes crash; USING TIMESTAMP handling not yet implemented.

**Design flags** (originally planned for Phase 3, still apply to Phase 3b/gaps)

**`needed_cols` deferred.** The original design couples a `needed_cols` optimization into Phase 3.
This significantly increases scope and is not needed for correctness. Implement `apply_mutation`
without `needed_cols` first — pass an empty set (meaning "all columns") everywhere, add the
optimization later. The `needed_cols: DynamicArray<U64>` fields on `MutationSpec` and
`ProjectionPlan` can be added when the optimization is implemented.

**`locator.index` slot does not exist yet.** Phase 4 refers to "`build_row_locator` already has
the `locator.index` slot (currently unused)." The actual `RowLocator` struct has no such field.
Phase 4 must add it.

**Static columns can be split.** Static column writes are independent of CK mutation correctness
and add significant complexity. Can be deferred to a Phase 3b if needed: Phase 3 delivers CK
DELETE/UPDATE without static-column write support, Phase 3b adds `rewrite_static`.

**`plan_mutation` extension for clustering tables.** Currently `plan_mutation` asserts if
`pk_is_equality` is false. For clustering tables, add: if `ck_is_equality` is also false,
return `PlanError::MissingClusteringKey`. The planner already captures CK equality in
`ck_begin` via the `all_ck_eq` path in `build_row_locator`.

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

## Phase 3b — Static Column Writes, CK Prefix Equality, Tuple Range, USING TIMESTAMP

**Impact: ~10 unique tests blocked by the Phase 3 gaps listed above.**

### Static column writes (`rewrite_static`)

`rewrite_static(pager, entry, tbl, col_values, col_present)`: reads the existing static blob
(if any), merges in updated static columns, writes a new blob, updates `entry.static_page`.
Called from `apply_mutation` after any ColumnUpdate that targets a `col.is_static` column.
INSERT must also route static-col updates through this path for clustering tables.

### CK prefix equality DELETE

`plan_mutation` currently requires `ck_is_equality` for clustering tables. When a query like
`DELETE FROM t WHERE k=? AND c1=0` specifies only the first of N CK columns, treat it as a
range: synthesize `ck_begin = serialize_ck_prefix(c1=0)` and `ck_end = next_prefix(c1=0)`,
setting `ck_begin_is_partial = true` and `ck_end_is_partial = true` (exclusive upper bound at
the next prefix). The existing partial-bounds loop in `collect_range` already handles this.

### Tuple-syntax range DELETE

`build_row_locator` currently routes `TupleExpressionRelation` with non-EQ/IN operators to
filter predicates only. For single-column tuples `(ck_col) OP (?)`, extract the CK column and
operator and populate `ck_begin`/`ck_end` exactly as the scalar relation path does.

### USING TIMESTAMP on DELETE

The engine DELETE handler should parse and store the USING TIMESTAMP value (already in the
parsed `Delete` AST). For now, silently ignore it (log + skip) so tests that use it do not
crash. True timestamp-filtered visibility requires the Phase 10 row blob metadata header.

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

Add `Optional<U64> index_col_idx` to `RowLocator`. `build_row_locator` sets it when an equality
relation on a non-PK column matches an indexed column and `!needs_allow_filtering`. Engine SELECT
path: if `locator.index_col_idx` is set, do a prefix range scan on the index BTree to collect
pk_bytes, then run pk equality lookups.

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
