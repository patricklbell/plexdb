# CQL Engine

## Minor gaps

- **INSERT semantics: cell-level merge for unspecified columns.** plexdb's INSERT on a
  table without clustering keys overwrites the entire row blob with only the named
  columns set, so any column omitted from the VALUES list (or bound with `UNSET`) is
  cleared instead of preserved. Cassandra writes per-cell, so unspecified columns retain
  their existing values. Blocks `testSetWithUnsetValues`, `testMapWithUnsetValues`,
  `testListWithUnsetValues` (the UNSET literal itself is now handled — INSERT NamesValues
  validation skips UNSET non-key columns, planner drops `CollectionPatch` on UNSET RHS,
  and the native protocol decodes the -2 length sentinel as `Constant{Unset}` for
  collection types — but the second INSERT in those tests overwrites the previously
  inserted set/map/list to NULL). Fix: read the existing row before writing, merge
  named-column writes over the existing cell mask, then rewrite. The clustering-key path
  at `engine.cpp` already does this through `apply_updates_to_row`; the non-clustering
  branch needs the same read-modify-write.

- **Single-bind tuple WHERE RHS.** Parser's `tuple_expression_relation` requires `(...)`
  on the RHS, so `WHERE (a, b) = ?` (with the driver sending a single tuple-typed bind)
  fails with `SyntaxException`. The parenthesized form `WHERE (a, b) = (?, ?)` works
  (locator populated via the existing tuple-eq handler, filter narrows via the new
  per-column equality in `evaluate_where`). Inequality on a parenthesized tuple
  (`(a, b) > (?, ?)`) reaches the planner but silently no-ops in the filter; the
  remaining `testAllowSkippingEqualityAndSingleValueInRestrictedClusteringColumns` cases
  expect proper Invalid errors. Fix: widen the parser RHS to also accept a bare term;
  lexicographic compare in the planner for inequality, with the ORDER BY-composition
  check producing the existing error.

- **`m[k] = v` in WHERE → Entries-index lookup.** Parser's `column_expression_relation`
  takes only `column_name` on the LHS, not a `SimpleSelection` with optional subscript.
  The planner's `try_capture_collection_index` currently handles only `Operator::contains`
  and `Operator::contains_key` against `Values`/`Keys` indexes; the `Entries` index path
  for `key ++ value` composite prefixes is not wired up. Both layers need work: parser
  to accept subscripted LHS as a new `WhereClause` variant, planner to use the new
  Entries case in `try_capture_collection_index`.

## Performance

- **Patch-supplied diff for collection-index maintenance.** `update_indexes` diffs
  old/new column values element-by-element (O(n+m) on the nested loop in
  `engine.cpp`). For `CollectionPatch` ops the delta is known at construction time
  (e.g. `c = c + {x}` → added = `{x}`). Plumbing a `CollectionPatchDiff` from
  `apply_collection_patch` through `apply_mutation` would let `update_indexes` consume
  the delta directly when present. Pure perf — correctness is already in place via
  the diffing fallback, which must remain for the `TermWithIdentifiers` branch that
  doesn't go through `CollectionPatch`.

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
