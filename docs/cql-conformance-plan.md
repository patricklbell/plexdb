## CQL conformance plan — architectural redesign

Companion to [`cql-conformance-status.md`](cql-conformance-status.md); that
document enumerates the concrete failures and hacks (§H1–§H14). This one
names the architectural redesigns that would delete the hacks as a *class*.

The `cql/engine` + `cql/native` tree is 20,489 LOC serving 313 tests, and
three cross-cutting inefficiencies compound to produce every §H entry:

1. **The AST is the runtime representation.** `execute_inside_transaction` in `engine.cpp` is a single lambda with 116 `if constexpr (SameAs<T, X>)` arms. Prepared statements do not exist as a runtime artefact — `PreparedEntry` stores the raw query string, and every `EXECUTE` re-parses it (`engine.cpp:4499`, `engine.cpp:4513`, `native.cpp:1143`). Type dispatch is a nested chain of `SameAs<T, X>` at 205+ sites in `engine.cpp` alone.
2. **Coroutine promise frames allocate on the heap.** 184 `coroutine::Task<>` sites across the CQL tree. The compiler can HALO (Heap Allocation eLision Optimization) many of them, but the coroutines currently share a global `operator new`, so any that survives inlining pays a general-purpose malloc pair on a path that is otherwise cold.
3. **Five representations of "a value".** `Constant`, `Evaluated`, `ColumnValue`, `NestedColumnValue`, and the AST literal types (`MapLiteral`, `SetLiteral`, `ListOrVectorLiteral`, `UdtLiteral`, `TupleLiteral`) all describe values with different in-memory shapes. Every crossing between them is a switch, and several crossings SIGABRT (`column_value.cppm:38`, `column_value.cppm:60`, `engine.cpp:409`, `engine.cpp:425`, `engine.cpp:430`).

Beyond these three, the engine leans on `assert_not_implemented` at ~20 sites
for type-completeness gaps (§H2, §H6, §H10, `column_value.cppm:38/60`,
`engine.cpp:409/425/430`, `key.cppm:279/410/496/554/637`, ten sites across
`native.cppm`). This was a fair shortcut during initial bringup; the engine
is now mature enough that it is a bug when new code needs one. The R3
redesign — and every redesign below — bans the pattern going forward and
replaces it with compile-time exhaustiveness.

The redesigns below compose: R1 depends on R2 (typed plan needs a single
runtime value), which is cheap in memory only with R6; R3 makes R7
mechanical; R5 makes R4 tractable. Ordering below is by leverage per line
deleted, not by dependency order.

| # | Redesign | LOC delta (est.) | Retires |
|---|----------|-----------------:|---------|
| R1 | AST → typed physical plan tree; walker replaces the 116-arm executor | −3 000 (net) | §H2, §H4, §H14, 116 `SameAs<T,X>` arms + `SelectOp`/`FilterPlan`/`MutationPlan` variants |
| R2 | **Done** — value shapes consolidated into `Literal`/`Value`; coercion behaviour in the [coercion spec](cql-type-coercion-spec.md) | −1 200 | §H2 (empty-union asserts), §H12 |
| R3 | Type dispatch via existing `visit()`; delete redundant `switch(type::Basic)` | −900 | §H8, §H10 (compile-time), §H14 (testMapBulkRemoval), and 350+ scattered `switch(type::Basic)` |
| R4 | Arena-backed coroutine promise allocator; keep coroutines pervasive | −0, huge alloc reduction | promise-frame heap pressure on every request |
| R5 | Native protocol as a declarative message spec, generated code committed | −1 400 | §H1 (partial), §H7 (structurally impossible), §H10 (declarative) |
| R6 | Bounded per-request bump arena + STL-style templated allocator | −400 (call sites), massive alloc reduction | 126 `AutoString8` sites in `engine.cpp`; heap pressure on every query |
| R7 | Structured errors + Cassandra-source-extracted message strings | −600 | §H1, §H4 |
| R8 | Schema mutation as `SchemaMutation` diff, precheck-then-commit | −300 | §H6, §H13 |
| R9 | Cell-writetime and dropped-column shadowing | +200 (new logic) | §H9, part of §H11, §H14 (dropped-column state) |

Out-of-scope: standalone LWT and Conditional BATCH (multi-node), documented
at the bottom. JIT compilation of the plan tree is discussed under R1 as an
optional future step — nothing in this plan blocks or requires it.

---

### R1 — Typed physical plan tree; delete the AST-walking executor

**Problem shape.** `engine.cpp` is 4 520 lines. Its heart is one visitor
lambda with 116 `if constexpr (SameAs<T, Statement>)` arms, each of which
walks a distinct AST shape by hand, calls the planner (which produces
another AST-adjacent structure `SelectPlan` / `MutationPlan`), and walks
that. Prepared statements are strings — every `EXECUTE` calls `parsers::parse`
again (`engine.cpp:4499`), so the "prepared" cache is only useful for the
bind-variable metadata it happens to keep.

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
plans once per call (parser output lives on the request arena, so the
compile is allocation-free) and enters the same walker.

**Why plan tree over bytecode.** Postgres, DuckDB, Umbra all interpret typed
plan trees; SQLite's VDBE bytecode is the outlier and pays for it in debug
UX. A typed tree keeps the walker code readable, `EXPLAIN` becomes a tree
print, and peephole optimisation is a tree → tree rewrite. Bytecode would
save a small constant on dispatch cost that is not the bottleneck; the store
is pager-I/O bound.

**JIT (future, non-blocking).** If a workload appears where the walker
dispatch is the bottleneck, the plan tree admits a straightforward LLVM
lowering (Umbra shows the pattern: interpreter on first N calls, JIT once a
plan is hot). This plan does not decide when or whether to add it — the
typed tree is designed so both options remain open. Nothing else in this
document assumes a JIT.

**Deletes.**
- `SelectPlan`, `MutationPlan`, `FilterPlan`, `SelectOp`, and every
  `type_matches_tag<...>` chain in `engine.cpp` (~205 sites) and
  `planner.cpp` (~87 sites).
- `evaluator.cpp`'s `apply_operator` (§H2), `evaluate_contains`,
  `evaluate_where`, `evaluated_to_s64` (`engine.cpp:400–430`) — all become
  `ExprNode::Kind`s or plan nodes with well-defined semantics.
- The 116-arm dispatch — statement kind decides which plan-node tree the
  planner emits.
- The re-parse at `engine.cpp:4499`, `engine.cpp:4513`, `native.cpp:1143`.

**Adds.** ~700 LOC: `PlanNode` / `ExprNode` definitions, walker, planner emit
helpers.

**Invariants.**
- Every valid CQL statement compiles to a plan tree whose walker covers
  every `Kind`. `assert_not_implemented` in the walker is banned — a missing
  case is caught at compile time by `-Werror=switch-enum`, not at runtime.
- Bind-marker numbering is a plan-time property: the planner assigns slot
  indices; the walker reads them. The AST-visitor bind-numbering pass (§H2
  evaluator.cpp:1067) is deleted.

**Verification.** For every passing test today, the walker run produces
byte-equal frames vs the current executor. Property test: `plan(parse(s))`
is idempotent and the plan tree serialises for disk-persistent prepared
caches.

**Side effect.** `EXPLAIN <query>` becomes a plan-tree dump. Predicate
pushdown, constant folding, and residual-filter unification become
localised tree rewrites.

---

### R2 — Consolidate value shapes into `Literal`/`Value` — **done**

**Problem shape.** Five representations of a value coexisted — `Constant`
(AST literal), the AST collection literals (`MapLiteral`/`SetLiteral`/…),
`ColumnValue` + `NestedColumnValue` (engine-side row values), `Evaluated`
(a variant over all of the above), and raw wire bytes. Every crossing was a
switch; several SIGABRT'd (`evaluated_to_s64`, `column_value.cppm` hash/eq),
and the default-constructed empty `TaggedUnion` SIGABRT'd (§H12). Worst of
all, `materialize_as_column_value` converted `Evaluated → ColumnValue` by
*serializing to a temp buffer and deserializing it back* — a full wire
round-trip per value on the INSERT/UPDATE hot path.

**What landed.** The redesign settled on **two** types, not one, because the
shapes serve two genuinely different roles:

- `Literal` (was `Constant`) — parse-time, width-unresolved; owns `bool`/`Hex`/
  `Unset` and S64-for-all-ints. Lives only in the AST/expression layer.
- `Value` (was `ColumnValue` + `NestedColumnValue`) — resolved, width-correct,
  storage/wire/key shape; recursive collections with `visit()`-based hash/eq
  (the two `assert_not_implemented`s deleted).

The single crossing is `resolve(Literal | expr, type::Type) -> Value`, now
total over scalars, list/set/map/vector, tuple, and UDT — which **deleted the
serialize/deserialize round-trip** (`materialize` is now `resolve` + assert).
The unreachable string→inet/varint/… io write arms were removed, making the
write and read encoders symmetric by construction.

`Evaluated` is **retained as a thin wrapper** (a type-safe marker that
pre-calculation ran), not deleted: its collection-literal arms are the
deferred-resolution mechanism, and collapsing them fully belongs to R1's typed
expression tree. The parent-plan idea of a single `Value` with a `Bytes`
kind was dropped — folding `Literal` into `Value` would drag `Unset`/`Hex`/
unresolved-width variants into every storage consumer, re-introducing the
empty-arm asserts R2 exists to remove.

**Also landed (adjacent).** Schema epoch — `Schema.version` bumped on every DDL
mutation, and `PreparedEntry.schema_version` re-derives a stale prepared entry
instead of serving stale bind metadata. This makes "evaluate time" a
schema-stable cache boundary.

**Remaining → coercion spec.** `resolve` currently reproduces the pre-R2
coercion (string→text only, raw S64/F64 arithmetic). The full coercion
behaviour — assignability matrix, numeric precision model with the
`PLEXDB_CQL_WRAP_ON_OVERFLOW` flag, temporal arithmetic, and string-parse
coercions — is specified in the
[type-coercion spec](cql-type-coercion-spec.md) and implemented incrementally
against the conformance gate.

**Invariants (met).** `Value{}` is `Null`; `visit(Value)` is total; no `Value`
accessor reaches `assert_not_implemented`; the storage `Value` visitor is total
by construction (only storable kinds exist, because `Literal` stays separate).
Byte-equivalence of `resolve` vs the old round-trip is covered by a unit test;
totality was proven by a full conformance run with no assert-fire.

---

### R3 — Use existing `visit()` for type dispatch; ban `assert_not_implemented` for completeness

**Problem shape.** `type::Type` and `type::ast::Type` are two parallel trees
differing only in UDT reference (pointer vs textual name). Every operator==,
hash, and `create_*` is duplicated (~200 LOC in `types.cppm`). Then 350+
scattered `switch(type::Basic)` sites in eight files each re-implement
structural recursion over the tree (`io.cpp`: 102 sites, `planner.cpp`: 87,
`evaluator.cpp`: 58, `system_schema.cpp`: 254, `key.cppm`: 103, `native.cppm`:
48). Many switches take a `type::Type` but only look at the `Basic` variant,
crashing on collections at runtime via `assert_not_implemented`
(`key.cppm:279, 410, 496, 554, 637`; `native.cppm` × 10; §H8). Adding a type
means editing eight files; forgetting one is §H8.

**Redesign.** Two coordinated changes, both leveraging patterns already used
in the codebase:

1. **Single parameterised type tree.** `Type<UdtRep>` parameterised on the
   UDT reference representation. The parser produces `Type<UdtName>`; the
   engine produces `Type<UdtHandle>`; `resolve(Type<UdtName>, Schema) →
   Type<UdtHandle>` is a `visit()` walk. All duplicated `operator==`, hash,
   and `create_*` collapse to one instantiation per representation.

2. **Dispatch through the existing `visit()`.** `plexdb.tagged_union` already
   provides `visit(tu, lambda)` and this codebase uses it in
   `column_value.cppm` and `evaluator.cpp`. Every site that today does

   ```
   switch (type::Basic tag) { ... }        // on a full type::Type
   ```

   becomes

   ```
   visit(t.value, per-node lambda)         // covers every Type variant
   ```

   Sites that genuinely receive only a `type::Basic` (the wire type-code
   table, for instance) keep the `switch` but the compilation unit builds
   with `-Werror=switch-enum` so a missing enumerator is a build failure.

   No new visitor-concept infrastructure — `visit()` and generic lambdas do
   the exhaustiveness work. A generic lambda dispatches on the node type
   with a per-node arm; the fallback arm is a compile-time assertion that
   fires at instantiation for any unhandled node.

**Assert-not-implemented is banned.** Every existing
`assert_not_implemented` for type completeness is deleted as part of this
change:
- `column_value.cppm:38, 60` — replaced by real hash / equality via
  `visit()` recursion.
- `engine.cpp:409, 425, 430` — replaced by proper `Value` accessors (R2).
- `key.cppm:279, 410, 496, 554, 637` — replaced by real collection-key
  encoders driven off `visit()`.
- `native.cppm:180, 236, 240, 244, 247, 390, 392, 394, 396` — folded into
  R5's declarative encoder.
- `native.cpp:153, 251, 493` — same.

New code that would previously reach for `assert_not_implemented` must
instead implement the case. The engine is mature enough that
"not-implemented" is a bug, not a placeholder.

**Deletes.**
- ~200 LOC of `ast::Type` boilerplate.
- 350+ switches replaced by ~15 `visit()` call sites (~40 LOC each = ~600
  LOC total, replacing 2 000+ LOC of switches and their `not_implemented`
  fallbacks).
- `key.cppm` shrinks from 900 LOC to ~250 (one KeyEncoder + one KeyDecoder,
  each a single `visit()` over `type::Type`).
- `native.cppm`'s type-code, encode, and size functions collapse into one
  encoder driven by `visit()`.

**Invariants.**
- `wire_type_string`, `wire_type_code`, `key_encode`, `wire_encode` all agree
  by construction — each is a `visit()` over the same `type::Type<UDT*>`
  variants.
- Adding a type is: one variant in `type::Type` + one arm in every
  `visit()`. The compile fails until every arm is complete.
- No `assert_not_implemented` for type completeness exists after R3 lands,
  and none may be introduced in future code.

**Verification.** Property tests parameterised over `all_types()` — for
each fixture type, `wire_encode ∘ wire_decode` is identity;
`key_encode(v, ASC)` and `key_encode(v, DESC)` produce prefix-orderable
bytes; `wire_type_string` matches the driver's captured type name.
Compile-time verification: a static assert in each `visit()` site that all
`Type` variants are handled.

---

### R4 — Arena-backed coroutine promise allocator; keep coroutines pervasive

**Problem shape.** 184 `coroutine::Task<>` sites across `cql/engine` and
`cql/native`. Whether coroutines are the *right* structure is not the
question — they read well and the request-response shape maps cleanly onto
them. The problem is that any coroutine the C++ compiler does not HALO-elide
allocates its promise frame on the general heap, contributing to the
per-query allocation load. There is no `coroutine::Task<>::promise_type::operator new`
override.

**Redesign.** Override the coroutine promise's allocator on
`coroutine::Task<>` so promise frames come from the current thread's
per-request arena (R6). The scratch arena is already equipped on the main
thread by `main.test.cpp`; the arena reset at request end frees every
non-HALO promise frame in a single pointer bump.

```
Task promise allocation:
    allocate(n bytes)  → bump current thread's per-request arena
    deallocate         → no-op; arena reset at request end frees the lot
```

Coroutines stay everywhere they are today. Async is not restructured. What
disappears is the per-suspend malloc pair for every non-elided frame.

**Deletes.** Zero source-line deletions in the engine. Massive reduction
in per-query malloc traffic on suspending call chains: the pager fetches
that dominate today's promise allocations move to the arena bump.

**Invariants.**
- `promise_type::operator new` is thread-local; the arena is per-request and
  reset when the response frame ships. Coroutine promise frames outliving
  the request are a bug (they would return a dangling pointer to the
  arena-freed frame).
- HALO remains an optimisation the compiler is free to apply.

**Verification.** Benchmark: prepared point-lookup on a warm cache
allocates ≤ 1 heap block (the response frame body) — the same target as
R6's arena win. Compile-time check: `coroutine::Task<>::promise_type` has an
`operator new` override.

**Note on scope.** Earlier drafts of this plan proposed rewriting the
majority of the 184 coroutine sites to plain functions ("sync-default").
That was overstated: HALO handles the elidable ones already, and the arena
allocator here handles the rest. The pervasive-coroutine shape is a
deliberate choice; R4 makes it cheap, not different.

---

### R5 — Native protocol as a declarative message spec; commit generated code

**Problem shape.** `native.cpp` (1 245 LOC) + `native.cppm` (701 LOC) hand-
code every CQL message shape as imperative byte-appending. Encoder gaps
appear as `assert_not_implemented` scattered across 10+ sites (§H10). §H7
is a truncated-frame bug that no invariant would have caught — the writer
sets a length header and the body diverges.

**Redesign.** One spec file describes every message:

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
    (for [option]<T>: dispatch table mapping type-code → wire shape)
```

Two walkers — `encode(spec, MessageValue) → Bytes` and
`decode(spec, Bytes) → MessageValue` — parameterised by the spec. Adding a
v5 opcode is a diff to the spec table.

**Committed generated code.** The spec table is the source of truth. A
`tools/native_regenerate.sh` script produces the encoder and decoder
functions from it; the generated files are committed to the repo (like
`.pb.h` output). Readers see plain C++, code reviews see the diff, but the
authoring surface is the spec. This preserves R2's flexibility guarantee —
if the `Value` layout needs to change, the regeneration produces new
encoders without invalidating any hand-written code.

**Deletes.** ~1 400 LOC of imperative encoders/decoders. §H1's parser rule-
name leakage is structural (the error field's wire shape is `[string]`, and
the string value comes from R7's formatter — not from lexy).

**Invariants.**
- The response frame's header length equals the byte length of the encoded
  body. Enforced by having `encode` write the header last, from the actual
  written byte count. §H7 becomes structurally impossible.
- No file outside `native/messages.cppm` (spec) constructs response bytes.
- `assert_not_implemented` in the protocol layer is impossible: an
  incomplete spec is a build-time error, and R3 forbids the pattern going
  forward.

**Verification.** Golden vectors: for each message spec, capture one
Cassandra-produced frame in every version and diff byte-for-byte. Fuzzing:
for random `MessageValue`, `decode(encode(v)) == v`. Regeneration is
idempotent: running `native_regenerate.sh` twice produces byte-identical
output; CI runs it and checks the tree is clean.

---

### R6 — Bounded per-request bump arena; STL-style templated allocator

**Problem shape.** 126 `AutoString8` construction sites in `engine.cpp`
alone. Every error message, every projected column alias, every type
string, every `AutoString8(some_slice) + "..."` concatenation allocates.
Per-query, the load is measured in dozens of malloc/free pairs, all on
data that dies when the response frame ships.

**Redesign.** Two pieces:

1. **Per-request bounded arena.** The scratch arena is extended to
   per-request lifetime for the native handler:
   - Scope opens when a request frame arrives.
   - Every intermediate — parsed AST (R1), planned tree, `Value` collection
     payloads (R2), non-HALO coroutine promise frames (R4), error message
     bytes on the success-path (R7), result row buffers before frame ship
     — allocated on the arena.
   - Scope resets when the response frame is on the wire.

   The arena has a **default cap of 64 MB per request** and rejects
   allocations that would exceed it with a `CqlError::RequestTooLarge`.
   Individual element / key allocations are capped at **64 KB**, matching
   Cassandra's per-value limit; this closes the three `over64K` xpasses
   with an explicit, documented decision. Both caps are policy knobs
   surfaced in the config; the defaults match upstream.

2. **STL-style templated allocator.** Dynamic containers gain an allocator
   template parameter, C++-standard-library style:

   ```
   DynamicArray<T, Allocator = HeapAllocator>
   DynamicMap<K, V, Allocator = HeapAllocator>
   AutoString8<Allocator = HeapAllocator>
   ```

   The default `HeapAllocator` preserves the current behaviour — every
   existing user compiles unchanged. Request-lifetime code paths use
   `<T, ArenaAllocator>` explicitly. Schema storage, prepared cache
   entries, and other request-outliving state stay on the heap allocator
   by default.

**Deletes.** Every `AutoString8 + AutoString8` heap alloc chain on the
request path. Result: malloc / free pairs per query drop from ~40 to < 5
(schema-mutation paths keep their allocations because they persist).

**Invariants.**
- No `AutoString8<HeapAllocator>` is constructed inside the request-handler
  body. Enforced by grep-lint in CI on files marked as request-scoped.
- Arena scope is enforced by lifetime — the request handler owns the arena
  and passes it as a parameter; escape is a compile error (borrow the
  handle by reference; caller-outlives-arena is proven by lifetime).
- Element allocations over 64 KB return `CqlError::ValueTooLarge` (matching
  the Cassandra exception name); the write path never sees the value.

**Verification.** Benchmark: prepared point-lookup allocates ≤ 1 heap
block (the response frame body). Malloc counter in a Catch2 harness fails
the test if the count exceeds a per-op budget. The over-64K xpass tests
now expect the plexdb rejection message; three lines diff in the fixtures.

---

### R7 — Structured errors + Cassandra-source-extracted message strings

**Problem shape.** §H1 (Cassandra format drift), §H4 (parser rule-name
leakage), and 23 hand-rolled `create_*_error` factories in `engine.cpp`
that build message strings by inline concatenation. `PlanError::ColumnNotFound`
is repurposed at 7 sites for "unknown function", "invalid CAST target", etc.
(`planner.cpp:1236, 1289, 1338, 1394, 1408, 1430, 1438`).

**Redesign.** Two coordinated pieces:

1. **Structured `CqlError` with typed payloads.**

   ```
   CqlError = tagged variant over kinds:
       UnknownFunction        { name }
       CounterOnNonCounter    { column, operation_expr }
       AllowFilteringRequired { column }
       OrderByNonClustering   { column }
       UdtInPkAlterAddField   { keyspace, type_name }
       IncompatibleLiteral    { column, from_type, to_type }
       ValueTooLarge          { column, size }
       RequestTooLarge        { size }
       ParseFailure           { parse_error_kind, line, column, near_token }
       ...   (~40 kinds total, one payload struct per kind)

   format_wire_message(err, arena) → String8    // Cassandra-flavoured
   ```

   Grammar-typed parser errors: each lexy production carries a
   `ParseErrorKind` tag; the sink maps the tag + byte offset to
   `CqlError{Kind::ParseFailure, {ParseErrorKind, offset}}`. The formatter
   produces the Cassandra shape (`"line 1:87 mismatched input '...' expecting ..."`),
   not `parsers::grammar::primary_term`. §H4 rule-name leakage becomes
   structurally impossible.

2. **Cassandra-extracted message strings.** For each `CqlError::Kind`, the
   exact wire-format string is lifted from Cassandra's source. A small
   tool (`tools/extract_cassandra_errors.py`) scans the Cassandra Java
   tree (`org.apache.cassandra.exceptions.*`, `SyntaxException.java`,
   `InvalidRequestException` call sites) and produces a table:

   ```
   Kind::CounterOnNonCounter -> "Invalid operation ({op_expr}) for non counter column {col}"
   Kind::UdtInPkAlterAddField -> "Cannot add new field to type {ks}.{type_name}"
   ...
   ```

   Committed to `cql/engine/error_messages.cppm`. `format_wire_message`
   picks the template and substitutes payload fields. Regeneration is
   idempotent; CI runs it and checks the tree is clean.

**Error-path allocations.** Errors are cold; the formatter allocates
freely on the heap or arena as convenient. Only the success path is
budget-constrained (R6). Message quality is worth more than the
allocation cost on a path that ships a client-visible exception.

**Deletes.**
- 23 `create_*_error` factories in `engine.cpp`.
- `PlanError` enum (subsumed by `CqlError::Kind`).
- All inline `AutoString8 + ...` message construction (also served by R6).
- §H4 rule-name leakage — the sink never sees the rule name in the output
  path.

**Invariants.**
- No file outside `errors.cpp` constructs an error wire message.
- Every `CqlError::Kind` has one and only one `format_wire_message` case
  and one and only one template row in `error_messages.cppm`; every case
  has a golden-string test vs upstream.
- Where plexdb's message is strictly more informative than Cassandra's
  (column name + literal, richer parse position, etc.) it is documented
  in the fixture with a `# upstream: "..."` comment and the conformance
  regex is widened.

**Verification.** For each Cassandra error string in the test suite,
plexdb's `format_wire_message(err)` produces the exact same bytes. Regex
matches in upstream tests become string equality. Tests that used to
`Regex pattern did not match` (§H1) now pass; tests where plexdb's message
is more informative are annotated in the fixture as a documented
divergence.

---

### R8 — Schema mutation as `SchemaMutation` diff, precheck-then-commit

**Problem shape.** §H6 (silent SIGSEGV on `frozen<map>` PK/CK because
`schema.cpp` accepts it and `key.cppm` crashes), §H13 (`delete_table`
writes into unallocated blob storage when `create_table` failed midway).
Both are symptoms of interleaving validation with mutation.

**Redesign.** DDL as a two-stage pipeline over a `SchemaMutation` diff:

```
SchemaMutation = tagged variant over kinds:
    CreateKeyspace | DropKeyspace | AlterKeyspace
    CreateTable    | DropTable    | AlterTable
    CreateType     | DropType     | AlterType
    CreateIndex    | DropIndex
    ...

  each mutation carries a fully-resolved payload:
    - columns to add / drop
    - indexes to create / remove
    - keyspace options
    - all type references resolved to handles (no AST refs)

  the payload is self-contained — no borrowed references to parser
  output — so it survives beyond the request and can be serialised.
```

1. **Precheck** (pure). `(Schema, SchemaMutation) → Optional<CqlError>`.
   Validates every constraint before any mutation. Runs all
   `key_encodable` checks (R3), all reference-integrity checks, all
   type-compatibility checks, all Cassandra-flavoured name/case/length
   rules. Zero side effects on `Schema` or its blobs.

2. **Commit** (async, atomic). `(Schema, SchemaMutation) → Task<Schema'>`.
   Applies the mutation as one blob resize + one record append + one
   index update. No intermediate state observable. No rollback path exists
   because there is nothing to roll back.

The precheck state — everything needed to reject a bad mutation — is
`(Schema, SchemaMutation)`. Nothing else. This makes the precheck trivial
to test in isolation: fixture schema + fixture mutation → expected error
or None.

Delete `delete_table`'s tombstone-write-into-blob path used for rollback
(`schema.cpp:958-971`) — it's no longer called from `create_table`. It's
still called by `DROP TABLE` (as a legitimate mutation), but only against
fully-materialised tables.

The hoisted duplicate-name detection in `engine.cpp:2524` returns to the
schema layer where it belongs.

**Multi-node extension (future).** In a replicated setup, the
`SchemaMutation` diff is exactly the object that Raft/Paxos would replicate
between nodes: precheck runs on every replica, commit runs on the leader,
followers apply the committed diff. Not in scope here — this plan is
single-node — but the shape is chosen so multi-node is additive, not a
rewrite.

**Deletes.** §H6 becomes a proper `CqlError` at plan time. §H13 becomes
impossible. The engine-side hoisted checks (~150 LOC of `engine.cpp`).

**Invariants.**
- No `schema.storage.*` mutation occurs while a `CqlError` may still be
  raised.
- Every DDL statement has one precheck function returning
  `Optional<CqlError>` and one commit function returning `coroutine::Task<void>`.
- The `SchemaMutation` payload is self-contained — no borrowed references
  to the AST — so it can be serialised for replication or logging.

**Verification.** For every `CREATE TABLE` / `ALTER TABLE` / `CREATE INDEX`
failure vector, running the precheck twice returns the same error and
does not perturb `Schema`. For every accepted mutation, applying the
`SchemaMutation` to a fresh `Schema` twice produces the same result.

---

### R9 — Cell-writetime and dropped-column shadowing

**Problem shape.** §H9. `testDrop{,Static,Multiple}WithTimestamp` fail
because a subsequent INSERT with `USING TIMESTAMP T2` returns rows that
Cassandra shadows against the drop tombstone at `T1`. Cells carry
`writetime` metadata (per `io.cppm:70-90`) but the read/write paths do not
consult the schema's dropped-column tombstone timestamps. Additionally,
`system_schema.dropped_columns` is not populated (`system_schema.cpp:544`,
§H11).

**Redesign.** Add an optional `drop_timestamp` field to `schema::Column`.
Populated by `ALTER TABLE DROP COLUMN` at ts=T_d. Add a single predicate:

```
visible(cell, column) =
    column has no drop_timestamp
    OR cell.writetime > column.drop_timestamp
```

Called by the R1 walker's projection node; called by the mutation apply
path in `engine.cpp`. Populate `system_schema.dropped_columns` from the
same field.

**Invariants.** A cell is visible iff its `writetime > col.drop_timestamp`
(or the column has no drop timestamp).

**Verification.** For any interleaving of `(INSERT ts=T_i)` and
`(DROP COLUMN ts=T_d)`, `SELECT` returns exactly the cells with `T_i > T_d`.

---

## Ordering and dependency graph

```
R3 (Type dispatch) ─┬─► R1 (Plan tree) ─┬─► R9 (Writetime)
R2 (Value)         ─┤                   └─► R8 (SchemaMutation)
R6 (Arena+alloc)   ─┴─► R7 (Errors)
                                        R4 (Promise allocator) — depends only on R6
                                        R5 (Native spec)      — independent
```

R2 + R3 + R6 are foundational. R1 is the biggest single win but requires
R2 (typed walker stack) and R3 (per-node type-aware wire ops). R4 depends
on R6 for the arena. R5 is independent and can land in parallel.

## Non-goals

- **One-level partition BTree.** The current two-level split
  (partition BTree → per-partition clustering BTree + static page) is
  correct for wide-row workloads. Point queries take two BTree walks; a
  composite-key single-level design would speed them up by a constant
  but slow full-partition scans, complicate static-cell storage, and
  break the on-disk format. Not worth it.
- **JIT compilation of the plan tree.** Discussed in R1. The plan tree is
  designed so a future JIT (Umbra-style adaptive execution) can be added
  without breaking the walker or the plan-tree shape. No design decision
  in this document forecloses it, and none require it. The interpreter is
  the target for the foreseeable term.

## Out of scope — multi-node

- **Standalone LWT** (`UPDATE … IF`, `DELETE … IF`, `INSERT … IF NOT EXISTS`) — requires Paxos across replicas. On a single replica the semantics collapse to "always applies", so there is nothing meaningful to test. Engine returns `Invalid` cleanly.
- **Conditional BATCH** — same rationale.
- **Multi-node schema propagation** — R8's `SchemaMutation` diff is the natural replicated object, but the transport (Raft/Paxos) is out of scope.
- Tracked in `TODO.md` under "Conditional BATCH and standalone LWT".
