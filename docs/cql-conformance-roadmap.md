# CQL Engine

## Minor gaps

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
- **Type-conversion built-ins (`blobAsBigint`, `bigintAsBlob`, and friends).** The
  planner accepts unknown SELECT functions only as a clean Invalid; conversion
  built-ins are unimplemented. Blocks `testTimestampTTL` which wraps `writetime(c)`
  in `blobAsBigint(bigintAsBlob(...))`. Pattern needed: register the `<type>As<type>`
  family in the planner, evaluate them in the projection layer. Same shape as TTL/
  WRITETIME projection — a new SelectOp variant plus dispatch in `project_row_via_ops`.


## Major features

### Token-based partition ordering

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

## Out of scope

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
