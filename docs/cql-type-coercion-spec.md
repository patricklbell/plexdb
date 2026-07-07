## CQL type-coercion specification

R2 of the [conformance plan](cql-conformance-plan.md) landed the value-type
infrastructure this spec builds on: the two value types and the single crossing
between them.

```
Literal   — parse-time, unresolved (S64-for-all-ints, F64-for-all-floats, Hex, Unset, bool)
Value     — resolved, width-correct, storage/wire/key shape
resolve(Literal | expr, target : type::Type) -> Value
```

`resolve` exists and is total over every value shape, but it currently
*preserves the pre-R2 coercion behaviour*. This document specifies the coercion
semantics `resolve` and the expression evaluator must obey — the remaining R2
work — following Cassandra, expressed as data tables rather than prose so the
implementation is a lookup, not a switch forest. It replaces the two ad-hoc
sites that exist today: `type_compatible_for_assignment` (`evaluator.cpp`,
strings only) and `evaluate_binary_arithmetic` (`evaluator.cpp`, raw S64/F64
with no precision model).

The spec has three layers, evaluated in this order:

1. **Expression evaluation** — arithmetic/functions run in a *wide* domain, so
   no intermediate overflows. (§2, §3)
2. **Assignability** — can this literal/value legally reach this target type at
   all? A static matrix. (§1)
3. **Demotion** — the wide result is range-checked and narrowed to the target
   at the `resolve` boundary. (§4)

Complex types (§5) recurse through the same three layers per element.

---

### Status — remaining R2 work

R2's value-type infrastructure is done; the coercion *behaviour* below is what
remains.

**Landed (R2).** The `Literal`/`Value` split (was
`Constant`/`Evaluated`/`ColumnValue`/`NestedColumnValue`); `resolve` total over
scalars, list/set/map/vector, tuple, and UDT — replacing the
serialize/deserialize round-trip (`materialize_as_column_value`); symmetric io
encoders (the unreachable string→inet/… write arms were removed). `Evaluated`
is retained as a thin wrapper. `resolve` reproduces the pre-R2 coercion
(string→text only, raw S64/F64 arithmetic), so the gate is held while the
behaviour below is implemented incrementally.

**Remaining (this spec).**
- §1 — assignability matrix as the authoritative literal→target gate.
- §2 — numeric precision model: `numeric_result`, magnitude-based literal
  typing, and the `PLEXDB_CQL_WRAP_ON_OVERFLOW` overflow flag (§2.4), replacing
  the raw S64/F64 `evaluate_binary_arithmetic`.
- §3 — temporal (timestamp/date/time ± duration) arithmetic.
- §6 — string-parse coercions (string → inet/varint/decimal/timestamp/…), which
  also re-enable the io write arms R2 removed as unreachable dead code.
- Fold `narrow_evaluated` / `apply_typed_conversion` into `demote` (§4).

---

### 0. Domains

Every `type::Basic` belongs to exactly one **coercion domain**. Domains gate
which rules apply; cross-domain coercion is only ever via explicit `CAST` or the
string-parse table (§6), never implicit.

| Domain | Members | Notes |
|---|---|---|
| `INT` | tinyint, smallint, int, bigint, counter, varint | counter ≡ bigint for typing |
| `FLOAT` | float, double | |
| `DECIMAL` | decimal | arbitrary-precision fixed point |
| `TEXT` | ascii, text, varchar | text ≡ varchar |
| `BLOB` | blob | |
| `UUID` | uuid, timeuuid | |
| `TEMPORAL` | timestamp, date, time | |
| `DURATION` | duration | not orderable; own arithmetic domain (§3) |
| `INET` | inet | |
| `BOOL` | boolean | |

`INT ∪ FLOAT ∪ DECIMAL` = the **numeric** domain; §2's precision model spans it.

---

### 1. Assignability matrix (literal → target)

A `Literal` of a given syntactic kind is assignable to a target `Basic` iff the
cell is set. This is Cassandra `Constants.Literal.testAssignment` verbatim.
Every cell is *weak* (a literal is never an EXACT match — that rank is reserved
for typed terms during function-overload resolution; for single-receiver column
assignment, assignable/not is all that matters).

Literal kinds map onto R2's `Literal` variants: `STRING`=AutoString8,
`INTEGER`=S64, `FLOAT`=F64, `BOOL`=bool, `UUID`=UUID, `HEX`=Hex,
`DURATION`=Duration, plus `NULL`/`UNSET` (assignable to everything).

| target ↓ / literal → | STRING | INTEGER | FLOAT | BOOL | UUID | HEX | DURATION |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| ascii, text, varchar | ✅ | | | | | | |
| tinyint … bigint, varint | | ✅ | | | | | |
| counter | | ✅¹ | | | | | |
| float, double | | ✅ | ✅ | | | | |
| decimal | | ✅ | ✅ | | | | |
| boolean | | | | ✅ | | | |
| uuid, timeuuid | | | | | ✅ | | |
| blob | | | | | | ✅ | |
| duration | | ✅² | | | | | ✅ |
| timestamp, date | | ✅ | | | | | |
| time | | ✅² | | | | | |
| inet | ✅³ | | | | | | |

¹ counter columns reject direct literal writes outside `UPDATE … SET c = c ± n`;
assignability is for the arithmetic operand, not the stored cell.
² **Verify against Cassandra source** `Constants.Literal.testAssignment` — the
integer→{time,duration} cells are version-sensitive; treat as authoritative only
after extraction (see §8).
³ STRING→{timestamp,date,time,inet,uuid} are *parse* coercions (§6), not identity.

`NULL` is assignable to every target (stores a tombstone / removes the cell).
`UNSET` is assignable to every target (means "no change"); it may never appear
inside a collection or as a partition/clustering key component.

Non-assignable literal→target ⇒ `CqlError::IncompatibleLiteral{col, from, to}`
(R7), raised at plan time, before any storage mutation.

---

### 2. Numeric precision model

#### 2.1 Size + floating flag

Cassandra does not rank by a hand-written lattice; it uses two intrinsics per
type — a **byte size** and an **is-floating-point** flag
(`OperationFcts.returnType/size`, `NumberType.isFloatingPoint`):

```
size:      tinyint=1  smallint=2  int=4  bigint=8  counter=8(≡bigint)
           varint=∞   decimal=∞          (∞ = Integer.MAX, "unbounded")
floating:  float, double, decimal = true;  all INT types = false
```

#### 2.2 Result type of a binary numeric op (typed operands)

`result(a, b)` is symmetric and is *exactly* Cassandra's rule — three lines:

```
result(a, b):
    floating = isFloating(a) || isFloating(b)
    size     = max(size(a), size(b))
    return floating ? floatTypeForSize(size) : intTypeForSize(size)

floatTypeForSize:  4 -> float   8 -> double   ∞ -> decimal
intTypeForSize:    1 -> tinyint 2 -> smallint 4 -> int  8 -> bigint  ∞ -> varint
```

This reproduces every cell of the 9×9 without a special-case list: e.g.
`bigint(8) × float(4)` → floating, size 8 → **double**; `varint(∞) × int(4)` →
non-floating, size ∞ → **varint**; `varint(∞) × float(4)` → floating, size ∞ →
**decimal**; `decimal(∞) × anything` → floating, ∞ → **decimal**.

Derived matrix (rows/cols symmetric; read either way):

| | tiny | small | int | bigint | counter | float | double | varint | decimal |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| **tiny** | tiny | small | int | bigint | bigint | float | double | varint | decimal |
| **small** | | small | int | bigint | bigint | float | double | varint | decimal |
| **int** | | | int | bigint | bigint | float | double | varint | decimal |
| **bigint** | | | | bigint | bigint | double | double | varint | decimal |
| **counter** | | | | | bigint | double | double | varint | decimal |
| **float** | | | | | | float | double | decimal | decimal |
| **double** | | | | | | | double | decimal | decimal |
| **varint** | | | | | | | | varint | decimal |
| **decimal** | | | | | | | | | decimal |

The matrix is derivable, so it need not be a stored table: `result` is the
three-line function above over `size`/`isFloating`. If a `Basic[COUNT][COUNT]`
lookup is preferred for the hot path, generate it from `result` at compile time
and `static_assert` equality — the function is the source of truth either way.

#### 2.3 Literal typing — narrowest fitting type, **not** varint

**Cassandra does not have a varint literal, and does not compute in a wide
type.** There is one INTEGER literal kind
(`Constants.Type.INTEGER.getPreferedTypeFor`); its type is the **smallest fixed
type that holds the literal's magnitude**:

```
integer literal  ->  int      if value fits in int32
                     bigint   else if fits in int64
                     varint   else                       (genuinely > 64-bit only)
float literal    ->  double   if it round-trips through IEEE-754 double
                     decimal  else
string literal   ->  ascii    if ASCII-encodable, else text
hex literal      ->  tinyint  (ByteType)
```

So `1 + 1` is typed `int + int` and computed in **int32**; `varint`/`decimal`
appear only when a literal itself exceeds 64 bits / double precision. There is
no per-op wide-precision penalty because there is no wide-precision phase.

Arithmetic then follows §2.2 on those inferred types, computed in the promoted
machine type. The behaviour *at* the fixed-width limit is the compile-time
policy of §2.4. When a literal meets a *typed* operand (`bigint_col + 1`), the
literal keeps its inferred type (`int`) and §2.2 promotes the pair
(`result(bigint,int)=bigint`). When a literal meets a *column* (assignment /
comparison), it is `resolve`d to that column's type and **range-checked there**
(§4) regardless of policy — `INSERT tinyint_col = 300` → `ValueTooLarge` in both
modes.

#### 2.4 Overflow policy (compile-time)

The behaviour of a fixed-width numeric op whose true result exceeds its result
type (§2.2) is selected at build time:

```
CMake:  PLEXDB_CQL_WRAP_ON_OVERFLOW   (default ON)
macro:  PLEXDB_CQL_WRAP_ON_OVERFLOW   1 = wrap (Cassandra) · 0 = promote
```

**Mode WRAP (default, ON — Cassandra-exact).** The op computes in
`numeric_result(a, b)` and lets fixed-width overflow/underflow wrap two's-
complement, exactly as `Int32Type.add` does (`left.intValue() + right.intValue()`).
Float overflow saturates to ±Inf (IEEE / Java parity). The static result type is
`numeric_result(a, b)`; result-set metadata is stable. Bit-for-bit Cassandra.

**Mode PROMOTE (OFF — overflow-safe divergence).** The op computes with
**checked** arithmetic in `numeric_result(a, b)`. On overflow/underflow the
result is recomputed one step up the **promotion ladder** and the op's result
type becomes that wider type:

```
integer ladder:  tinyint → smallint → int → bigint → varint     (varint: exact, terminal)
float   ladder:  float → double → decimal                       (decimal: bounded scale¹, terminal)
```

Widening propagates bottom-up: a promoted sub-expression hands its wider type to
its parent, which re-runs §2.2. The ladder terminates at `varint`/`decimal`,
which cannot overflow, so evaluation never wraps and never loops. Underflow
(subtraction below `MIN`, float flush-to-zero) promotes by the same rule.

Because the runtime result type is value-dependent in this mode, a **top-level
arithmetic result column is declared as the terminal type of its axis** —
`varint` for an integer expression, `decimal` for a fractional one — so the
result-set metadata can hold any promoted value. (This is the one place mode
PROMOTE changes wire-visible types; it is a deliberate divergence.) Column
assignment is unaffected: the value is `resolve`d/range-checked against the
column type (§4), so a genuine overflow into a narrow column still errors rather
than wrapping — which is the whole point of the mode.

¹ decimal division uses a bounded scale (Cassandra `DecimalType` divides under
`MathContext.DECIMAL128`); `+ − ×` are exact.

Both modes share §2.1–§2.3 unchanged; only the single op-evaluation primitive
branches on the macro. `varint`/`decimal` operands behave identically in both
(they never overflow).

---

### 3. Temporal arithmetic domain

Separate from numeric. Only these pairs are legal; all else ⇒
`CqlError::InvalidOperation`.

| left | op | right | result |
|---|:-:|---|---|
| timestamp | + / − | duration | timestamp |
| date | + / − | duration | date |
| time | + / − | duration | time |

`duration ± duration`, `temporal − temporal`, and `duration × n` are **not**
supported (Cassandra parity). `*`, `/`, `%` are undefined on the temporal
domain.

---

### 4. Demotion (coercion) table — wide/source `Value` → target `Value`

Applied by `resolve` once a target type is known. Symmetric to §1 but operates
on already-evaluated values, and defines the *runtime* action + failure mode.

| from → to | action | failure |
|---|---|---|
| INT → wider INT | sign-extend | — |
| INT → narrower INT (incl. varint→fixed) | **range-check**, then truncate width | out of range ⇒ `ValueTooLarge` |
| INT → float/double | convert | silent precision loss (Cassandra parity) |
| INT/float → decimal | exact widen | — |
| float → double | exact | — |
| double → float | convert | silent precision loss |
| decimal → float/double | convert | silent precision loss |
| decimal/float → INT | **only via CAST**; implicit ⇒ not assignable | — |
| ascii → text/varchar | identity (bytes) | — |
| text/varchar → ascii | validate all bytes < 0x80 | non-ASCII ⇒ `InvalidRequest` |
| text ↔ varchar | identity | — |
| uuid ↔ timeuuid | identity bytes; timeuuid validates version==1 | bad version ⇒ `InvalidRequest` |
| Hex → blob | identity bytes | — |
| same type | identity | — |

**Range-check** is the crux of "demote when we hit a known type": the wide
literal result of §2.3 is bounds-checked against the target's representable
range here. `INSERT … tinyint_col = 200 + 200` computes `400 : varint`, then
`resolve(400, tinyint)` fails `ValueTooLarge` — matching Cassandra, and
impossible to get wrong because the arithmetic never overflowed a machine word.

---

### 5. Complex types

A collection/tuple/UDT literal has **no intrinsic type**; the target column type
drives coercion, recursively. `resolve` dispatches on the target's `type::Type`
node, not on the literal's shape.

#### 5.1 List / Set

```
resolve(ListLiteral es, list<E>):   Value{List{ [ resolve(e, E) for e in es ] }}
resolve(SetLiteral  es, set<E>):     dedup + sort_by(E.comparator) [ resolve(e, E) ]
```

- Homogeneous: every element resolves to the *same* `E`. No per-element variance.
- Element expressions may be arithmetic (`{ 1+1, x*2 }`): each evaluates in its
  inferred type (§2.3) then demotes to `E` (§4).
- **Null / Unset elements are rejected**: `CqlError::NullInCollection`
  (Cassandra: "null is not supported inside collections").
- Set stores in `E`-comparator order; duplicates dedup (last occurrence wins for
  identity). Map/set ordering is a storage invariant (see AGENTS.md key rules).

#### 5.2 Map

```
resolve(MapLiteral kvs, map<K,V>):
    sort_by(K.comparator) [ (resolve(k,K), resolve(v,V)) for (k,v) in kvs ]
```

- Keys coerce to `K`, values to `V`, independently.
- Duplicate keys: **last wins** (Cassandra parity).
- Null key or null value ⇒ `NullInCollection`.
- Empty `{}` is **kind-ambiguous** (set vs map) at parse time; the receiver type
  disambiguates. `resolve({}, set<E>)` ⇒ empty set; `resolve({}, map<K,V>)` ⇒
  empty map. `[]` is always a list; `()` always a tuple.

#### 5.3 Tuple

```
resolve(TupleLiteral es, tuple<T0..Tn-1>):
    Value{Tuple{ [ resolve(es[i], Ti) for i in 0..len(es) ] }}
```

- **Positional.** Element `i` coerces to field type `Ti`.
- `len(es) < n` allowed ⇒ trailing fields are `null` (Cassandra: partial tuple).
- `len(es) > n` ⇒ `CqlError::TooManyTupleElements`.
- Null element allowed inside a tuple (unlike collections).

#### 5.4 UDT

```
resolve(UdtLiteral fields, udt{name_i: Ti}):
    for each (fname, fval) in fields:
        Ti = lookup(udt, fname)  or  CqlError::UnknownUdtField
        out[fname] = resolve(fval, Ti)
    omitted fields -> null
```

- **By name**, order-independent. Unknown field name ⇒ `UnknownUdtField`.
- Omitted fields default to `null`.
- Null field value allowed.

#### 5.5 Frozen

`frozen<…>` does not change coercion: R2/AGENTS treat frozen and non-frozen
identically. `resolve` ignores the `frozen` flag; recursion is the same. (The
flag still affects storage layout and mutability elsewhere — out of scope here.)

#### 5.6 Nesting & precision

Rules compose: an element expression computes wide (§2.3), then demotes to the
element type (§4), then the element sits in the collection. There is no
"collection-wide precision" — each element's target type is fully known from the
column type, so no ⊤ inference is needed below the top level.

---

### 6. String-parse coercions (STRING literal → non-text target)

A STRING literal assigned to certain targets is *parsed*, not reinterpreted.
Table of accepted formats; parse failure ⇒ `CqlError::InvalidLiteralFormat`.

| target | accepted string format |
|---|---|
| timestamp | ISO-8601 / `yyyy-mm-dd[ HH:MM[:SS[.fff]]][zone]` / epoch-ms digits |
| date | `yyyy-mm-dd` / days-since-epoch digits |
| time | `HH:MM:SS[.fffffffff]` / nanos-since-midnight digits |
| inet | IPv4 dotted / IPv6 colon form |
| uuid, timeuuid | canonical 8-4-4-4-12 hex; timeuuid validates version 1 |
| blob | `0x`-prefixed hex |

These are the same routines the driver-facing text protocol uses; centralise
them so `resolve` and the native decode path share one parser per target.

---

### 7. Comparison & WHERE coercion

`col <op> literal` (and `literal IN (…)`) reuse §1 + §4: the literal is
`resolve`d to `col`'s type, then compared with the column's comparator. There is
no separate comparison-coercion table.

- If the literal is not assignable to `col`'s type (§1) ⇒ `IncompatibleLiteral`.
- Cross-domain comparison is never implicit: `int_col = '5'` errors (STRING not
  assignable to INT), matching Cassandra.
- Numeric comparison across INT/FLOAT/DECIMAL promotes both sides to
  `result(lhs_type, rhs_type)` (§2.2) before comparing, so `bigint_col = 5.0`
  compares in double.

---

### 8. Implementation shape (data-driven)

Three committed tables + two recursive functions:

| Artifact | Kind | Source of truth |
|---|---|---|
| `assignable[LiteralKind][Basic]` | `bool` bitset (§1) | Cassandra `Constants.Literal.testAssignment` |
| `numeric_result(a, b)` | 3-line fn (§2.2) | `OperationFcts.returnType` |
| `prefered_type(literal)` | magnitude fn (§2.3) | `Constants.Type.getPreferedTypeFor` |
| `demote(from, to)` | dispatch (§4) | §4 table |
| `resolve(Literal|expr, Type)` | recursive fn (§5) | this doc |
| `parse_string_as(str, Basic)` | dispatch (§6) | §6 table |

`numeric_result` and `prefered_type` are ports of the two Cassandra functions
cited above (in `~/Documents/cassandra`), not hand-transcribed tables — the port
is small enough to diff against the Java by eye. The only genuine table is
`assignable` (§1), lifted from the one `testAssignment` switch.

**What this deletes.** `type_compatible_for_assignment` (`evaluator.cpp:396`)
becomes one row of `assignable`. `evaluate_binary_arithmetic`'s raw S64/F64
fork (`evaluator.cpp:475–534`) becomes: eval operands → `numeric_result` lookup
→ compute in that type → return. `narrow_evaluated` /
`apply_typed_conversion` (`engine.cpp:1855,1903`) fold into `demote`.

**Overflow policy is a build flag, not a hardcoded rule.** `PLEXDB_CQL_WRAP_ON_OVERFLOW`
(default ON = Cassandra wrap; OFF = promote up the ladder). It gates exactly one
place — the op-evaluation primitive that `numeric_result` feeds — so the two
modes cannot drift elsewhere. See §2.4. The CMake option and `#define` land with
the arithmetic evaluator (there is nothing to guard until then).

**Confirmed against the tree (not divergences).**
- String `+` concatenation **is** Cassandra behaviour — `OperationFcts`
  registers `text+text`, `ascii+ascii`, `text+ascii` (`excuteOnStrings` →
  `StringType.concat`). The repo's `text + text` (`evaluator.cpp:485`) is
  correct; keep it. Result type follows the TEXT rows (ascii+ascii→ascii,
  else text).
- Integer literals type as the narrowest of int/bigint/varint by magnitude
  (`Constants.Type.INTEGER.getPreferedTypeFor`), not varint. §2.3.

---

### 9. Worked examples

| Expression | Inferred / op type | Target | Demotion | Result |
|---|---|---|---|---|
| `1 + 1` | `int + int = int` (int32) | — | — | `2 : int` |
| `200 + 200` | `int + int = int` | tinyint col | range-check `400` | `ValueTooLarge` |
| `2000000000 + 2000000000` | `int + int` overflows | — | WRAP mode | wraps: `-294967296 : int` |
| `2000000000 + 2000000000` | promote int→bigint | — | PROMOTE mode | `4000000000 : bigint` |
| `9999999999 + 1` | lit>int32 → `bigint + int = bigint` | — | — | `10000000000 : bigint` |
| `1 + 2.5` | `int + double = double` | float col | convert | `3.5 : float` |
| `bigint_col + 1` | `bigint + int = bigint` | — | — | `bigint` |
| `int_col * 2.0` | `int + double = double` | — | — | `double` |
| `{ 1+1, 3 }` | elems `int` | `set<smallint>` | each →smallint | `{2,3}:set<smallint>` |
| `'ab' + 'cd'` | `text + text = text` | — | — | `'abcd' : text` |
| `{ }` | ambiguous | `map<int,text>` | — | empty map |
| `'2021-01-01'` | STRING | timestamp col | parse (§6) | epoch-ms |
| `int_col = '5'` | — | INT | not assignable (§1) | `IncompatibleLiteral` |
