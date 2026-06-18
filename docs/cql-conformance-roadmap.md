# CQL Engine Roadmap

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

## Phase 8 — Collection DML

**Impact: ~22 unique tests in collections_test.py. Complex; isolated from other phases.**

Two independent sub-problems plus the rolled-in collection-index work.

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

**Sub-problem C: collection-column indexes** (~8 fires).
The current `CreateIndex` handler rejects collection columns ("cannot create index on
collection column"). Phase 8's per-element iteration is the same machinery needed to emit
an index entry per collection element, so collection indexes land here rather than in a
separate phase.

- Schema: `Index` gains an `IndexKind` enum (`Values` / `Keys` / `Entries` / `Full`)
  parsed from `CREATE INDEX ON tbl(values(col))`, `tbl(keys(col))`, `tbl(entries(col))`.
  Plain `tbl(col)` on a list/set defaults to `Values`; on a map it remains an explicit
  error (per Cassandra, map without `keys()`/`values()`/`entries()` is ambiguous).
- Index key format: `[len-prefixed element_bytes][pk_len][pk_bytes][ck_bytes]`. For map
  `Entries`, the element is `key_bytes ++ value_bytes`; for `Keys`, just the map key.
- Maintenance: when an indexed collection column changes, iterate the old element set,
  remove each `(element, pk, ck)` index entry, then iterate the new element set and
  insert. The diff approach (only emit the changed elements) is an optimization deferred
  to a later pass.
- WHERE handling: `col CONTAINS x` planner-translates to an index lookup against a
  `Values`-kind index on `col`; `col CONTAINS KEY x` against a `Keys` index; `col[k] = v`
  to an `Entries` index lookup with element = `k ++ v`.

Blocks: `testSetContainsWithIndex`, `testListContainsWithIndex`,
`testListContainsWithIndexAndFiltering`, `testMapKeyContainsWithIndex`,
`testMapValueContainsWithIndex`, `testFilterWithIndexForContains`,
`testQueryMultipleIndexTypes`, `testContainsKeyAndContainsWithIndexOnMapKey`,
`testContainsKeyAndContainsWithIndexOnMapValue`, `testIndexLookupWithClusteringPrefix`.

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

### Out of scope inside this phase — conditional BATCH (LWT)

`IF`-clauses on BATCH require compare-and-swap semantics that, in Cassandra, are built on
the Paxos consensus protocol across replicas. Single-node plexdb has no replica set, but
the test suite expects the *user-visible* CAS semantics (read-check-write atomicity with
`[applied]` result rows). Implementing this would require a separate CAS executor that
reads condition columns, evaluates the predicate, and either applies the batch or returns
the existing row — distinct from the unconditional path above and not on the critical
path for any unblocked test cluster. Mark with `assert_not_implemented`; revisit only
if a downstream user explicitly needs LWT.

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

### Deferred sub-task — background TTL sweep

Expiry enforcement at read time (filter expired rows in the iterator) is sufficient for
correctness; the row blob format change above is the only on-disk prerequisite. A
background sweep that emits tombstones is a separate optimization with trigger candidates
on open, after N mutations, or on explicit COMPACT. Schedule once read-time enforcement
is stable. The header laid down here already carries `expiry_unix_ms`, so the sweep can
land without another breaking change.

---

## Cross-phase planner & schema follow-ups

Smaller items that don't fit one of phases 6–10 but remain on the critical path for
specific conformance tests. Each is independent and can be picked up in any order.

- **Variable-width DESC clustering columns.** `append_escaped_terminated` currently
  asserts `not_implemented` for DESC on `text`/`varchar`/`ascii`/`blob`/`hex`. Needs an
  inverted escape/terminator scheme so the encoded bytes still compare correctly under
  the byte-inversion-for-DESC convention. Touches `append_escaped_terminated` and the
  matching decode loops. Isolated change; lives outside every phase because no upcoming
  feature work depends on it.
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

---

## Major future direction — token-based partition ordering

**Status: out of scope for the current single-node correctness pass; required for full
Cassandra-shape conformance.**

Cassandra hashes partition keys with Murmur3 and orders partitions by token, not by raw
PK bytes. plexdb currently orders by raw PK bytes inside the partition BTree, which is
correct for point lookups and PK ranges but produces a different cross-partition order
than Cassandra. Tests affected include `testIndexQueryWithCompositePartitionKey` and most
multi-partition paging tests that observe ordering.

Why it's out of scope for now: every BTree key comparison on the partition tree would
swap from raw bytes to `(token, pk_bytes)`; schema would have to record the partitioner
choice; secondary-index keys would need to embed the token of the pointed-to partition;
and the `token(...)` function plus `WHERE token(pk) > ?` syntax would need wiring through
the planner. Doing this before phases 6–10 are stable would churn every key-handling code
path without unblocking any of the larger feature gaps. Revisit once the phase work above
is complete and a concrete conformance target makes the cost worthwhile.

---

## Out of scope (will not implement)

- **`CUSTOM INDEX ... USING '...'` (SASI/SAI) and per-index `WITH OPTIONS`.** SASI and SAI
  are Cassandra-internal index implementations whose on-disk format and query semantics
  are tied to specific JVM-side data structures. Replicating them duplicates the role of
  the built-in B-tree index plus the collection indexes planned for Phase 8, while adding
  large surface area for a single test category. The agreed behavior is to return
  `Invalid` with a clear error message and refuse the statement.
- **Conditional BATCH and standalone LWT (`IF` on UPDATE / DELETE).** Compare-and-swap
  semantics modeled on Paxos consensus. The unconditional Phase 9 path covers every
  unblocked BATCH test; LWT only unblocks a small number of conformance tests that all
  depend on multi-replica semantics plexdb is not designed for. Returning
  `assert_not_implemented` is the agreed behavior; standalone LWT applies the same
  rationale to single-statement `UPDATE … IF` / `DELETE … IF`.
