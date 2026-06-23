# CQL Engine Roadmap

## Status

Conformance: **144 passing** in `tools/cql_tests/mustpass.txt`. No regressions
gating CI.

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

**The planner is shared across all DML.** `RowLocator` is shared across statements and describes partition
key bounds, clustering key bounds, and residual filter predicates. What the statement *does*
with the located rows is separate (`ProjectionPlan` for SELECT, `MutationSpec` for mutations).

**plan → validate → execute are three separate steps.**
- `plan_*` functions do pure analysis (no I/O, no error message strings).
- `validate` checks the plan is valid.
- `execute` trusts the plan is valid and does storage layer changes.

**Aggregation sits above iteration.** `SELECT COUNT(*)` consumes `RowIterator` inside
the engine and returns a synthesized `VirtualRows` row, avoiding deferred-tx lifetime issues.

---

## Phase 11 — Drop PK/CK duplication from the row blob

**Net code reduction; no conformance gain directly, but simplifies Phase 12
(TTL projection) and shrinks every row blob. Breaking on-disk format change —
recreate the database.**

Today the row blob's column mask covers PK and CK columns even though their
values are already encoded in the partition/clustering keys. `ColumnIterator`
already knows how to recover PK values from the encoded key bytes — the
`static_only_row` branch in `RowIterator::deref` deserializes the partition key
and passes it as `injected_pk` to `load`, and `ColumnIterator::deref` returns
those values when the mask says the cell is absent. Extending the same
mechanism to CK (and making it unconditional, not just for `static_only_row`)
makes the blob smaller and eliminates the engine-level `inject_key_columns`
helper plus its eight callers.

### Writer — exclude PK and CK alongside static

Three writers compute "is this column live in the row blob": the INSERT
inline writer (`row_is_active`), `write_row_blob` (`is_active`), and
`rewrite_static` (`is_active`). All three already exclude static columns.
Extend each to also exclude PK and CK:

```
is_active(ci) = present[ci]
              && !cols[ci].is_static
              && cols[ci].key_kind == KeyKind::None
```

Mask bits for PK/CK positions are then always zero; their values are
never written to the blob.

### Reader — inject PK and CK from key bytes inside `load`

Replace `injected_pk` (a `DynamicArray<ColumnValue>` parameter to `load`)
with a pair of byte spans the iterator deserializes itself:

```
load(it, pager, tbl, page_idx, static_page_idx,
     pk_bytes: TArrayView<const U8, U16>,
     ck_bytes: TArrayView<const U8, U16>):
    ... existing blob open + read_blob_header ...
    it.injected_pk = pk_bytes.length > 0
                       ? key::deserialize_partition(*tbl, pk_bytes)
                       : {}
    it.injected_ck = ck_bytes.length > 0
                       ? key::deserialize_clustering(*tbl, ck_bytes)
                       : {}
```

`deref()` keeps its current shape but consults `key_kind` directly
instead of a linear search through `partition_key_col_indices`:

```
deref():
    if current_is_null():
        col = table.cols[current_column_idx]
        if col.key_kind == PartitionKey  && injected_pk.length > 0:
            return injected_pk[col.key_position]
        if col.key_kind == ClusteringKey && injected_ck.length > 0:
            return injected_ck[col.key_position]
        return Null
    ...
```

`RowIterator::deref` already has both byte spans at hand —
`partition_it.key()` for PK, `clustering_it.key()` for CK on
clustering tables — and forwards them to `load`. The
`static_only_row` branch passes its PK bytes and an empty CK span,
which matches today's behavior.

### Caller simplification

The eight engine-level call sites that today do

```
co_await read_row_into(engine, tbl, row_page, static_page, cv, present, ...);
inject_key_columns(*tbl, pk_view, ck_view, cv, present);
```

collapse to a single `read_row_into` call once that helper grows
`pk_bytes` / `ck_bytes` parameters and forwards them to `load`. The
free function `inject_key_columns` is then unreferenced and deletable.

`read_row_into`'s `out_metadata` parameter from Phase 10 stays as-is.

### What gets deleted

- `inject_key_columns` (the static helper in `engine.cpp`).
- The eight inline `inject_key_columns(...)` calls.
- The `injected_pk` parameter on `load(ColumnIterator)`, replaced by
  byte-span params.
- The linear-search `for (ki = 0; ki < partition_key_col_indices.length; ki++)`
  in `ColumnIterator::deref` — replaced by an O(1) `key_position` index.
- Per-INSERT/UPDATE/DELETE bytes written: one mask bit per PK and CK
  column was 0 and is now structurally absent — same; but every PK and
  CK column's encoded value (often the bulk of small rows) leaves the
  blob.

### Migration

Row blobs written by prior builds carry PK/CK values in their masks.
The new reader would see those as duplicate-but-consistent data and
the new injection would not run for them, so `deref()` would return
the blob value — accidentally correct, but only because the blob and
key agree. Rather than rely on that, treat the format as breaking and
require database recreation, consistent with Phase 10.

### Why before Phase 12

Phase 12 adds a per-cell metadata mask (one bit per column) and
per-cell `CellMetadata` for cells that carry TTL or WRITETIME. PK
and CK columns can never carry either. With them removed from the
blob, the cell-meta mask naturally only ranges over columns that can
have metadata; without this phase, Phase 12 would have to add an
explicit "skip PK/CK" carve-out in both the mask writer and the
INSERT/UPDATE handler when populating cell metadata.

---

## Phase 12 — TTL projection (`SELECT TTL(col)`, `WRITETIME(col)`)

**Impact: unblocks `testInsertWithTtl`, `testInsertWithDefaultTtl`,
`testUpdateWithTtl`, `testUpdateWithDefaultTtl`, `testMixedTTLOnColumns`,
`testMixedTTLOnColumnsWide`, `testTimestampTTL`, plus the UPDATE-side of the
Phase 10 deferral. Breaking on-disk format change — databases must be recreated.**

The Phase 10 row metadata is per-row; `TTL(col)` needs per-cell answers. The
remaining work splits into three layers: storage (per-cell metadata in the row
blob), write paths (INSERT / UPDATE USING TTL set cell metadata), and read paths
(parser + planner + evaluator + io for the `TTL()` / `WRITETIME()` selectors).

### 1. Per-cell metadata storage

```
struct CellMetadata {
    flags          : U8       // bit 0 = HAS_TTL, bit 1 = HAS_WRITETIME
    expiry_unix_ms : S64      // present iff HAS_TTL
    writetime_us   : S64      // present iff HAS_WRITETIME; absent for static columns
}

new row blob layout:
    [ row_meta (Phase 10, 9 bytes) ]
    [ col_count: U64 ]
    [ mask_words: U64 ... ]
    [ cell_meta_mask_words: U64 ... ]   // bit ci set ⇒ column ci has a CellMetadata
    [ for each ci with mask bit set:
        if cell_meta_mask bit set: CellMetadata
        column data
    ]
```

`cell_meta_mask` keeps the common "no per-cell metadata" case to one extra
U64 per 64 columns. PK / CK columns never carry metadata (their values live in
keys, not the blob). The row-level `row_meta` is kept as the default for any
column that has the present bit set but no cell-meta bit.

```
io.cppm additions:
    struct CellMetadata { flags: U8; expiry_unix_ms: S64; writetime_us: S64 }
    write_cell_metadata(w, m)
    read_cell_metadata(r) -> Task<CellMetadata>
    skip_cell_metadata(r, flags)         // for skip path when caller doesn't need it
```

### 2. Reader plumbing

`ColumnIterator` already exposes `metadata` for the row. Extend it to carry
per-cell metadata of the current column:

```
struct ColumnIterator:
    metadata: RowMetadata              // existing
    cell_meta_mask: DynamicArray<U64>  // new; loaded in load()
    current_cell_metadata: CellMetadata // populated by deref() when cell_meta bit set

    load(...):
        read row_meta                  // existing
        read col_count + masks          // existing
        read cell_meta_mask             // new

    deref():
        if cell_meta_mask bit set for current column:
            current_cell_metadata = co_await read_cell_metadata(...)
        else:
            current_cell_metadata = derive_from_row_metadata(metadata)
        return co_await read_column_value(...)

    advance():
        if current value not consumed and not skipped:
            if cell_meta_mask bit set: co_await skip_cell_metadata(...)
            co_await skip_column_value(...)
```

`read_row_into` extends to optionally return parallel `DynamicArray<CellMetadata>`
so engine-level consumers (`execute_select_index`, `execute_select_pk_in_ordered`,
the count loop) can answer `TTL(col)` without re-reading.

### 3. Writer plumbing

Update `write_row_blob`, the INSERT inline writer, and `rewrite_static` to take a
`DynamicArray<Optional<CellMetadata>>` parallel to `col_values` / `col_present`:

```
write_row(buf, row_meta, col_values, col_present, cell_meta):
    write_row_metadata(buf, row_meta)
    write_column_mask(buf, is_active, n)
    write_cell_meta_mask(buf, ci -> cell_meta[ci].has_value(), n)
    for ci in 0..n:
        if is_active(ci):
            if cell_meta[ci]: write_cell_metadata(buf, *cell_meta[ci])
            write_column_value(buf, col_values[ci], type[ci])
```

The INSERT/UPDATE handlers populate `cell_meta`:

```
INSERT USING TTL t USING TIMESTAMP ts ... VALUES (v1, v2, ...):
    ttl_ms       = resolve_using_ttl_ms(params) || tbl.default_ttl_ms
    writetime_us = resolve_using_timestamp_us(params) || os::unix_us_now()
    for each named column ci where evaluated value is not Unset/Null:
        cell_meta[ci] = CellMetadata{
            flags          = (ttl_ms > 0 ? HAS_TTL : 0) | HAS_WRITETIME,
            expiry_unix_ms = ttl_ms > 0 ? now_ms + ttl_ms : 0,
            writetime_us   = writetime_us,
        }

UPDATE USING TTL t SET col_i = v_i WHERE ...:
    read existing (col_values, cell_meta) via read_row_into
    for each assignment ci:
        col_values[ci] = v_i
        cell_meta[ci]  = CellMetadata{ttl/writetime as above}
    // cells NOT in the assignment list keep their old cell_meta — that's the
    // semantic the per-row approximation could not express. Lifts the
    // UPDATE USING TTL rejection.
    write_row(...)
```

UPDATE without USING TTL behaves the same but with `ttl_ms = 0` for updated
cells (Cassandra's "live forever" semantic).

DELETE remains row-level (no per-cell TTL needed).

### 4. Parser

The `Selector` variant already carries `Function { name; arguments: [Selector] }`.
The parser accepts `TTL(col)` and `WRITETIME(col)` today because `function_call`
is the same nonterminal as user-defined functions. No grammar change needed —
verify with a parser test and add cases if missing.

### 5. Planner + evaluator

```
SelectOp variant additions:
    struct TtlOf       { col_idx: U64 }
    struct WritetimeOf { col_idx: U64 }

ProjectionPlan.needed_cols:
    TtlOf / WritetimeOf add col_idx to needed_cols so the executor materializes
    the cell (the column-mask + cell_meta_mask answer comes for free).

plan_select(Selector::Function{"ttl", [arg]}):
    if arg is a single ColumnName resolving to a regular (non-PK/CK/static) column:
        push_back(ops, SelectOp::TtlOf{col_idx})
    else: return PlanError::InvalidTtlArgument
plan_select(Selector::Function{"writetime", [arg]}): symmetric
```

`evaluator` gains a new VirtualColumn type so `native.cpp::append_cql_value`
can render `TTL` (int, seconds remaining) and `WRITETIME` (bigint, µs). Both
return null when the column is null/absent or carries no metadata.

### 6. Engine projection

The two existing projection sites (native row loop + the per-row sites in
`execute_select_index`, `execute_select_pk_in_ordered`, aggregate count) build
the output row via `select_col_indices`. Replace that flat array with a
`DynamicArray<SelectOp>` so the loop can dispatch:

```
for op in projection.ops:
    match op:
        ColumnRef{ci}    -> output value[ci]
        TtlOf{ci}        -> if cell_meta[ci]?.flags & HAS_TTL:
                                output (cell_meta[ci].expiry_unix_ms - now_ms) / 1000
                            else: output null
        WritetimeOf{ci}  -> if cell_meta[ci]?.flags & HAS_WRITETIME:
                                output cell_meta[ci].writetime_us
                            else: output null
        CountStar        -> existing
```

`make_virtual_rows_shell` and `build_select_col_order` need parallel updates so
the result column metadata reports `TTL(col)` as `int` and `WRITETIME(col)` as
`bigint`, with the alias from `SelectColumn.as` when present.

### 7. Migration

Same approach as Phase 10: the on-disk row blob shape changes, so existing
databases will not load. No migration code — recreate the database. The Phase 10
`row_meta` field stays in place (it remains the default expiry when a column has
no cell-meta bit set).

### Test seam

After the storage + writer + reader pieces land, the SELECT side can be staged
behind the parser/planner work — a feature flag in the engine that returns
`Invalid` for `TTL()` / `WRITETIME()` until the projection wire-up is complete
keeps mustpass green during the multi-PR rollout.

---

## Smaller follow-ups

Independent items that can be picked up in any order.

- **UNSET in INSERT bind / collection compound RHS.** WHERE rejection landed; the
  remaining UNSET tests (`testSetWithUnsetValues`, `testMapWithUnsetValues`,
  `testListWithUnsetValues`) bind `UNSET_VALUE` to a `?` in an INSERT VALUES list or as
  the RHS of `UPDATE col = col + ?`. Cassandra treats this as "no-op for this column".
  Today the INSERT path returns `Invalid: incompatible literal` (because `Unset`
  fails the `can_cast_write_evaluated_as_column_value` check), and compound-op
  paths assert. Fix: in the INSERT NamesValues handler, skip a named column when its
  evaluated value is `Constant{Unset}`; in the planner's CollectionPatch construction,
  drop the patch when RHS evaluates to `Unset`.
- **Patch-supplied diff for collection-index maintenance.** `update_indexes` currently
  diffs old/new column values element-by-element (O(n+m)). For `CollectionPatch` ops the
  delta is already known (e.g. `c = c + {x}` → added = {x}). Plumb a `CollectionPatchDiff`
  from `apply_collection_patch` through `apply_mutation` so `update_indexes` consumes it
  directly when present. Pure perf optimization; correctness is already in place.
- **Variable-width DESC clustering columns.** `append_escaped_terminated` currently
  asserts `not_implemented` for DESC on `text`/`varchar`/`ascii`/`blob`/`hex`. Needs an
  inverted escape/terminator scheme so the encoded bytes still compare correctly under
  the byte-inversion-for-DESC convention. Touches `append_escaped_terminated` and the
  matching decode loops.
- **Tuple-equality WHERE expansion.** `WHERE (a, b) = (?, ?)` is not yet expanded into
  per-column equalities, so it does not compose with the CK-equality-skipping ORDER BY
  path. Blocks `testAllowSkippingEqualityAndSingleValueInRestrictedClusteringColumns`.
  Pure planner work: split the tuple in `build_row_locator` before populating
  `KeyConstraints`.
- **ALTER TABLE WITH options persistence.** Options like `min_index_interval` and
  `gc_grace_seconds` are parsed and silently dropped. Persist on `TableHeader` and
  surface in `system_schema.tables`. Required by tests that round-trip
  `ALTER TABLE … WITH …` and then `SELECT` the option back.
- **CREATE TABLE without keyspace name.** Currently returns `Invalid` with a generic
  message; the conformance tests grep for the specific phrase
  "no keyspace has been specified". Trivial copy-edit in the create-table handler.
- **`m[k] = v` in WHERE → Entries-index lookup.** Parser's `column_expression_relation`
  currently takes only `column_name` on the LHS, not a `SimpleSelection` with optional
  subscript. Once parsed, the planner already has the `Entries` index machinery (C2/C3)
  ready to consume a `key ++ value` composite prefix.

---

## Major future direction — token-based partition ordering

**Status: out of scope for the current single-node correctness pass; required for full
Cassandra-shape conformance.**

Cassandra hashes partition keys with Murmur3 and orders partitions by token, not by raw
PK bytes. plexdb currently orders by raw PK bytes inside the partition BTree, which is
correct for point lookups and PK ranges but produces a different cross-partition order
than Cassandra. Tests affected include `testIndexQueryWithCompositePartitionKey` and most
multi-partition paging tests that observe ordering.

Every BTree key comparison on the partition tree would swap from raw bytes to
`(token, pk_bytes)`; schema would have to record the partitioner choice; secondary-index
keys would need to embed the token of the pointed-to partition; and the `token(...)`
function plus `WHERE token(pk) > ?` syntax would need wiring through the planner. Doing
this before the phase work above is stable would churn every key-handling code path
without unblocking any of the larger feature gaps. Revisit once a concrete conformance
target makes the cost worthwhile.

---

## Out of scope (will not implement)

- **`CUSTOM INDEX ... USING '...'` (SASI/SAI) and per-index `WITH OPTIONS`.** SASI and SAI
  are Cassandra-internal index implementations whose on-disk format and query semantics
  are tied to specific JVM-side data structures. Replicating them duplicates the role of
  the built-in B-tree index plus the collection indexes already in place, while adding
  large surface area for a single test category. Return `Invalid` with a clear error
  message and refuse the statement.
- **Conditional BATCH and standalone LWT (`IF` on UPDATE / DELETE).** Compare-and-swap
  semantics modeled on Paxos consensus. The unconditional Phase 9 path covers every
  unblocked BATCH test; LWT only unblocks a small number of conformance tests that all
  depend on multi-replica semantics plexdb is not designed for. Returning
  `assert_not_implemented` is the agreed behavior; standalone LWT applies the same
  rationale to single-statement `UPDATE … IF` / `DELETE … IF`.
