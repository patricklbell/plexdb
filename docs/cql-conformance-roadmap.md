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
```

### Key design principles (inspired by ScyllaDB's cql3 layer)

**The planner is shared across all DML.** `build_row_locator(where, tbl, ctx)` is called
identically for SELECT, UPDATE, and DELETE. It produces a `RowLocator` describing partition
key bounds, clustering key bounds, and residual filter predicates. What the statement *does*
with the located rows is separate (`ProjectionPlan` for SELECT, `MutationSpec` for mutations).
This mirrors how ScyllaDB's `cql3::restrictions` is shared between read and mutation paths.

**plan → validate → execute are three separate steps.**
- `plan_*` functions do pure analysis (no I/O, no error message strings).
- `validate_plan` converts plan-level error codes into protocol `ExecutionResult` values.
  Only this layer knows about CQL error messages, status codes, and string formatting.
- `execute_plan` trusts the plan is valid and does only I/O.

**Column-level DELETE is a mutation with null assignments.**
`DELETE col1, col2 FROM t WHERE pk = ?` is identical to
`UPDATE t SET col1 = null, col2 = null WHERE pk = ?` at the storage level.
The row blob's null mask already encodes absent columns (bit = 0, no data bytes).
Both statements produce the same `MutationSpec` and share the entire execution path.

**Aggregation sits above iteration.**
`SELECT COUNT(*)` consumes the `RowIterator` without returning a `RowRange` to the caller.
The result is a single synthesized `VirtualRows` row, avoiding deferred-tx complexity.

**Unimplemented features are `assert_not_implemented`.**
Partially-implemented engine paths that accept unsupported client input call
`assert_not_implemented`. Nothing is stubbed with a no-op.

---

## Phase 1 — Query Planner Module + Plan/Validate/Execute Separation

### New module: `cql.engine.planner` (planner.cppm / planner.cpp)

The planner owns all query analysis. No I/O, no coroutines, no error message strings.

#### Core types

```
// ── Row location ───────────────────────────────────────────────────────────
struct RowLocator:
    pk_begin:             Optional<bytes>
    pk_end:               Optional<bytes>
    pk_begin_inclusive:   bool = true
    pk_end_inclusive:     bool = true
    pk_is_equality:       bool = false   // pk_begin == pk_end; point lookup

    ck_begin:             Optional<bytes>
    ck_end:               Optional<bytes>
    ck_begin_inclusive:   bool = true
    ck_end_inclusive:     bool = true
    ck_is_equality:       bool = false

    reverse_partitions:   bool = false
    reverse_clustering:   bool = false

    index:                *Index = null  // non-null → use index BTree

// ── Iteration-time filter (post-locator) ───────────────────────────────────
struct FilterPlan:
    predicates:           Array<WhereClause::Relation>
    needs_allow_filtering: bool          // true if predicates non-empty and no index

// ── SELECT projection ──────────────────────────────────────────────────────
struct SelectOp:
    variant:
        ColumnRef { col_idx }
        CountStar {}
        FunctionCall { name, arg_col_indices: Array<U64> }
        Cast { col_idx, to_type }

struct ProjectionPlan:
    ops:             Array<SelectOp>
    is_aggregate:    bool              // true if any op is CountStar or aggregate function
    needed_cols:     Array<U64>        // union of col_indices referenced by ops + filter

// ── Mutation spec (UPDATE / DELETE / INSERT) ───────────────────────────────
struct ColumnUpdate:
    col_idx:    U64
    new_value:  Evaluated              // Null means clear this column (delete it)

struct MutationSpec:
    updates:        Array<ColumnUpdate>
    is_full_delete: bool               // true if entire row/partition is deleted

// ── Plan error (protocol-agnostic) ─────────────────────────────────────────
enum PlanError:
    None
    RequiresAllowFiltering
    MissingPartitionKey            // UPDATE/DELETE WHERE clause has no pk equality
    OrderByOnNonClusteringColumn
    ColumnNotFound { name }
    TypeMismatch { col_name }

struct PlanResult:
    error:   PlanError = None
    context: String8 = ""          // static storage only (column/option name, etc.)
```

#### Planner functions

```
// Shared WHERE-clause analysis — called by plan_select, plan_mutation
build_row_locator(where, tbl, ctx) → (RowLocator, FilterPlan):
    for each relation in where.relations:
        if relation is ColumnExpressionRelation:
            col = relation.column
            if col is partition key column:
                map operator to pk_begin/pk_end bounds
                set pk_is_equality if operator == eq
            elif col is clustering key column:
                map operator to ck_begin/ck_end bounds
                set ck_is_equality if operator == eq
            elif tbl has index on col and operator == eq:
                locator.index = &tbl.indexes[i]
                locator.pk_is_equality = true
            else:
                filter.predicates.push(relation)
                filter.needs_allow_filtering = true
        elif relation is TupleExpressionRelation:
            assert_not_implemented("tuple expression relations")
        elif relation is TokenRelation:
            assert_not_implemented("token relations")
    return (locator, filter)


plan_select(stmt: Select, tbl, ctx) → (RowLocator, FilterPlan, ProjectionPlan, PlanResult):
    if stmt.allow_filtering == false and filter.needs_allow_filtering:
        result.error = RequiresAllowFiltering
    if stmt.order_by:
        for each col_order in stmt.order_by.columns:
            if col_order.column is not a clustering key:
                result.error = OrderByOnNonClusteringColumn
            locator.reverse_clustering = (col_order.sort == DESC)
    build projection from stmt.select.clauses
    compute projection.needed_cols = union(select_col_indices, filter_predicate_col_indices)
    return (locator, filter, projection, result)


plan_mutation(stmt: Update|Delete, tbl, ctx) → (RowLocator, FilterPlan, MutationSpec, PlanResult):
    (locator, filter) = build_row_locator(stmt.where, tbl, ctx)
    if not locator.pk_is_equality:
        result.error = MissingPartitionKey

    if stmt is Update:
        for each assignment in stmt.assignments:
            idx = find_col_idx(tbl, assignment.target.column)
            if assignment.target.access != null:
                assert_not_implemented("subscript/field access in UPDATE SET")
            spec.updates.push(ColumnUpdate{ idx, evaluate_toi(assignment.value, ctx) })
            // NOTE: evaluate_toi handles column refs (counter: col = col + n)
            //       by resolving AutoString8 → Null at plan time; the actual
            //       current value is substituted at execution time with row_ctx

    elif stmt is Delete:
        if stmt.selections is empty:
            spec.is_full_delete = true
        else:
            // column-level delete: same as UPDATE SET col = null
            for each selection in stmt.selections:
                idx = find_col_idx(tbl, selection.column)
                spec.updates.push(ColumnUpdate{ idx, Null })

    return (locator, filter, spec, result)
```

#### Validation layer (in engine.cpp, not in planner)

```
validate_plan(result: PlanResult) → Optional<ExecutionResult>:
    switch result.error:
        None                     → return nullopt
        RequiresAllowFiltering   → return ExecutionResult{Invalid,
                                     "Cannot execute this query as it might involve data
                                     filtering and thus may have unpredictable performance.
                                     If you want to execute this query despite the
                                     performance unpredictability, use ALLOW FILTERING"}
        MissingPartitionKey      → return ExecutionResult{Invalid,
                                     "UPDATE/DELETE requires partition key equality in WHERE"}
        OrderByOnNonClusteringColumn → return ExecutionResult{Invalid, ...}
        ColumnNotFound           → return ExecutionResult{Invalid, "Column ... not found"}
        TypeMismatch             → return ExecutionResult{Invalid, "Incompatible type for column ..."}
```

#### Engine integration (SELECT path rewritten)

```
execute SELECT:
    (locator, filter, projection, plan_err) = plan_select(stmt, *tbl, ctx)
    if err = validate_plan(plan_err): co_return err

    if projection.is_aggregate:
        // aggregate: consume iterator immediately, return VirtualRows
        it = co_await build_iterator(locator, pager, tbl)
        count = 0
        while it != end_it:
            col_range = co_await it.deref()
            // materialize only needed_cols via ColumnIterator
            if evaluate_where(filter.predicates, row_ctx): count += 1
            co_await it.advance()
        co_return ExecutionResult{ VirtualRows = synthesize_count_row(count) }

    // non-aggregate: return live iterator to caller
    (start_it, stop_it) = co_await build_iterator_range(locator, pager, tbl)
    co_return ExecutionResult{
        Rows, start=start_it, stop=stop_it,
        filter=filter.predicates, projection=projection.ops,
        needed_cols=projection.needed_cols
    }


execute UPDATE / DELETE:
    (locator, filter, spec, plan_err) = plan_mutation(stmt, *tbl, ctx)
    if err = validate_plan(plan_err): co_return err
    co_await apply_mutation(engine, tbl, locator, spec, ctx)
    co_return create_void_success()
```

The existing column-resolve and key-serialization code moves from engine.cpp into planner.cpp.
The assert_not_implemented calls for non-PK relations and ORDER BY are removed from engine.cpp;
the planner handles these cases through PlanError.

---

## Phase 2 — Reverse Iterator (ORDER BY)

### btree::Iterator direction

The existing `btree::Iterator<BTree, V>` advances forward (`btree::next`). To support reverse
without duplicating the iterator struct, add a `bool reverse` field and branch in `advance()`:

```
Iterator<BTree, V>::advance():
    if reverse:
        btree::prev(this)   // walk to the previous key
    else:
        btree::next(this)   // existing behavior

// New factory functions in btree:
rbegin_it<BTree, V>(btree) → Iterator   // positioned at last key, reverse=true
rend_it<BTree, V>(btree)   → Iterator   // sentinel (same shape as begin sentinel)
```

Templates over direction should be preferred if the btree layer supports it without undue
complexity; otherwise a runtime bool is acceptable here since the branch has negligible cost
compared to the page I/O.

### RowIterator direction

Add `reverse_partition: bool` and `reverse_clustering: bool` to `RowIterator`.

```
RowIterator::advance():
    if table has clustering keys:
        advance clustering_it (respecting reverse_clustering)
        if clustering_it == clustering_end_it:
            advance partition_it (respecting reverse_partition)
            if partition_it != end: reload clustering_it for new partition
    else:
        advance partition_it (respecting reverse_partition)
```

### New factory functions (it.cppm)

```
create_table_rbegin_it(pager, tbl) → Task<RowIterator>
// Bounds-aware reverse factories (used by planner integration):
create_table_range_it(pager, tbl, locator) → Task<(RowIterator start, RowIterator stop)>
```

`create_table_range_it` replaces the six `create_table_{eq,lt,le,gt,ge,begin}_it` call sites in
engine.cpp with a single call driven by the planner's `RowLocator`.

---

## Phase 3 — DELETE / UPDATE on Clustering Tables + Column-Level DELETE

All paths share `apply_mutation` (new function in engine.cpp), which centralizes:
- row read, mutation application, row rewrite, static column handling, index maintenance.

### apply_mutation (pseudocode)

```
apply_mutation(engine, tbl, locator, spec, ctx):
    pk_bytes = locator.pk_begin  // guaranteed equality by validate_plan

    partition_entry = co_await btree::tfind<PartitionEntry>(tbl.btree, pk_bytes)

    if tbl has clustering keys:
        if not partition_entry: co_return  // nothing to mutate
        ck_bytes = locator.ck_begin        // equality required for single-row mutations

        clustering_bt = ClusteringBTree(pager, partition_entry.data_page)
        row_page = co_await btree::tfind<U64>(clustering_bt, ck_bytes)

        if spec.is_full_delete:
            if row_page:
                co_await blob::remove(load_blob(row_page))
                co_await btree::remove(clustering_bt, ck_bytes)
            // if clustering BTree is now empty: remove partition entry + static blob
            if btree::is_empty(clustering_bt):
                if partition_entry.static_page != 0:
                    co_await blob::remove(load_blob(partition_entry.static_page))
                co_await btree::remove(tbl.btree, pk_bytes)
        else:
            // read-modify-write (shared by UPDATE and column-level DELETE)
            col_values, col_present = co_await read_row(pager, tbl, row_page,
                                                         partition_entry.static_page,
                                                         needed_cols)
            apply_updates(col_values, col_present, spec.updates, row_ctx)
            // row_ctx = ctx with table=tbl, row_values=col_values.ptr
            // (enables counter: col = col + n via evaluate_toi)

            new_row_page = co_await write_row(pager, tbl, col_values, col_present)
            co_await blob::remove(load_blob(row_page))
            co_await btree::remove(clustering_bt, ck_bytes)
            co_await btree::tinsert(clustering_bt, ck_bytes, new_row_page)

            if any update targets a static column:
                co_await rewrite_static(pager, partition_entry, tbl,
                                        col_values, col_present)
                co_await btree::tupdate(tbl.btree, pk_bytes, partition_entry)

    else:  // non-clustering table
        if spec.is_full_delete:
            if partition_entry:
                co_await blob::remove(load_blob(partition_entry.data_page))
                co_await btree::remove(tbl.btree, pk_bytes)
        else:
            // same read-modify-write path
            existing = partition_entry ?? PartitionEntry{ new empty row_page, 0 }
            col_values, col_present = co_await read_row(pager, tbl,
                                                         existing.data_page,
                                                         existing.static_page,
                                                         needed_cols)
            apply_updates(col_values, col_present, spec.updates, row_ctx)
            new_row_page = co_await write_row(pager, tbl, col_values, col_present)

            co_await btree::remove(tbl.btree, pk_bytes)   // remove old if present
            new_entry = PartitionEntry{ new_row_page, existing.static_page }

            if any update targets a static column:
                co_await rewrite_static(pager, new_entry, tbl,
                                        col_values, col_present)
            co_await btree::tinsert(tbl.btree, pk_bytes, new_entry)
```

### apply_updates (sync helper, no I/O)

```
apply_updates(col_values, col_present, updates, row_ctx):
    for each ColumnUpdate(col_idx, new_value) in updates:
        evaluated = evaluate_toi_with_row(new_value, row_ctx)
        // row_ctx.row_values = col_values.ptr so column refs resolve to current values
        if evaluated is Null:
            col_present[col_idx] = false
        else:
            col_values[col_idx] = evaluated
            col_present[col_idx] = true
```

### Column-level DELETE reuse

`DELETE col1, col2 FROM t WHERE pk = ?` goes through `plan_mutation` which produces a
`MutationSpec` with `updates = [{col1, Null}, {col2, Null}]` and `is_full_delete = false`.
The execution follows the exact same `apply_mutation` path as UPDATE. No separate code path.
The row blob's null mask (bit = 0 means column absent, no bytes in blob) handles the storage
representation — clearing col_present[idx] in `apply_updates` and then calling `write_row`
produces the correct mask and packed column data.

### Existing UPDATE code removal

The current `execute Update` and `execute Delete` branches in `engine.cpp` (≈200 lines each)
are replaced by:
```
(locator, filter, spec, plan_err) = plan_mutation(stmt, *tbl, ctx)
if err = validate_plan(plan_err): co_return err
co_await apply_mutation(engine, tbl, locator, spec, ctx)
```

---

## Phase 4 — Secondary Indexes

### Schema changes

Add to `schema.cppm`:

```
// Index BTree: indexed_col_bytes (varlen key) → pk_bytes (varlen value)
using IndexBTree = btree::BTreePaged<VarlenKeyPolicy, VarlenValuePolicy>

struct Index:
    idx:        U64
    tombstone:  bool
    name:       String8
    col_idx:    U64          // column index in owning Table (name derivable from tbl.cols[col_idx])
    btree:      IndexBTree

Table gains:
    indexes: Array<Index>
```

`SchemaHeader` gains an `indexes_page: U64`. Schema `create` and `load` persist/reload indexes
alongside the existing keyspaces/tables/columns blobs. A packed `IndexHeader` struct:

```
IndexHeader:
    tombstone:    bool
    col_idx:      U64
    table_idx:    U64
    btree_page:   U64
    name_length:  U64
```

Schema functions:
```
create_index(schema, tbl, stmt: CreateIndex) → Task<Result<Index*>>
delete_index(schema, tbl, name) → Task<Result<void>>
```

### Parser update for CREATE INDEX

`CreateIndex` in `statements.cppm` gains `column_name: AutoString8`.
The parser is updated to parse `ON table_name (column_name)`.

### Engine: CREATE INDEX / DROP INDEX

Replace the `assert_not_implemented` at the `CreateIndex` handler with:
```
validate table and column exist
co_await schema::create_index(engine.schema, *tbl, stmt)
co_return create_schema_changed(ks_name, tbl_name)
```

Add `DropIndex` statement (if not in AST) and a symmetrical handler.

### DML index maintenance

A helper called from `apply_mutation` and the INSERT path:

```
update_indexes(engine, tbl, pk_bytes,
               old_col_values, old_col_present,
               new_col_values, new_col_present):
    for each index in tbl.indexes:
        old_present = old_col_present[index.col_idx]
        new_present = new_col_present[index.col_idx]
        if old_present:
            old_key = serialize_col_value(old_col_values[index.col_idx])
            co_await btree::remove(index.btree, old_key + pk_bytes)
        if new_present:
            new_key = serialize_col_value(new_col_values[index.col_idx])
            co_await btree::insert(index.btree, new_key + pk_bytes, empty_value)
```

Note: the index BTree key is `indexed_col_bytes ++ pk_bytes` concatenated, with an empty
value. This allows prefix range scans (all entries for a given indexed value) without needing
a separate varlen value policy. The planner extracts pk_bytes by stripping the index key's
leading indexed_col_bytes length.

### INSERT index maintenance

After writing the row blob and inserting into `tbl.btree`, call:
```
update_indexes(engine, tbl, pk_bytes,
               empty (no old values), new_col_values, new_col_present)
```

### Planner integration

`build_row_locator` already sets `locator.index = &tbl.indexes[i]` when a non-PK equality
relation matches an indexed column. Execution path for index lookup:

```
if locator.index != null:
    indexed_val_bytes = serialize_col_value(evaluate(eq_relation.value, ctx))
    // prefix scan: all keys starting with indexed_val_bytes
    pk_bytes = co_await index_lookup(locator.index->btree, indexed_val_bytes)
    // then do a pk equality lookup using pk_bytes
```

---

## Phase 5 — Aggregation / SELECT COUNT

### ProjectionPlan aggregation flag

`plan_select` sets `projection.is_aggregate = true` when any `SelectOp` is `CountStar` or
an aggregating function. When `is_aggregate` is true, the engine consumes all rows before
returning rather than handing a live `RowRange` to the caller.

### Execution path

```
if projection.is_aggregate:
    count = 0
    (start_it, stop_it) = co_await build_iterator_range(locator, pager, tbl)
    it = start_it
    while it != stop_it:
        col_range = co_await it.deref()
        row_ctx = build_row_ctx(tbl, col_range, projection.needed_cols)
        if evaluate_where(filter.predicates, row_ctx):
            accumulate(projection.ops, row_ctx, &count)
        co_await it.advance()
    co_return ExecutionResult{ VirtualRows = synthesize_aggregate_row(projection.ops, count) }
```

`synthesize_aggregate_row` builds a single-row `VirtualRows` result with the aggregate
values, using the same `VirtualRows` infrastructure already used for system tables.

### Supported aggregations

- `COUNT(*)` → row count.
- `COUNT(col)` → count of non-null values.

Function calls in SELECT (e.g., `writetime`, `ttl`, `now`) are handled by the existing
`evaluate_function_call` in `evaluator.cpp`. `writetime` and `ttl` return 0 until Phase 7
adds per-row TTL metadata; use `assert_not_implemented` until then.

`TOKEN(col)` is `assert_not_implemented` — deferred until multi-node sharding is designed.

---

## Phase 6 — Counter Columns (non-constant/non-bind UPDATE assignments)

The parser already produces `TermWithIdentifiers` with `TOIArithmeticOperation` for
`col = col + n`. The evaluator already handles `AutoString8` in `evaluate_toi` via
`lookup_column_value(col_name, ctx)`. The blocker is that `apply_mutation` must provide
a `row_ctx` with `ctx.table = tbl` and `ctx.row_values = col_values.ptr`.

### Changes to apply_mutation

```
// After reading col_values from existing row:
row_ctx = ctx
row_ctx.table = tbl
row_ctx.row_values = col_values.ptr
// Pass row_ctx to apply_updates so evaluate_toi resolves column refs to current values
apply_updates(col_values, col_present, spec.updates, row_ctx)
```

The `plan_mutation` function currently evaluates `TermWithIdentifiers` at plan time
(producing `Null` for column refs since no row context is available). Instead, for
`TermWithIdentifiers` values containing column references, store the `TermWithIdentifiers`
unevaluated in `ColumnUpdate.new_value` and defer evaluation to `apply_updates` where
`row_ctx` is available:

```
ColumnUpdate:
    col_idx:       U64
    new_value:     TermWithIdentifiers   // evaluated at apply time with row_ctx
```

`plan_mutation` sets `needed_cols` to include any column referenced in `TermWithIdentifiers`
values (so `ColumnIterator` materializes them).

### Column determination optimization

Both `ProjectionPlan.needed_cols` (for SELECT) and `MutationSpec.needed_cols` (for
UPDATE/DELETE, derived from column refs in assignments and filter predicates) are passed
to the ColumnIterator load function:

```
load(col_it, pager, tbl, row_page, static_page, needed_cols: Array<U64>)
```

When `needed_cols` is non-empty, `ColumnIterator::deref()` returns `Null` for columns not
in the set, skipping their blob bytes. This avoids deserializing columns the statement
does not reference.

---

## Phase 7 — TTL / Row Blob Metadata

This is a **breaking on-disk format change**. The database must be recreated after this
phase is merged (consistent with existing project policy).

### New row blob layout

Current layout:
```
[ col_count: U64 ][ mask_words: U64... ][ packed column data ]
```

New layout:
```
[ row_flags: U8 ][ expiry_unix_ms: S64 ][ col_count: U64 ][ mask_words: U64... ][ packed column data ]
```

`row_flags` bit 0 = `HAS_TTL`. `expiry_unix_ms` is only meaningful when `HAS_TTL` is set.
The header is always present (12 bytes) for all rows, even those without TTL, to keep
the reader uniform. `io.cppm` exposes:

```
struct RowMetadata:
    flags:           U8 = 0
    expiry_unix_ms:  S64 = 0

ROW_FLAG_HAS_TTL: U8 = 0x01

write_row_metadata(writer, metadata)   // called before write_column_mask
read_row_metadata(reader) → RowMetadata  // called before read_column_mask
```

All blob writers (INSERT, `write_row` in `apply_mutation`) write the metadata header.
`ColumnIterator::load` reads and skips past it. `blob::create_cursor` offset is adjusted
by `sizeof(RowMetadata)`.

### INSERT USING TTL

Remove the `ExecutionResult::Invalid` return for TTL in the INSERT handler.
```
if using_param.kind == TTL:
    metadata.flags |= ROW_FLAG_HAS_TTL
    metadata.expiry_unix_ms = os::unix_ms_now() + ttl_seconds * 1000
```

Pass metadata to `write_row`.

### Compaction (deferred)

Add to `docs/todo.md`:

```
TTL expiry sweep:
  - Walk all rows, read RowMetadata from each row blob.
  - If HAS_TTL and expiry_unix_ms < now: delete the row via apply_mutation.
  - For clustering tables: delete all expired rows; if partition empties, delete partition.
  - Trigger candidates: (a) on database open, (b) after N mutations per table,
    (c) explicit COMPACT TABLE statement.
  - static blobs and partition entries must be consistent after expiry sweep.
  - default_time_to_live: store as a field in TableHeader; apply to every INSERT
    on that table that does not supply an explicit USING TTL.
```
