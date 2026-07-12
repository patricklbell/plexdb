# CQL engine data flow

How `cql/engine/` transforms a parsed `Statement` into bytes on disk (DML) or
into a row stream (SELECT). The goal of this document is to make the dependency
edges explicit — what depends only on the schema, what depends only on the bind
values, what depends on per-row partition state — so we can pull the
schema-only work out of the hot path and identify which abstractions are
genuinely CQL-specific versus shareable with future protocols (SQL, RESP,
graph).

## 1. Layers and entry point

Module layering (top depends on bottom):

```
cql.parsers
   ↓ produces Statement (cql::engine::statements)
cql.engine
   ├── types          POD value types (Null, UUID, Blob, …) and type::Type
   ├── column_value   ColumnValue tagged union (basic + nested collections)
   ├── statements     parsed AST: Statement variant + WhereClause + Term + …
   ├── schema         on-disk schema (Keyspace / Table / Column / PartitionBTree)
   ├── system_schema  virtual system_schema.* tables
   ├── virtual_table  representation of non-storage-backed result rows
   ├── evaluator      Term → Evaluated; WhereClause → bool; builtin functions
   ├── key            Evaluated ↔ key bytes (lexicographic encoding)
   ├── io             Evaluated ↔ row-blob bytes (per-type serialization)
   ├── planner        Statement + schema → RowLocator / FilterPlan / MutationSpec
   ├── it             RowIterator: (PartitionBTree, ClusteringBTree) → ColumnRange
   └── engine         execute(): glue + transaction management
cql.native
```

`engine::execute(Engine&, const Statement&)` (engine.cpp:1439) is the single
entry point. It opens a `pager::Transaction`, dispatches on the
`Statement::value` tagged union inside `execute_inside_transaction`
(engine.cpp:812), and for everything except `SELECT` commits the transaction
before returning. For `SELECT` the transaction is moved into
`ExecutionResult::deferred_tx` so that row iteration (which uses the `own_tx`
borrowing pattern inside `RowIterator::advance`) always sees a stable active
transaction; the caller commits it after consuming all rows.

The statement variants split into four families:

| Family            | Variants                                                           | Path                                                                                                |
| ----------------- | ------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------- |
| DDL               | `CreateKeyspace`, `CreateTable`, `AlterTable`, `DropTable`, `TruncateTable`, `UseKeyspace`, … | Direct `schema::*` calls, no planner.                                                               |
| Query             | `Select`                                                            | `planner::plan_select` → `RowRange` → caller iterates.                                              |
| Single-row DML    | `Insert`                                                            | Path-specific (`engine.cpp:995`), bypasses the planner; serializes directly via `io` and `key`. |
| Multi-row DML     | `Update`, `Delete`                                                  | `planner::plan_update` / `planner::plan_delete` → `MutationSpec` → `apply_mutation`.                |

`Insert` is the odd one out — it has no `WHERE`, so it doesn't go through
`build_row_locator`. Every other DML statement converges on `apply_mutation`
(engine.cpp:570).

## 2. AST → planner intermediate

The transformation from parsed AST to executable plan is concentrated in
`planner.cpp` and produces three plan structs (planner.cppm):

- **`RowLocator`** — how to scope the operation in storage. Holds serialized
  PK bound bytes (`pk_begin`/`pk_end`), CK bound bytes (`ck_begin`/`ck_end`),
  inclusivity/partial flags, and pre-materialized lists for IN clauses
  (`pk_in_values`, `ck_in_values`). Every byte field here is the output of
  `key::serialize_*` and is directly usable as a BTree key.
- **`FilterPlan`** — `WhereClause::Relation` predicates that must be re-checked
  per-row, plus `needs_allow_filtering`. CK predicates and PK-IN go into
  `filter.predicates` for post-scan evaluation **without** raising the
  ALLOW FILTERING flag, since they are still selective; non-key predicates set
  the flag.
- **`ProjectionPlan` / `MutationSpec`** — what columns to emit (SELECT) or
  what assignments to apply (UPDATE/DELETE). Column references are resolved
  from names to `U64` indices into `Table::cols`, so the runtime never
  re-does name lookups.

`build_row_locator` (planner.cpp:48) is the workhorse:

1. Walk every `WhereClause::Relation`.
2. For each `ColumnExpressionRelation`, decide whether the column is a PK
   position, a CK position, or non-key — via linear searches over
   `tbl.partition_key_col_indices` / `tbl.clustering_key_col_indices`.
3. Collect EQ values into per-position slots (`pk_eq_vals`, `ck_eq_vals`),
   IN values into per-position lists (`pk_in_vals`, `ck_in_vals`), and CK
   range bounds into the locator fields.
4. After the walk, decide the locator's mode:
   - All PK positions filled by EQ → `pk_is_equality = true` and serialize
     the composite key once into `pk_begin`.
   - All PK positions covered by EQ or IN → expand the cartesian product
     into `pk_in_values` (one fully serialized key per combination). EQ
     predicates that participated also get pushed to `filter.predicates`
     so each enumerated partition is re-checked (cheap because the
     candidate row already matches the EQ position).
   - Partial CK prefix EQ → synthesize a prefix range bound with the
     `ck_*_is_partial` flag so the iterator and `apply_mutation` use prefix
     comparison instead of full-key comparison.

Crucially, **terms are evaluated at plan time, not at iteration time.** Each
`r.value` term goes through `evaluate(term, ctx)` which resolves bind markers,
runs arithmetic, and calls builtin functions (`uuid()`, `now()`, …). The
resulting `Evaluated` is fed into `key::serialize_*` so the locator carries
already-serialized bytes, not deferred expressions. The only re-evaluation per
row is `evaluator::evaluate_where(filter.predicates, ctx)` for the residual
filter — and there the `ctx.row_values` pointer is populated from the actual
row.

`plan_select` (planner.cpp:524) wraps `build_row_locator` and then resolves the
projection: each `Select::Selector` is walked, `ColumnName` → `col_idx`,
`Count{}` → `CountStar`. Aggregates set `is_aggregate`.

`plan_update` and `plan_delete` (planner.cpp:621, 733) reuse
`build_row_locator`, then:

1. Validate that mutation WHEREs are PK/CK only and use EQ/IN on the PK
   (validate_mutation_where).
2. Resolve each assignment / selection to a `col_idx`, evaluate the
   right-hand side once, and accumulate `ColumnUpdate{col_idx, Evaluated}` in
   `spec.updates`.
3. For UPDATE, also append PK and CK equality values into `spec.updates` so
   that `apply_mutation` can upsert: when a new partition is created, the
   key columns are populated from these injected entries rather than from a
   row read.

The output is therefore: a small POD struct (`MutationPlan`) carrying
serialized key bytes + a list of `(col_idx, Evaluated)`. Once built, planning
no longer needs the original `Statement` or `WhereClause`.

## 3. Schema dependency surface

The planner reads the following schema state and *nothing else*:

| Schema datum                            | Used by                                                                                                       |
| --------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `Table::cols[i].name`                   | Resolve `ColumnName` / `ColumnDefinition` → `col_idx`.                                                         |
| `Table::cols[i].type` (`type::Basic`)   | Choose key-encoding branch in `key::append_component`; choose value-encoding branch in `io::write_column_value`. |
| `Table::cols[i].is_static`              | Mutation validation (`StaticOnlyUpdateWithCK`, `StaticOnlyDeleteWithCK`); INSERT path decides static vs row blob. |
| `Table::cols[i].tombstone`              | Skip dropped columns during name resolution.                                                                  |
| `Table::partition_key_col_indices`      | `find_pk_position`; composite vs single PK; key serialization order.                                          |
| `Table::clustering_key_col_indices`     | `find_ck_position`; composite vs single CK; prefix-key support.                                               |
| `Table::static_col_indices`             | INSERT and `rewrite_static`: decide which columns go to the static blob.                                      |
| `schema::has_clustering_keys(tbl)`      | Branch between single-level and two-level BTree paths everywhere.                                             |

This is the *complete* contract. The planner reads no per-row state and opens
no transactions. `build_row_locator(where, tbl, ctx)` and `plan_select` are
pure functions of `(WhereClause, Table, EvalContext)`, where `EvalContext`
itself depends only on bind variables. That is the natural boundary for
caching: anything that can be derived from `(query AST, schema snapshot)`
without bind variables can be cached, and what depends on bind values can be
patched in at execute time. See §6.

The two-level BTree (`PartitionBTree` → `PartitionEntry` → `ClusteringBTree`)
is purely a property of the schema — `has_clustering_keys(tbl)` decides which
shape. The planner only emits a locator; the *interpretation* of that locator
against the BTree shape lives in `it.cpp` and the apply_mutation branches.

## 4. Per-row execution

This is where the partition BTree and clustering BTree finally enter the
picture. There are two execution paths, both downstream of the planner.

### 4.1 SELECT path — `it::RowIterator`

`create_table_range_it` (engine.cpp:504) builds a `RowRange{start, stop}`
from the locator:

1. **Partition positioning.** Based on `pk_is_equality`, `pk_has_begin`,
   `pk_has_end`, `pk_begin_inclusive`, `pk_end_inclusive` it calls one of
   `create_table_{begin,eq,lt,le,gt,ge}_it`. These open the partition BTree
   iterator with the corresponding `btree::SearchStrategy` and, if the table
   has clustering keys, also call `create_clustering` to set up the inner
   `ClusteringBTree` for the first partition.
2. **CK bounds.** If the locator has any CK bound, the start iterator's
   `ck_*` fields are populated from the locator and
   `apply_ck_bounds_on_clustering` re-positions `clustering_it` /
   `clustering_end_it` using `setup_clustering_for_partition`.
   `advance_past_empty_ck_partitions` then skips leading partitions where
   the CK range is empty so the first `*row_it` is immediately valid.

Iteration mechanics, all in `it.cpp`:

- `RowIterator::deref()` returns a `ColumnRange` of a freshly loaded
  `ColumnIterator` over the row blob and (if present) static blob. For
  static-only rows it injects partition-key column values by deserializing
  the partition key bytes via `key::deserialize_partition`. The column
  iterator pre-loads all mask words upfront (small) so subsequent reads can
  open their own short transactions.
- `RowIterator::advance(stop)` advances within the current partition's
  clustering BTree until exhausted, then re-runs
  `setup_clustering_for_partition` for the next partition — looping past
  empty partitions until the next valid row or `stop`. Honors partial CK
  prefix end-bounds via byte-level memcmp.
- `ColumnIterator::deref/advance` reads (or skips, using fixed-width
  arithmetic where possible) one column value from the row or static blob,
  routed by `Column::is_static` and a mask-bit check
  (`current_is_null`/`current_is_static`).

The residual filter is applied by the *caller* (`cql/native/native.cpp`
consumes `ExecutionResult` and runs `evaluator::evaluate_where` against each
row before emitting it on the wire). The engine doesn't filter inside the
iterator.

### 4.2 DML path — `apply_mutation`

`apply_mutation` (engine.cpp:570) takes the planner's `RowLocator` + `MutationSpec` and
the schema and produces BTree/blob mutations. For UPDATE/DELETE, the engine
loops over `locator.pk_in_values` and `locator.ck_in_values` to handle IN by
calling `apply_mutation` once per fully-bound (PK, CK) combination (engine.cpp:1266-1287
for UPDATE, 1318-1339 for DELETE), so by the time `apply_mutation` runs,
`pk_is_equality` is true and `pk_begin` holds a single concrete partition key.

The branching structure mirrors §3's schema dependency:

```
has_clustering_keys?
├── yes
│   ├── is_full_delete           → walk CK range, remove rows + (optional) static blob, drop partition entry if empty
│   ├── !ck_is_equality (static-only UPDATE)
│   │                            → read static blob → apply_updates_to_row → rewrite_static
│   └── ck_is_equality (UPDATE or column-DELETE row)
│                                → read existing row blob (if any) → inject missing PK col vals
│                                  → apply_updates_to_row → write new row blob → reinsert in clustering BTree
│                                  → optionally rewrite_static
└── no (single-level)
    ├── is_full_delete           → remove row blob, drop partition entry
    └── else                     → read existing row blob → inject PK → apply → write new → reinsert
```

`apply_updates_to_row` walks `MutationSpec::updates`, and for each one either
clears the column (Null) or feeds the `Evaluated` through `io::write_evaluated_*`
+ `io::read_column_value` to coerce into a `ColumnValue` of the column's
declared `type::Type`. The cast is content-driven by `type::Basic`.

INSERT (engine.cpp:1024+) is the path that bypasses the planner. It still
relies on the same schema-derived facts (PK/CK indices, static indices,
`has_clustering_keys`) and on the same `io::write_column_*` /
`key::compute_partition_token_from_evals` / `encode_clustering` primitives — it just
inlines them.

## 5. Lifecycle of an Evaluated

`Evaluated` is the engine-internal value currency, distinct from the AST's
`Term` and from the row's `ColumnValue`. Translation goes:

```
Term  ──evaluate(term, ctx)──►  Evaluated  ──key::append_component──►  key bytes
                                          └──io::write_evaluated_as_column_value──►  blob bytes
                                          └──io::can_write_evaluated_*──►  validation

ColumnValue (from a row read) ──wrapped into Evaluated by lookup_column_value──► used by evaluate_where
```

`Evaluated` carries both `Constant` (plan-time, from bind/literal) and
`ColumnValue` (run-time, from a row). That dual identity is what lets the
same `evaluate_*` and `compare_evaluated` work for both bind values and row
values in `evaluator.cpp:evaluate_where`.

`EvalContext` (evaluator.cppm:17) bundles the two pieces of per-execution
state: positional/named bind values and the current row's columns + table
pointer. The planner uses only the bind half; per-row filtering uses both.

## 6. Caching boundary — what could be prepared once

Given the analysis above, a prepared statement could carry forward
*everything that is pure in (Statement, Schema)*:

- Column-name → `col_idx` resolution (already partly done by `planner::SelectOp::ColumnRef`).
- PK/CK position assignment for every WHERE relation.
- For each WHERE relation, the **role** in the locator (PK EQ slot N,
  PK IN slot N, CK range bound, CK partial prefix, residual filter,
  ALLOW FILTERING needed) — i.e. essentially a `RowLocator` skeleton
  whose `*_bytes` fields are replaced by `(eval-recipe, type::Basic)`
  pairs. At execute time, evaluate the residual `Term`s (which can only
  contain bind markers / functions / arithmetic) and run
  `key::append_component` to materialize the bytes.
- Whether the result needs cartesian expansion (`pk_has_in`,
  `ck_has_in`) and the EQ-row predicates that must be pushed to filter
  in the IN case.
- The full `ProjectionPlan` for SELECT.
- For UPDATE/DELETE: the resolved `col_idx` list and which assignments
  inject PK/CK upsert values.
- Schema validation results (`PlanError`).

What cannot be precomputed:

- Actual bind values, including IN list sizes (so the *count* of
  serialized PK keys cannot be precomputed, but the *recipe* can).
- Result of non-deterministic builtins (`uuid()`, `now()`) — these must
  re-fire per execution.
- All actual BTree positions (`it.partition_it`, `clustering_it`) and
  any `pager::Transaction`.

The current cache (`engine.cppm`'s `PreparedEntry`) only stores the query
string, bind variable specs, keyspace/table, and a hint `pk_index` for
INSERT routing. `engine::execute(prepared_id, …)` literally re-parses the
query string each time (engine.cpp:1859). Moving the planner output into
`PreparedEntry` — keyed by `(query_hash, schema_version)` so it auto-
invalidates on DDL — is the next step. The planner is already structured to
support this: `build_row_locator` is a pure function modulo bind values,
and the parts that depend on bind values are all single-pass evaluations
followed by `key::serialize_*`.

Schema versioning hook: today schema mutations don't bump a version, but the
on-disk schema is small and a monotonic `Schema::version` could be set inside
`schema::create_*` / `delete_*`. Anyone with a stale `schema_version` on a
cached plan re-plans.

## 7. Shared vs CQL-only abstractions

Looking at the layers in §1 in light of multi-protocol support:

**Genuinely CQL-only (stay in `cql::`):**

- `cql::engine::statements` — the AST shape *is* CQL's grammar.
- `cql::Engine::prepared_cache`, `BindVariableSpec` — semantics mirror the
  CQL native protocol's prepared-statement model.
- `system_schema` / `virtual_table` — Cassandra-specific virtual tables
  that drivers query for cluster metadata.
- `evaluator`'s builtin function registry (`uuid()`, `now()`, `mintimeuuid()`,
  `textasblob()`, …) — Cassandra function names and semantics.
- `key`'s composite-key escape encoding — this is the CQL physical layout
  contract; other protocols will want their own. (The *idea* of
  lexicographic-comparable encodings is reusable; the layout is not.)

**Shareable (candidates for promotion to `plexdb::query::*` or similar):**

- The locator/filter/projection split — `RowLocator`, `FilterPlan`,
  `ProjectionPlan`, `MutationSpec` are a generic "physical plan" shape
  for any range-scannable key-value store with a two-level partition/cluster
  layout. SQL wouldn't have the PK/CK distinction but would have analogous
  index-prefix bounds.
- `it::RowIterator` over `(PartitionBTree, ClusteringBTree)` — the
  partitioning is a plexdb storage feature, not a CQL feature. The
  iterator's *interface* (`deref` → ColumnRange, advance, advance_partition)
  is protocol-agnostic.
- `io::Reader` / `io::Writer` / `io::read_column_value` / `write_column_value`
  is parameterized on `type::Basic` — that type set is currently CQL-flavored
  (timeuuid, varint, inet, …) but the IO mechanics (mask words, column
  count header, length-prefixed varlen) are not.
- `Evaluated` and `EvalContext`'s row-side interface (column lookup,
  comparison, where evaluation) — the comparison/where evaluator is
  generic; only the function registry is CQL-specific.

**Lives in a strange middle ground:**

- `key` is half-shareable: the per-type append/decode functions follow the
  `type::Basic` taxonomy (CQL), but the composite-key escaping discipline is
  general. If a future protocol uses its own type taxonomy, `key` would
  fork on the basic-type switch but reuse the framing helpers
  (`append_escaped_terminated`, the float/int sign-bit XOR tricks).

A natural refactor target: split the planner into a CQL front
(`Statement` → generic `PhysicalPlan`) and a generic back
(`PhysicalPlan` → execution). Today the planner already does roughly this
split, since `RowLocator` etc. don't reference `Statement` types after
`build_row_locator` returns — only `WhereClause::Relation` lingers in
`FilterPlan::predicates`. Replacing that with a generic `Predicate` IR is
the missing piece.

## 8. Verifiability notes

A few invariants are currently enforced by structure rather than by tests
or assertions, and would benefit from being made explicit:

- The locator's serialized bytes (`pk_begin`, `ck_begin`, IN entries) must
  always be byte-compatible with what the BTree stores for the same Table.
  This is upheld by *both* the locator and INSERT going through
  `key::serialize_*`, but there's no central assertion. Suggest a
  `key::verify_round_trip` (serialize then deserialize, compare to input)
  used in debug builds.
- `MutationSpec::updates` may contain duplicate `col_idx` entries (planner
  for UPDATE appends PK/CK equality values *after* user assignments).
  `apply_updates_to_row` applies them in order, so PK/CK injection wins
  over an explicit assignment with the same col_idx. This is fine for
  WHERE-derived values (they should be equal anyway) but worth a
  `@note` in `MutationSpec`.
- `RowIterator::clustering_btree` is held by value with iterators into it
  (it.cppm:178 — `fix_clustering_btree_ptr`). Any planner-level cache
  that materializes a `RowIterator` directly must respect this — better,
  the cache stays at the locator level and the iterator is built fresh
  per execute.

## Appendix — quick reference

Function dispatch table for the execute path (engine.cpp):

| Source line | Function                              | Purpose                                                                 |
| ----------- | ------------------------------------- | ----------------------------------------------------------------------- |
| 812         | `execute_inside_transaction`          | Statement variant dispatch.                                             |
| 504         | `create_table_range_it`               | Locator → `RowRange` for SELECT.                                        |
| 570         | `apply_mutation`                      | Locator + spec → BTree/blob mutations for UPDATE/DELETE (and one of the INSERT branches). |
| 374         | `read_row_into`                       | `(row_page, static_page)` → parallel col_values/col_present arrays.    |
| 398         | `write_row_blob`                      | Inverse of `read_row_into` for the row blob only.                       |
| 463         | `rewrite_static`                      | Replace the static blob for a partition entry.                          |
| 423         | `apply_updates_to_row`                | Apply `MutationSpec` to col_values/col_present in place.                |

Planner function table (planner.cpp):

| Source line | Function              | Inputs                                  | Output                                  |
| ----------- | --------------------- | --------------------------------------- | --------------------------------------- |
| 48          | `build_row_locator`   | `WhereClause`, `Table`, `EvalContext`   | `(RowLocator, FilterPlan)`              |
| 524         | `plan_select`         | `Select`, `Table`, `EvalContext`        | `SelectPlan`                            |
| 621         | `plan_update`         | `Update`, `Table`, `EvalContext`        | `MutationPlan`                          |
| 733         | `plan_delete`         | `Delete`, `Table`, `EvalContext`        | `MutationPlan`                          |
| 580         | `validate_mutation_where` | `WhereClause`, `Table`              | `PlanResult`                            |
