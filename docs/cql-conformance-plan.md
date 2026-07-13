## CQL conformance plan — architectural redesign

Companion to [`cql-conformance-status.md`](cql-conformance-status.md); that
document enumerates the concrete failures and hacks (§H1–§H14). This one
names the architectural redesigns that delete the hacks as a *class*.

The `cql/engine` + `cql/native` tree is ~17,500 LOC (including generated
code) serving 313 tests — down from 20,489 as R2's value/key unification and
a general cleanup landed. Two cross-cutting structural problems remain and
produce most of what's left in §H1–§H14:

1. **The AST is the runtime representation.** `execute_inside_transaction` in `engine.cpp` is still one function dispatching on statement kind via `if constexpr (SameAs<T, X>)`. Prepared statements are not a runtime artefact — `PreparedEntry` stores the raw query string (`engine.cppm:38`), and every `EXECUTE` re-parses it (`engine.cpp:4647`, `engine.cpp:4661`, `native.cpp:1162`). Type dispatch via `type_matches_tag<...>` chains is down from ~205/~87 sites to **~96 in `engine.cpp` / ~47 in `planner.cpp`** (up slightly from a session of bugfixes adding call sites, not a regression in the pattern) — the shape is still unchanged. This is R1.
2. **Values were materialised even when only copied — now mostly fixed.** Storage, wire, and key formats used to be three different byte layouts, forcing a `ColumnValue` round-trip for plain projection or exact-type writes. R2 (below) has landed the bulk of the fix: a generated per-type wire/storage codec and a type-aware clustering comparator that both operate on encoded bytes directly.

Beyond these, 26 `assert_not_implemented` sites remain (down from 34) for
type-completeness gaps — each a runtime crash on a collection/tuple/UDT
shape or an unencodable native-protocol arm (§H8, §H10; `native.cppm`/`native.cpp`
wire arms, `column_value.cpp:275`, `schema.cppm:229/233`, `evaluator.cpp:794`,
`engine.cpp` ×8, `generated/token_wire.cpp:143`). This was a fair shortcut
during initial bringup; the engine is now mature enough that a new one is a
bug. R3 bans the pattern going forward and replaces it with compile-time
exhaustiveness.

| # | Redesign | Status | Retires |
|---|----------|--------|---------|
| R2 | One serialization for wire/value/key; type-aware clustering comparator; generated per-type codec | **Landed** (see below) | §H2 (empty-union asserts), §H12 partial, 5 `key.cppm` asserts |
| R1 | AST → typed physical plan tree; walker replaces the dispatch executor | Not started | §H2, §H4, §H14, `type_matches_tag` chains, prepared-statement reparse |
| R3 | Collapse `ast::Type`/`type::Type` duplication; drive `assert_not_implemented` to 0 | Not started (26 sites remain) | §H8, §H10, 26 `assert_not_implemented` |
| R5 | Native protocol as a declarative message spec, generated code committed | **Proved out, partial** — value/token wire codec is now generated (`cql/engine/codegen/`); message framing (`native.cpp`/`native.cppm`, opcodes/frames) is still hand-coded | §H1 (partial, done), §H10 (native message layer, open) |
| R7 | Structured errors + Cassandra-source-extracted message strings | Not started (~21 inline `create_*_error` factories remain) | §H1, remaining §H4 |
| R8 | Frozen collections: real key-encodability instead of a crash | **Retargeted** — §H13, this redesign's other original motivator, is already fixed by reordering `create_table`; see "Frozen collections" below for the current scope (§H6 plus two related equality/validation gaps) | §H6, part of §H4, part of §H14 |
| R9 | Cell-writetime, dropped-column, and row-liveness tracking | Not started; scope grew this session (row marker, range-tombstone reconciliation — see below) | §H9 |

Out-of-scope: standalone LWT and Conditional BATCH (multi-node), documented
in `TODO.md`. Standalone (non-BATCH) LWT statements now return a clean
`Invalid` (`engine.cpp`, `"INSERT ... IF NOT EXISTS is not supported"` etc.)
rather than hitting `assert_true_not_implemented`, independent of whether
LWT itself is ever implemented.

---

### R2 — One serialization for wire/value/key (Landed)

**Original problem.** A value passed through storage (host-native
little-endian), wire (CQL native, big-endian), and key (order-preserving
bytes) — three different byte layouts for the same logical value, forcing a
`ColumnValue` materialisation even for plain projection or an exact-type
write.

**What landed.**
- `cql/engine/io.codec.cppm` + `cql/engine/generated/io_codec_wire.cpp{,m}` — a per-type wire/storage codec generated from `cql/engine/codegen/wire_types.json` via `cql/engine/codegen/generate.py` and Jinja templates (`codegen/templates/`). Adding a type is a JSON entry, not hand-written encode/decode arms.
- `cql/engine/generated/token_wire.cpp{,m}` — same generation approach for the partition-token wire encoding.
- `cql/engine/clustering_compare.cppm` — `clustering_compare::ClusteringKeyComparator`, a type-aware comparator (`Sort` direction × `type::Basic` per column) that reproduces Cassandra's `AbstractType.compare` instead of relying on order-preserving byte encoding. The clustering BTree (`schema.cppm:74-75`, `ClusteringKeyPolicy = VarlenKeyPolicy<..., ClusteringKeyComparator>`) uses it directly.
- `cql/engine/key.cppm` was rewritten in terms of `io.codec` (self-delimiting component encoding) instead of a bespoke order-preserving codec — the 5 `key.cppm` `assert_not_implemented` sites from the old codec are gone.
- **Partition BTree is token-only.** `PartitionBTree = BTreePaged<FixedKeyPolicy<S64>, FixedValuePolicy<sizeof(PartitionEntry)>>` (`schema.cppm:61-71`) — the key is the bare Murmur3 token, no partition-key bytes stored redundantly in the BTree at all.
- `cql/engine/io.cppm` is now a 4-line re-export of `io.codec` + `io.evaluator` — the old monolithic `io.cpp` (source of the `writing integer value to this dtype` abort) is gone.
- Row read and read-modify-write paths are unified across INSERT and UPDATE (commit `dc6713a`).

**Open/unverified.**
- The plan's zero-allocation-projection benchmark claim has not been measured; no benchmark exists yet to confirm projection of unfiltered columns does zero `ColumnValue` allocations.
- §H12 (the `Null{}` guard in `drain_columns`/`project_virtual_row`) is now centralized into one `column_value_or_null(cv, present)` helper both call — but this is a call-site convenience, not the type-level fix R2 originally envisioned (`ColumnValue{}` itself defaulting to `Null`). That would require either changing `TaggedUnion`'s generic default-index behavior (shared by every other tagged union in the codebase) or turning `ColumnValue` from a type alias into a wrapper struct (breaks `get<T>`/`type_matches_tag<T>`/`visit` call sites that assume it *is* a `TaggedUnion`, spanning hundreds of sites) — real scope for a future pass, not attempted here.

---

### R1 — Typed physical plan tree; delete the AST-walking executor

**Status: not started.** `engine.cpp` is ~4668 lines. `execute_inside_transaction` is still one function with a `type_matches_tag<...>`-chain dispatch per statement kind (~96 sites in `engine.cpp`, ~47 in `planner.cpp` — down from ~205/~87, but the shape hasn't changed). `PreparedEntry` (`engine.cppm:38`) stores `query_string`, not a plan; every `EXECUTE` re-parses (`engine.cpp:4647`, `:4661`, `native.cpp:1162`).

**Redesign.** Move to a typed physical plan tree — the Postgres/DuckDB/Umbra
shape:

```
PlanNode = tagged variant over kinds:
    Access:    SeqScan | IndexScan | PartitionScan | PointLookup | StaticRowRead
    Transform: Filter | Project | ProjectExpr
    Aggregate: AggregateAll | GroupBy | CountStar
    Order:     Sort | PerPartitionLimit | Limit
    Set:       Union | IntersectByPk
    Mutation:  InsertRow | UpdateRow | DeleteRow | DeleteRange
             | DeletePartition | BatchApply
    DDL:       CreateTable | DropTable | AlterTable | CreateType | ...

  each node carries:
    - kind tag
    - fixed-size child pointer array
    - kind-specific payload (POD)
```

Expressions get their own tree with the same shape (`ExprNode` — column ref,
bind ref, constant, binary op, function call, TTL, writetime, cast). The
planner produces the tree; the walker dispatches on the kind tag,
exhaustively (compile-time-enforced — a missing arm is a build error).

`PreparedEntry` stores the plan tree, not the query string. `EXECUTE` fills
bind slots and enters the walker — no reparse, no re-plan. Ad-hoc `QUERY`
plans once per call and enters the same walker.

**Why plan tree over bytecode.** Postgres, DuckDB, Umbra all interpret typed
plan trees; SQLite's VDBE bytecode is the outlier and pays for it in debug
UX. A typed tree keeps the walker readable, `EXPLAIN` becomes a tree print,
and peephole optimisation is a tree → tree rewrite. The store is pager-I/O
bound, not dispatch bound, so bytecode's small constant-factor win doesn't
matter here.

**JIT (future, non-blocking).** The plan tree admits a straightforward LLVM
lowering later if dispatch ever becomes the bottleneck. Nothing here assumes
it.

**Deletes.** Every `type_matches_tag<...>` chain in `engine.cpp` (~96 sites)
and `planner.cpp` (~47 sites); `evaluator.cpp`'s `apply_operator` (§H2),
`evaluate_contains`, `evaluate_where`; the AST-visitor bind-numbering pass
(§H2 `evaluator.cpp:1094`); the re-parse at `engine.cpp:4647/4661`,
`native.cpp:1162`.

**Adds.** ~700 LOC: `PlanNode`/`ExprNode` definitions, walker, planner emit
helpers.

**Invariants.** Every valid CQL statement compiles to a plan tree whose
walker covers every `Kind` — `assert_not_implemented` in the walker is
banned; a missing case is a compile error (`-Werror=switch-enum`). Bind-marker
numbering is a plan-time property assigned once by the planner.

**Verification.** For every passing test today, the walker run produces
byte-equal frames vs. the current executor. Property test: `plan(parse(s))`
is idempotent.

---

### R3 — Collapse the `ast::Type`/`type::Type` duplication; drive `assert_not_implemented` to 0

**Status: not started.** `type::Type` (`types.cppm:243`) and `ast::Type`
(`types.cppm:388`) remain parallel trees differing only in UDT reference
(handle vs. textual name). 26 `assert_not_implemented` sites remain (down
from 34 — R2 retired the 5 in the old `key.cppm` codec as predicted).

**Redesign.**
1. Optionally parameterise the type tree as `Type<UdtRep>` — `Type<UdtName>` from the parser, `Type<UdtHandle>` in the engine, `resolve(Type<UdtName>, Schema) -> Type<UdtHandle>` a `visit()` walk. Worth doing only if the shared template ends up smaller and clearer than the two structs.
2. Drive the remaining 26 sites to 0 by implementing each case. Concentrated in `native.cpp`/`native.cppm` (10 sites — hex/varint/decimal/duration/nested-collection/vector-collection wire arms), `engine.cpp` (8), `schema.cppm` (2), plus one each in `column_value.cpp`, `evaluator.cpp`, `generated/token_wire.cpp`.

Sites that legitimately handle only `type::Basic` (the wire type-code table)
keep their `switch`, built with `-Werror=switch-enum`. New type-completeness
gaps must implement the case, not `assert_not_implemented`.

**Invariants.** No `assert_not_implemented` for type completeness exists
after R3 lands, and CI grep-gates against new ones.

**Verification.** Property tests over `all_types()` — including every
collection/tuple/UDT shape that currently asserts: `wire_encode ∘
wire_decode` is identity; `key_encode`/comparator ordering matches
Cassandra's; `wire_type_string` matches the driver's captured type name.

---

### R5 — Native protocol as a declarative message spec (proved out for value codecs; message framing still open)

**Status: partial.** The declarative-spec-plus-generated-code pattern this
redesign proposed is now proven in production for the *value* layer:
`cql/engine/codegen/wire_types.json` + `generate.py` + Jinja templates
produce `generated/io_codec_wire.cpp{,m}` and `generated/token_wire.cpp{,m}`,
committed to the repo. That work retired the old `io.cpp` monolith and its
crash (`writing integer value to this dtype is not implemented`).

What has **not** moved to this pattern: `native.cpp` (1264 LOC) +
`native.cppm` (705 LOC) still hand-code every CQL *message* shape (STARTUP,
QUERY, RESULT, ERROR frames — opcodes and field layout) as imperative
byte-appending, essentially unchanged in size from before R2. Encoder gaps
still appear as `assert_not_implemented` at 10+ sites (§H10), and the
duration arm (`native.cppm:394`) is no longer theoretical — it's the one
crash the conformance suite hits today (`testFilteringOnDurationColumn`).

**Remaining redesign.** Extend the same spec/generate pattern to message
framing:

```
MessageSpec:
    opcode
    direction      = request | response
    min_version    = v4 | v5
    fields         = ordered list of Field

Field:
    name
    shape          = [byte] | [short] | [int] | [string] | [long string]
                   | [bytes] | [inet] | [value] | [option]<T>
                   | [list]<T> | [map]<K, V>
    presence       = required | optional(predicate)
```

Two walkers — `encode(spec, MessageValue) → Bytes` and `decode(spec, Bytes)
→ MessageValue` — parameterised by the spec, generated the same way the
value codec is today.

**Deletes (once done).** ~1,400 LOC of imperative encoders/decoders in
`native.cpp`/`native.cppm`.

**Invariants.** The response frame's header length equals the encoded body's
byte length by construction (header written last, from the actual byte
count) — this makes a truncated-frame bug (the class §H7 used to be, now
fixed) structurally impossible going forward. No file outside the message
module constructs response bytes. `assert_not_implemented` in the protocol
layer becomes impossible: an incomplete spec is a build-time error.

**Verification.** Golden vectors per message spec, captured from a real
Cassandra frame, diffed byte-for-byte. Fuzzing: `decode(encode(v)) == v`.
Regeneration is idempotent; CI runs it and checks the tree is clean.

---

### R7 — Structured errors + Cassandra-source-extracted message strings

**Status: not started.** ~22 `create_*_error`-style factories remain in
`engine.cpp`, building message strings by inline concatenation. `PlanError`
is still used 72 times across `planner.cpp`/`planner.cppm`, repurposed for
several distinct upstream error shapes.

**Redesign.** Two coordinated pieces:

1. **Structured `CqlError` with typed payloads** — a tagged variant
   (`UnknownFunction`, `CounterOnNonCounter`, `AllowFilteringRequired`,
   `OrderByNonClustering`, `UdtInPkAlterAddField`, `IncompatibleLiteral`,
   `ParseFailure{ParseErrorKind, line, column, near_token}`, …) with one
   `format_wire_message(err, arena) → String8` per kind. Grammar-typed parser
   errors carry a `ParseErrorKind` tag instead of leaking the lexy rule name
   — parser rule-name leakage (§H1) no longer occurs at all (the one
   remaining case turned out to be a real parser bug, not a message-fidelity
   issue — see §H5), but a structured `ParseFailure` payload keeps it that
   way going forward instead of relying on every grammar production to
   avoid the pattern by convention.
2. **Cassandra-extracted message strings** — `tools/extract_cassandra_errors.py`
   scans Cassandra's Java source for the exact wire-format string per error
   kind, committed to `cql/engine/error_messages.cppm`.

**Deletes.** ~22 `create_*_error` factories; the `PlanError` enum (subsumed
by `CqlError::Kind`); inline `AutoString8 + ...` message construction.

**Invariants.** No file outside `errors.cpp` constructs an error wire
message. Every `CqlError::Kind` has exactly one template row and one
golden-string test vs. upstream. Where plexdb's message is more informative
than Cassandra's, the fixture documents the divergence explicitly rather
than widening the regex silently.

**Verification.** For every Cassandra error string in the test suite,
`format_wire_message(err)` produces the exact same bytes — the remaining
§H1 regex-mismatch failures (`testAdderNonCounter`, `testAlterTypeUsedInPartitionKey`,
`testAlias`, `testAllowFilteringOnPartitionKeyWithCounters`, `testBatchWithInRestriction`)
become string-equality passes.

---

### R8 — Frozen collections: real key-encodability instead of a crash

**Status: retargeted.** This redesign started as "schema mutation as a
`SchemaMutation` diff, precheck-then-commit," motivated by two bugs: §H6
(silent SIGSEGV on `frozen<map>` as a PK/CK column) and §H13 (`delete_table`'s
rollback path writing into unallocated blob storage when `create_table`
failed before the blob was resized).

**§H13 is already fixed**, without the redesign: `create_table` now commits
the table's blob row (`blob::resize` + `tupdate`) *before* the column-creation
loop runs, so every `delete_table` rollback call inside that loop operates on
an already-materialised row. The unallocated-offset SIGSEGV is no longer
reachable — simple call-ordering fixed what looked like a structural problem,
and the full `SchemaMutation`/precheck-commit split isn't needed for it.

**§H6 remains open, and is one instance of a broader "frozen collections
aren't really implemented" gap** rather than an isolated crash. All three
symptoms below share one root cause: frozen collections are currently
treated as *identical* to their non-frozen counterparts (AGENTS.md
documents this as intentional for storage), but a frozen collection is
supposed to have a different, well-defined identity — a canonical
byte-comparable serialization — that non-frozen collections don't need
because they can never appear in a key or be compared for equality in a
residual filter.

| Symptom | Test | What's actually missing |
|---|---|---|
| Crash | `testDeletionWithContainsAndContainsKey`, `testUpdateWithContainsAndContainsKey`, `testContainsFilteringForClusteringKeys`, `testContainsOnPartitionKey{,Part}`, `testOrderByForInClauseWithCollectionElementSelection` (§H6) | `key.cppm`'s clustering-key path has no encoding for a map-typed (or set/list-typed) key component at all — derefs something absent. |
| Wrong rows | `testFilteringWithoutIndicesWithFrozenCollections` (§H14) | The WHERE evaluator has no equality comparison for two frozen-collection `ColumnValue`s (only CONTAINS is implemented, and even that is a stub — §H2). |
| Silently accepted | `testInsertingCollectionsWithInvalidElements` (§H4) | Element-type validation against a `frozen<tuple<...>>`'s declared element types isn't enforced on INSERT. |

**Redesign.** Give frozen collections a real identity instead of reusing the
non-frozen path unchanged:
1. **Canonical serialization.** A frozen collection's encoded form must be
   deterministic (sorted keys for map/set — plexdb's `DynamicMap`/`DynamicSet`
   iteration order needs to be checked for this) and self-delimiting, reusing
   `io.codec`'s existing per-element encoding rather than inventing a new
   format.
2. **Key-encodability.** Extend `clustering_compare::ClusteringKeyComparator`
   (or a sibling comparator) to accept a frozen collection column: compare
   the canonical serialization byte-for-byte (matches Cassandra's
   `CollectionType.compare` for frozen collections, which does exactly
   this). This is what `key.cppm` is missing and what crashes today.
3. **Equality.** Once (1) exists, `ColumnValue` equality for frozen
   collections is just a byte-compare of the canonical form — wire up the
   WHERE evaluator's equality operator for `type::List`/`Set`/`Map` /
   `Tuple` when frozen, fixing `testFilteringWithoutIndicesWithFrozenCollections`.
4. **Element-type validation.** INSERT/UPDATE value-coercion (`io.evaluator`'s
   `resolve_evaluated`) already recurses into collection elements per
   §R2/§8's "one traversal, three intents" — extend the tuple-element
   arm to check each literal element against the frozen tuple's declared
   type instead of accepting any shape, fixing `testInsertingCollectionsWithInvalidElements`.

**Invariants.** A frozen collection's `key_encodable` predicate returns true
for every collection/tuple shape whose element types are themselves
key-encodable (recursive — a `frozen<map<int, frozen<list<int>>>>` must
work). No `assert_not_implemented` on the clustering-key path for a frozen
collection type — matches R3's zero-completeness-asserts goal for this
specific area.

**Verification.** Property test: for every frozen collection shape,
`encode ∘ decode` is identity, encoding is deterministic (same logical value
→ same bytes regardless of construction order), and the comparator produces
the same order as constructing two rows and observing Cassandra's own
`ORDER BY`/CONTAINS results (already exercised by the skiplisted tests once
they're re-enabled).

---

### R9 — Cell-writetime, dropped-column, and row-liveness tracking

**Status: not started; scope grew this session.** §H9 was originally just
the dropped-column case; investigating two other wrong-answer failures found
they're the same missing concept — this engine has no notion of *why* a
row or cell is visible beyond "does storage have bytes for it." All three
need `CellMetadata`'s existing writetime field to actually be consulted
against something, not just carried around.

**1. Dropped-then-re-added column with USING TIMESTAMP returns stale rows.**
`testDrop{,Static,Multiple}WithTimestamp` fail because a subsequent INSERT
with `USING TIMESTAMP T2` returns rows that Cassandra shadows against the
drop tombstone at `T1`. `schema::Column` has no `drop_timestamp` field yet.

Add `Optional<S64> drop_timestamp` to `schema::Column`, populated by `ALTER
TABLE DROP COLUMN` at ts=T_d:

```
visible(cell, column) =
    column has no drop_timestamp
    OR cell.writetime > column.drop_timestamp
```

Called by the projection path and the mutation-apply path. Populate
`system_schema.dropped_columns` (currently a `@todo` at `system_schema.cpp:493`,
part of §H11) from the same field.

**2. Row liveness has no row-marker concept.** `testAlterOnlyColumnBehaviorWithFlush`:
a row written by `UPDATE ... SET x = 1` (never touching column `y`) should
vanish entirely once `x` is dropped — Cassandra rows are alive only via an
INSERT row marker or a live regular-column cell, and this row has neither
once `x` is gone. plexdb returns the row anyway (`x`, `y` both NULL) because
"does the BTree entry exist" is the sole presence check today — there's no
marker to distinguish "INSERT with only PK columns" (row marker set, row is
legitimately alive with all-null regular columns) from "UPDATE set one
column, now dropped" (no marker, row should vanish). Confirmed both shapes
currently look identical in storage by testing `INSERT INTO t (k) VALUES
(1)` directly — it correctly returns a row today, which any naive "row
needs ≥1 live regular cell" fix would break.

Add `ROW_FLAG_HAS_MARKER` to `io.codec.cppm`'s `RowMetadata.flags` (currently
only `ROW_FLAG_HAS_TTL` is defined), set unconditionally by
`compute_insert_metadata` (INSERT only — never by `recompute_row_metadata`,
which is UPDATE's row-metadata path and must stay marker-free). The
non-trivial part: `recompute_row_metadata`'s four call sites
(`engine.cpp:2689`, `2737`, `3447`, `3488`) rebuild `RowMetadata` from
scratch on every UPDATE, so a subsequent UPDATE to an INSERT-created row
would silently drop its marker unless these are changed to read the
existing `RowMetadata` first and preserve the flag. Read-path row-presence
checks (at least 4 distinct SELECT execution paths — `native.cpp`'s raw-Rows
streamer, `execute_select_index`, `execute_select_pk_in{,_ordered}`,
`execute_select_with_meta`) then need `row_has_marker(meta) ||
any_regular_column_present` instead of "entry exists."

**3. Range-tombstone timestamp reconciliation across flush boundaries.**
`testReverseQueryWithRangeTombstoneOnMultipleBlocks`: a ranged `DELETE ...
USING TIMESTAMP T1 WHERE i >= ? AND i <= ?` must shadow exactly the cells
with `writetime <= T1` inside that range — checked against cells written
both before and after the delete, at higher or lower timestamps than T1 —
and give the same answer whether or not a flush happened in between. This
needs actual range-tombstone storage (a start/end/timestamp record per
range delete, not just per-cell tombstones) with timestamp-aware
reconciliation at read time. Substantially larger than (1) and (2); no
concrete design proposed here yet.

**Invariants.** A cell is visible iff `writetime > col.drop_timestamp` (or
no drop timestamp exists) AND is not shadowed by a covering range tombstone
with `writetime >= cell.writetime`. A row is visible iff it has a marker or
at least one visible regular-column cell.

**Verification.** For any interleaving of `(INSERT ts=T_i)` and `(DROP
COLUMN ts=T_d)`, `SELECT` returns exactly the cells with `T_i > T_d`. For
row liveness: `INSERT (pk only)` then `DROP` on every other column still
returns the row; `UPDATE (one column)` then `DROP` on that column returns
no row — both across a flush. For range tombstones: interleave point writes
at various timestamps around a ranged delete's bounds and timestamp, before
and after a flush, and check the result set is timestamp-order-independent.

---

## Ordering and dependency graph

```
R3 (Type dispatch) ─┬─► R1 (Plan tree) ─┬─► R9 (Writetime/row-liveness)
R2 (Value, landed) ─┘                   └─► R8 (Frozen collections, independent of R1/R3)
R7 (Errors)      ── depends on R3 for CqlError kinds; independent of R1
R5 (Native spec) ── value-codec half landed; message-framing half independent
```

R2 is done; R3 is now the foundational piece left before R1. R1 is the
biggest remaining win. R8 (frozen collections) doesn't depend on any of the
others and can land any time. R5's message-framing half and R7 can land in
parallel with R1.
