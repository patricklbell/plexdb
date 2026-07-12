## CQL conformance plan — architectural redesign

Companion to [`cql-conformance-status.md`](cql-conformance-status.md); that
document enumerates the concrete failures and hacks (§H1–§H14). This one
names the architectural redesigns that would delete the hacks as a *class*.

The `cql/engine` + `cql/native` tree is 20,489 LOC serving 313 tests. Two
cross-cutting structural problems produce most §H entries:

1. **The AST is the runtime representation.** `execute_inside_transaction` in `engine.cpp` is a single lambda with 116 `if constexpr (SameAs<T, X>)` arms. Prepared statements do not exist as a runtime artefact — `PreparedEntry` stores the raw query string, and every `EXECUTE` re-parses it (`engine.cpp:4499`, `engine.cpp:4513`, `native.cpp:1143`). Type dispatch is a nested chain of `SameAs<T, X>` at 205+ sites in `engine.cpp` alone.
2. **Values are materialised even when only copied.** `Literal` (AST scalar), the AST collection literals, `Evaluated` (partial-evaluation result), and `ColumnValue` (typed in-memory) each encode a real constraint, but the engine builds a fully-typed `ColumnValue` even for pass-through — projecting a stored column to the wire, or writing an exact-type bound value — because the storage format (native-LE) differs from the wire format (big-endian). R2 stores values wire-compatibly so pass-through is a copy and `ColumnValue` is built only to operate — see R2.

Beyond these, the engine leans on `assert_not_implemented` at 34 sites for
type-completeness gaps (§H2, §H6, §H10, `column_value.cppm:38/60`,
`engine.cpp:409/425/430`, `key.cppm:279/410/496/554/637`, ten sites across
`native.cppm`). This was a fair shortcut during initial bringup; the engine
is now mature enough that it is a bug when new code needs one. The R3
redesign — and every redesign below — bans the pattern going forward and
replaces it with compile-time exhaustiveness.

The redesigns below compose: R1 depends on R2 (typed plan needs one runtime
value) and R3 (per-node type-aware wire ops); R3 makes R7 mechanical.
Ordering below is by leverage per line deleted, not by dependency order.

| # | Redesign | LOC delta | Retires |
|---|----------|----------:|---------|
| R1 | AST → typed physical plan tree; walker replaces the 116-arm executor | −3 000 (est.) | §H2, §H4, §H14, 116 `SameAs<T,X>` arms + `SelectOp`/`FilterPlan`/`MutationPlan` variants |
| R2 | One serialization for wire/value/key (type-aware key comparator); materialise `ColumnValue` only to operate; one traversal for the crossings; clearer names (partly landed) | alloc reduction, −`key.cppm` codec | §H2 (empty-union asserts), §H12, 5 `key.cppm` asserts |
| R3 | Collapse the `ast::Type`/`type::Type` duplication; drive `assert_not_implemented` to 0 | −250 | §H8, §H10, 29 `assert_not_implemented` (R2 takes the other 5) |
| R5 | Native protocol as a declarative message spec, generated code committed | −1 400 | §H1 (partial), §H7 (structurally impossible), §H10 (declarative) |
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

### R2 — Materialise `ColumnValue` only to operate; rename the value crossings (partly landed)

**Representations.** A value passes through four forms, each solving a
constraint the others cannot:

- `Literal` (`statements.cppm:32`) — a scalar from the parse tree.
  Width-unresolved (`S64`/`F64` stand in for every int/float width) and
  carries kinds that never reach storage: `Unset` (bind placeholder → "leave
  the column unchanged"), `Hex`, `bool`, `Null`.
- AST collection literals (`MapLiteral`/`SetLiteral`/…, `statements.cppm:90`)
  — hold `Term`s, so their elements may still be unevaluated expressions,
  bind markers, or already-substituted `ColumnValue`s; they cannot take a
  storage shape until the target column type is known (`[1,2,3]` may be a
  list, set, vector, or frozen collection).
- `Evaluated` (`evaluator.cppm:27`) — the result of partially evaluating a
  `Term` against bindings and schema: bind markers, functions, arithmetic and
  type hints are folded; collection literals are kept (deferred until the
  type is known); a resolved `ColumnValue` passes through. This is the value
  form of the raw-AST → prepared-query division.
- `ColumnValue` (`column_value.cppm:26`) — a typed in-memory value; the only
  form you can compare, hash, order, aggregate, or edit element-wise.

**Formats and the transformation ledger.** Three byte formats also exist:
**wire** (CQL native — big-endian, `[bytes]` = 4-byte length + body; `int`=4B,
`bigint`=8B); **storage** (plexdb blob — host-native little-endian, 8-byte
`U64` length prefixes; `read_column_value`/`write_column_value`); and **key**
(order-preserving, byte-comparable; `key.cppm`, separate). Trace two hot paths
with `v bigint`:

| path | today | necessary work |
|------|-------|----------------|
| `INSERT … VALUES(?)`, exact-type bind | wire→`Literal`→`Evaluated`→`ColumnValue`→storage (2 endian flips, 3 wraps, 2 walks) | copy bytes |
| `SELECT v` (projected, not filtered) | storage→`ColumnValue`→wire (materialise tagged union, endian flip) | copy bytes |
| `SELECT … WHERE v > 10`, `m + {…}`, `ORDER BY`, index | storage→`ColumnValue`→operate→reserialize | **must** materialise |
| coercion (`123`→text, `'1'`→int, int→bigint) | typed conversion | **must** materialise |

The pass-through rows do real work only because storage ≠ wire (endianness,
length-prefix width) — a *chosen* mismatch. `ColumnValue` is genuinely needed
only in the bottom two rows: when the engine operates on the value, or coerces
a literal whose type differs from the column's. Materialising it for
projection or exact-type writes is pure overhead (it also allocates
`AutoString8`/`DynamicArray` per value).

**Redesign.**

1. **Store non-key column values in wire format** (or a form memcpy-able to
   it). Then a projected-but-unfiltered column is copied storage→result, and
   an exact-type bound value is copied wire→storage — no `ColumnValue`. On an
   operation or a type mismatch, decode to `ColumnValue` as today; this makes
   materialisation lazy — pay it only where the ledger says it is necessary.

   **Keys use the same serialization as column values, ordered to match
   Cassandra.** Key order is a conformance contract — applications and the
   test suite observe it — so the target is not "some consistent order" but
   Cassandra's, which has two layers:
   - *Partition:* `token ‖ partition-key bytes`, where `token =
     Murmur3_x64_128(key, seed 0)[hi 64]`; partitions sort by token.
     plexdb already does this (`key.cppm:652` `append_token_prefix`) but includes the key bytes unnecessarily,
     these key bytes should be stored in the static (static data should always be present, related code should be simplified)
     and it stays `memcmp` — the token is a fixed 8-byte quantity and the
     tiebreak is a byte compare, exactly Cassandra's `DecoratedKey` rule.
   - *Clustering:* each column compared by its type's `AbstractType.compare`,
     reversed per column for `DESC`. Several are **not** byte-lexicographic —
     `timeuuid` (by embedded timestamp), `uuid`, `decimal`/`varint` (numeric),
     `float`/`double` (sign-magnitude).

   So keep the partition token layer as-is, and give the *clustering* BTree a
   stateful, type-aware comparator — `VarlenKeyPolicy` already carries one
   (`btree.policy.cppm:84,98`), which is exactly how Cassandra, SQLite, and
   PostgreSQL compare (`AbstractType.compare`; `sqlite3VdbeRecordCompare`; PG
   opclass cmp). This lets clustering keys be stored in wire form (unifying the
   formats, making a projected key column a memcpy) and deletes `key.cppm`'s
   order-preserving codec and its 5 asserts (R3).

   Matching Cassandra's order is a second reason to prefer the comparator over
   order-preserving bytes: the comparator *is* `AbstractType.compare`, whereas
   the byte encoding must reproduce each comparator's order through `memcmp`
   and today gets the non-lexicographic types wrong — `timeuuid` is stored as
   raw bytes (`key.cppm:620`), so it orders by low timestamp bits instead of
   time, diverging from Cassandra; `decimal`/`varint` hit the asserts.

   The comparator gives up one thing memcmp keys retain: byte-level prefix
   compression and separator suffix truncation (the prefix-B-tree fanout win).
   plexdb realizes neither today (separators are full key copies,
   `btree.types.cppm:24`), so it is latent, not lost. Keeping that option
   open — a BTree API that avoids serializing full keys, and a hybrid
   per-table comparator — is tracked in `TODO.md`, gated on benchmarks.
2. **One traversal, three intents.** Validate, build a `ColumnValue`, and
   encode-to-bytes are currently three separate walks over `Evaluated × type`
   kept in agreement by a runtime assert (`engine.cpp:370`). Fold them into a
   single per-kind traversal so a type handled by one is handled by all three
   by construction.
3. **Rename the crossings** to say source→target and intent:

   | today | proposed | meaning |
   |-------|----------|---------|
   | `read_column_value` | `decode_value` | storage bytes → `ColumnValue` |
   | `write_column_value` | `encode_value` | `ColumnValue` → storage bytes |
   | `resolve_evaluated` | `coerce_to_value` | `Evaluated` → `ColumnValue` (target type) |
   | `resolve_literal_scalar` | `coerce_scalar` | scalar `Literal` → `ColumnValue` |
   | `write_evaluated_as_column_value` (renamed from `cast_write_evaluated_as_column_value`) | `encode_evaluated` | `Evaluated` → storage bytes, no intermediate |
   | `can_write_evaluated_as_column_value` (renamed from `can_cast_write_evaluated_as_column_value`) | `can_coerce` | validate the crossing |
   | `can_write_column_value` | `can_encode_value` | validate a `ColumnValue` fits |
   | `materialize_as_column_value` | *(drop)* | inline `coerce_to_value` |

Keep the four representations distinct — do not merge `Literal` into
`ColumnValue` (its `Unset`/`Hex`/unresolved widths reintroduce empty-arm
asserts). `NestedColumnValue` may fold into `ColumnValue` as an independent
wrapper cleanup. Per-arm coercion behaviour follows the
[type-coercion spec](cql-type-coercion-spec.md).

**Landed.** `Constant` → `Literal`; §H12 empty-union and the
`column_value.cppm` hash/eq crashes fixed; the direct `resolve_evaluated`
path (no byte round-trip). Schema epoch — `Schema.version` bumps on DDL,
`PreparedEntry.schema_version` re-derives a stale prepared entry.

**Deletes.** `ColumnValue` materialisation on the projection and exact-type
write paths; the three-walk agreement assert (`engine.cpp:370`) becomes
structural; `key.cppm`'s order-preserving encode/decode codec (replaced by a
type-aware comparator policy) and its 5 asserts.

**Migration.** Storage format has not been released, no migration necessary.

**Invariants.**
- `ColumnValue{}` is `Null`; `visit(ColumnValue)` is total; no value accessor
  reaches `assert_not_implemented`.
- Wire, value storage, and key storage use one serialization. Partitions sort
  by `token ‖ key bytes` (`memcmp`, matching Cassandra's ring order);
  clustering columns sort by a type-aware comparator reproducing
  `AbstractType.compare` × ASC/DESC.
- A value is decoded to `ColumnValue` only on an operation or a coercing
  write; projection (including key columns) and exact-type writes copy bytes.
- `can_coerce`, `coerce_to_value`, and `encode_evaluated` share one traversal.

**Verification.** For every `Evaluated × column-type` fixture (scalars, each
collection kind, tuple, UDT, widenings): `can_coerce` agrees with whether the
traversal succeeds, and the bytes from `encode_evaluated` equal those from
`coerce_to_value` then `encode_value`. A projection benchmark shows zero
`ColumnValue` allocations for a `SELECT` of unfiltered columns.

---

### R3 — Collapse the `ast::Type`/`type::Type` duplication; drive `assert_not_implemented` to 0

**Problem shape.** `type::Type` (`types.cppm:243`) and `ast::Type`
(`types.cppm:380`) are parallel trees differing only in UDT reference
(handle vs textual name), with duplicated `operator==`/hash/`create_*`.
Separately, 34 `assert_not_implemented` sites remain for type-completeness
gaps — each a runtime crash on a collection/tuple/UDT shape (§H8, §H10;
`native.cppm` wire arms, `column_value.cppm` hash/eq; the 5 `key.cppm` ones
are retired by R2's type-aware comparator, which drops key serialization).
Dispatch already runs through `plexdb.tagged_union`'s `visit()`; these two are
what is left.

**Redesign.**

1. Optionally parameterise the type tree as `Type<UdtRep>` — `Type<UdtName>`
   from the parser, `Type<UdtHandle>` in the engine, `resolve(Type<UdtName>,
   Schema) -> Type<UdtHandle>` a `visit()` walk — collapsing the duplicated
   boilerplate to one instantiation per representation. Worth doing only if
   the shared template is smaller and clearer than the two structs; measure
   `types.cppm` first.

2. Drive the remaining 29 `assert_not_implemented` sites to 0 by implementing
   each case (R2 retires the other 5 in `key.cppm`). The `column_value.cppm`
   collection recursion can land now; a few depend on R2 finishing
   (`engine.cpp` value accessors) or R5 (native encoder arms).

Sites that legitimately handle only `type::Basic` (the wire type-code table)
keep their `switch`, built with `-Werror=switch-enum` so a missing
enumerator is a build failure. New type-completeness gaps must implement the
case, not `assert_not_implemented`.

**Deletes.**
- `ast::Type` boilerplate, if item 1 is taken.
- The remaining 29 `assert_not_implemented` fallbacks, replaced by real
  implementations.

**Invariants.**
- No `assert_not_implemented` for type completeness exists after R3 lands,
  and none may be introduced later (CI grep gate).
- If item 1 is taken: `wire_type_string`, `wire_type_code`, `key_encode`,
  `wire_encode` each remain a single `visit()` over the shared `Type`
  variants; adding a type is one variant + one arm per `visit()`,
  compile-enforced.

**Verification.** Property tests over `all_types()` — including every
collection/tuple/UDT shape that currently asserts: `wire_encode ∘
wire_decode` is identity; `key_encode(v, ASC/DESC)` produces prefix-orderable
bytes; `wire_type_string` matches the driver's captured type name. A CI grep
asserts zero type-completeness `assert_not_implemented`.

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
freely on the heap. Message quality is worth more than allocation cost on a
path that ships a client-visible exception, so R7 has no dependency on the
arena work.

**Deletes.**
- 23 `create_*_error` factories in `engine.cpp`.
- `PlanError` enum (subsumed by `CqlError::Kind`).
- All inline `AutoString8 + ...` message construction.
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
R2 (Value)         ─┘                   └─► R8 (SchemaMutation)
R7 (Errors)      ── depends on R3 for CqlError kinds; independent of R1
R5 (Native spec) ── independent
```

R2 and R3 are foundational. R1 is the biggest single win but requires R2
(the value crossings) and R3 (`ast::Type` collapse, zero completeness
asserts). R5 is independent and can land in parallel.
