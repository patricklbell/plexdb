## Per-request arena allocation — performance follow-on

Split out of [`cql-conformance-plan.md`](cql-conformance-plan.md). Nothing
here retires a §H conformance hack or changes a single test result. This is
a pure allocation-reduction proposal: cut the malloc/free traffic a query
generates for scratch strings and coroutine promise frames. It is kept
separate because it is *not* on the conformance critical path and must not
block or be blocked by R1–R9.

**Gate — do not build any of this until a benchmark justifies it.** The
premise is that per-query malloc traffic is a measurable cost. That is an
assumption, not a measurement. Before writing the arena, the templated
allocator, or the promise-allocator override:

1. Land a malloc-counting harness (interpose `os::allocate`/`os::deallocate`,
   or use the Tracy hooks already at `core/os/core.cpp:55-61`) and record
   allocations-per-query and wall-time for a representative mix (warm
   prepared point-lookup, cold ad-hoc `SELECT`, INSERT, batch).
2. Confirm the store is not already pager-I/O bound to the point where
   allocator time is in the noise. The conformance plan states the store is
   "pager-I/O bound" (`cql-conformance-plan.md`, R1 rationale) — if that is
   true, this work moves nanoseconds and is not worth its complexity.
3. Only if malloc traffic is a real fraction of query latency, and only for
   the query classes where it is, proceed — and land the two pieces below in
   the order given, re-measuring after each. Stop as soon as the number
   stops moving.

The largest single piece (the `AutoString8` → `AutoString8Base<A>`
templatization) touches ~790 call sites across 41 files; it is not worth
paying that review cost on faith.

---

### A6 — Bounded per-request bump arena; STL-style templated allocator

**Problem shape.** 126 `AutoString8` construction sites in `engine.cpp`
alone (`grep -c AutoString8 cql/engine/engine.cpp` → 126), out of ~790
across 41 files repo-wide. Every error message, projected column alias,
and type string allocates, and each `operator+` (`string.cpp:257-268`)
allocates a *fresh* buffer for the whole result — a five-term chain
(`engine.cpp:3693-3694`) does five heap round-trips for one string. The
per-query load is dozens of malloc/free pairs, all on data that dies when
the response frame ships. Whether that load is a *cost* is the gate
question above.

**Design.** One bump arena per connection, reset between frames, reached
through an ambient thread-local rather than a parameter threaded through
every coroutine. The whole mechanism reuses idioms already in the tree:

1. *Ambient arena, swapped at suspension.* `cql/native`'s single-threaded
   event loop interleaves independent connections across real suspension
   points (`native::run`, `native.cppm:653`, spawns one
   `connection_handler` per connection), so a thread-local naming "the
   current request's arena" must be saved and restored across each
   `co_await`. `Awaitable` (`core/coroutine/base.cppm`) — the *only*
   awaiter that causes a real suspension anywhere in the tree; every
   suspending `co_await` in `core/aio/aio.cpp` constructs a
   `coroutine::Awaitable{...}` — already does exactly this for Tracy fiber
   tracking: `g_current_tracy_fiber` (`base.cppm:74`) is saved in
   `await_suspend` and restored in `await_resume` (`base.cppm:465-491`).
   The request arena is the same shape of problem and gets the same fix: a
   second thread-local (`current_request_arena`, a new field on
   `threads::Context`), saved/restored alongside the Tracy fiber name.
   `frame_handler` sets it once at entry; every coroutine below it — all
   126 sites in `engine.cpp` — reads it through
   `threads::current_request_arena()` and gains **no signature change at
   all**. This replaces parameter plumbing through the whole call tree
   with two files and a handful of lines.

   This is independent of the existing `arenas[2]` scratch pair
   (`threads::Scope`/`get_scratch`, `threads.cppm:81-101`), which stays
   untouched — those are for short synchronous uses that can't survive a
   `co_await`, which is exactly the boundary the request arena is swapped
   at. Within one connection frames are handled strictly sequentially
   (`post_startup_loop`, `native.cppm:486-646`, `co_await`s
   `frame_handler` to completion — `native.cppm:600,636` — before reading
   the next frame), so one arena per connection, reset between frames, is
   enough (no multiplexing; tracked in `TODO.md`).

2. *Backing is the existing `plexdb.arena` module.* `RequestArena` wraps
   `Arena arena(cap)` (`arena.cppm:36`), one page sized to the cap up
   front. Because `try_reserve` never lets `used` exceed `cap`,
   `arena::push` never enters its page-growth branch (`arena.cpp:65`) — it
   degenerates to plain bump allocation; `reset()` is
   `arena::pop_to(arena, 0)`. Zero new files in `core/os`, zero changes to
   `core/arena`.

3. *Global memory bound via live sysinfo, not a static cap.* Per-request
   caps don't bound process memory, and `cql/native` has no
   connection-count limit today (`core/os/uring.cpp:482`). Rather than a
   fixed byte ceiling, track process-wide usage and admit connections
   against what the box actually has free. `os::allocate`/`os::deallocate`
   (`core/os/core.cpp:55-61`) already wrap every heap block with
   `TracyAlloc`/`TracyFree`; thread a process-wide atomic byte counter
   through the same two call sites (`deallocate` takes no size, so it uses
   `malloc_usable_size(ptr)` to know how much to subtract) and export
   `os::allocated_bytes()`. At connection-accept, query
   `os::query_memory_status()` (`sysinfo.cpp:416`, live `/proc/meminfo`
   `MemAvailable`); if `available_bytes` less a `memory_safety_margin`
   cannot cover another `request_arena_cap`, the connection is declined —
   the same outcome as a plain connection-count limit, so "too many
   connections" and "not enough memory" collapse into one admission check
   at the accept layer, before CQL negotiation. `SystemInfo::vma_limit`
   (`sysinfo.cppm:17`) is a secondary factor if VA exhaustion ever bites.
   Expose `os::allocated_bytes()` and the effective connection ceiling as
   `plexdb::plugin::Stat` `Gauge`s.

**New types.**

```
// core/dynamic/allocator.cppm  (new module plexdb.dynamic.allocator)
concept Allocator = requires(A a, U64 n, void* p) {
    { a.allocate(n) }   -> SameAs<U8*>;
    { a.deallocate(p) } -> SameAs<void>;
};

struct HeapAllocator {                 // stateless, [[no_unique_address]]-friendly
    U8*  allocate(U64 bytes) const { return os::allocate(bytes); }
    void deallocate(void* p) const { os::deallocate(p); }
};

struct ArenaAllocator {                // non-owning handle, trivially copyable
    arena::ArenaPage** page;           // -> the request arena's single pre-sized page
    U8*  allocate(U64 bytes) const { return (U8*)arena::push(page, bytes); }
    void deallocate(void*) const {}    // no-op; reset() rewinds the page offset
};
```

`ArenaAllocator` stores `ArenaPage**` to match `push(Arena&, ...)`'s
existing signature, though for `RequestArena` it never relinks.

```
// cql/engine/request_arena.cppm  (new module cql.engine.request_arena)
struct RequestArena {
    Arena arena;   // arena::allocate(cap, nullptr) — one page sized to cap, from plexdb.arena
    U64   cap;     // e.g. 64 MiB, from config — per-request bound
};

ArenaAllocator             allocator(RequestArena&);          // {.page = &r.arena.page}
Optional<ExecutionStatus>  try_reserve(RequestArena&, U64 n); // cap check only: offset + n > cap
void                       reset(RequestArena&);              // arena::pop_to(r.arena, 0)
```

**Ambient hook.** Extend `threads::Context` with a field independent of
the existing `arenas[2]`, and mirror the Tracy save/restore in
`Awaitable`:

```
struct Context {
    Arena  arenas[2];                        // unchanged — Scope/get_scratch
    Arena* current_request_arena = nullptr;  // new — swapped only at real suspension boundaries
    bool   is_main = false;
};
Arena* current_request_arena();
void   set_current_request_arena(Arena*);    // frame_handler entry only

// Awaitable (base.cppm), alongside _saved_tracy_fiber:
[[no_unique_address]] Arena* _saved_request_arena{};
// await_suspend: _saved_request_arena = threads::current_request_arena();
// await_resume:  threads::set_current_request_arena(_saved_request_arena);
```

Every real suspension already routes through this one template, so this is
the entire centralized fix — nothing in `cql/engine` or `cql/native` needs
to know a suspension happened.

**Container changes.** `DynamicArray<T, Size = U64>`, `DynamicMap<K, V>`,
`DynamicSet<K>`, `DynamicDeque<T>` (`core/dynamic/containers.cppm`) each
gain a trailing defaulted `Allocator = HeapAllocator` parameter — every
existing use keeps compiling against the default. Each free function
(`reserve`, `resize`, `push_back`, …) reads/writes through a new
`[[no_unique_address]] Allocator alloc{};` member instead of calling
`os::allocate`/`os::deallocate` directly. Arena-backed instances are
constructed with an explicit allocator and only moved, never copy-assigned
across a `reset()` — so allocator-propagation semantics don't arise.

`AutoString8` is the largest-diff piece: a concrete struct today with
~170 lines of out-of-line impl in `string.cpp`, named at ~790 sites across
41 files. Rather than rename in place:

1. Rename the struct to `AutoString8Base<A = HeapAllocator>` in
   `string.cppm`, moving member bodies into the interface unit (interface
   units may only define template bodies inline, per AGENTS.md).
2. `using AutoString8 = AutoString8Base<HeapAllocator>;` — all ~790 sites
   keep compiling unchanged.
3. `extern template struct AutoString8Base<HeapAllocator>;` in
   `string.cppm` + the matching explicit instantiation in `string.cpp`
   keeps the heap-backed body compiled once, as today;
   `AutoString8Base<ArenaAllocator>` compiles inline only in the few
   `cql/engine`/`cql/native` units that use it.
4. `to_str`/`fmt`/`bytes_to_hex`/`operator+` get `A`-templated overloads;
   the existing non-template ones are untouched.

**Ownership model.** `RequestArena` lives in `post_startup_loop`'s
coroutine frame (`native.cppm:486`), constructed once per connection:

```
post_startup_loop(Engine&, const tcp::Request&)
    RequestArena request_arena(engine.config.request_arena_cap);
    while (true) {
        ... read one frame ...
        threads::set_current_request_arena(&request_arena.arena);  // only new call site outside frame_handler
        co_await frame_handler<V,C>(engine, req, ..., conn_keyspace);
        reset(request_arena);
    }
```

Nothing below `frame_handler` gains a parameter. The ~23 `create_*_error`
factories and alias/label builders (`engine.cpp:1997-2058`) switch from
`AutoString8` to `ArenaString8`
(`using ArenaString8 = AutoString8Base<ArenaAllocator>;`), constructed
from the ambient allocator — leaf sites stay one-line renames.

**Config.** Runtime knobs, following `cql/entry/main.cpp`'s `argparse`
flags (`--port`, `--checkpoint-interval`), threaded into a small config
struct on `Engine`:
- `request_arena_cap` (`--request-arena-cap-mb`, default 64 MiB) —
  per-request bound.
- `value_cap` (`--value-cap-kb`, default 64 KiB, matching Cassandra).
- `memory_safety_margin` (`--memory-safety-margin-mb`) — headroom kept
  free when deriving the connection ceiling from live `MemAvailable`.

**Implementation phases** (each independently landable and tested):

1. **Allocator + `RequestArena` primitives.** `core/dynamic/allocator.cppm`
   and `cql/engine/request_arena.cppm`, on the existing `plexdb.arena`
   module — no `core/os` changes. Unit tests: `try_reserve` rejects over
   cap; `reset` rewinds `used` to 0.
2. **Process-memory tracking.** Add the atomic byte counter to
   `os::allocate`/`os::deallocate` (`malloc_usable_size` on free), export
   `os::allocated_bytes()`, register the `Gauge`s. Test: counter tracks a
   known alloc/free sequence.
3. **Templatize the dynamic containers** on `Allocator`, defaulted to
   `HeapAllocator`. No call site elsewhere changes. Verify: full
   `core_tests` + `cql_tests` pass unchanged.
4. **`AutoString8` → `AutoString8Base<A>` + alias** (steps above). The
   single largest-diff piece — land in isolation behind a full test-suite
   + ASan run and a build-time/binary-size check before touching
   `engine.cpp`. **Re-measure here:** if steps 1–3 already flattened the
   per-query allocation count, this step may not be worth its review cost.
5. **The ambient hook.** Add the `threads::Context` field and the
   `Awaitable` save/restore. Two files, a handful of lines, mirroring code
   in the same file. Verify: two `Task`s each looping `co_await` on a
   dummy `Awaitable`, each with a distinct arena, asserting
   `current_request_arena()` is always its own between awaits, driven
   interleaved via manual `.resume()`.
6. **Wire `RequestArena` into `cql/native`** and the accept-layer
   admission check (query `query_memory_status()`, decline when
   `available_bytes - margin` can't cover another `request_arena_cap`).
   Verify: native integration tests pass unchanged; a test with a
   deliberately tiny margin declines the connection over the threshold and
   admits one after a close.
7. **Migrate `engine.cpp`'s 126 sites** from `AutoString8` to
   `ArenaString8`, section by section (error factories → alias/label
   builders at `engine.cpp:1997-2058` → `DynamicArray<AutoString8>`
   collections at `engine.cpp:3699-3711` → `PreparedEntry` construction at
   `engine.cpp:4454-4464`). Run the conformance suite after each sub-step:
   a missed lifetime-extension case (e.g. `column_name_storage`,
   `engine.cpp:180-181`) is a silent use-after-reset, not a compile error.
8. **Enforce the caps.** Call `try_reserve` at the two boundaries
   (whole-request cap on frame body, value cap per element). The three
   `over64K` xpass fixtures flip to expect plexdb's rejection message.
9. **CI enforcement.** Grep-based check (flag bare `AutoString8(`
   construction outside an allow-list covering connection-lifetime state
   like `conn_keyspace`) in `.hooks/pre-commit` and the `format-check` job
   in `.github/workflows/test.yml` — new lint, no existing infra.

**Invariants.**
- No `AutoString8` (`AutoString8Base<HeapAllocator>`) is constructed in
  `frame_handler`'s call tree for data that dies at response-ship time.
  Enforced by the phase-9 grep-lint.
- `threads::current_request_arena()` is correct — the resumed connection's
  arena, or `nullptr` outside any request — everywhere except
  mid-suspension inside `Awaitable`. This holds only while **every** real
  suspension constructs a `coroutine::Awaitable` (true today,
  grep-checked); a future suspension primitive that bypasses it breaks
  arena continuity. Worth a one-line comment on `Awaitable`'s declaration.
- `RequestArena` is owned by one `post_startup_loop` frame; only the
  *pointer* is thread-local and swapped, never the arena.
- `try_reserve` returns `None` before every allocation that could exceed
  the per-request cap; a rejected reservation never advances the cursor.
- A connection is admitted only when live `MemAvailable` less
  `memory_safety_margin` covers another `request_arena_cap`; otherwise it
  is declined at accept, identically to a connection-count limit.

**Verification.** Benchmark (the same one that gated the work): a prepared
point-lookup on a warm connection allocates zero heap blocks for
scratch/error/label construction. A malloc-counting Catch2 harness fails on
per-op allocation regression. The admission test drives the accept path
with a tiny margin. The three `over64K` xpass fixtures update to plexdb's
rejection message.

**Optional follow-on, benchmark-gated: virtual-memory-backed arena.** The
baseline sizes each `RequestArena` at a full `cap` of resident memory
regardless of what the connection touches. Usage-based accounting via lazy
VA commit is tighter but more complex — build it only if a metric shows
the gap:

- **Trigger.** `os::allocated_bytes()` (or a per-connection high-water
  `Gauge`, reported on close) shows actual usage a small fraction of
  reserved `cap` while the admission check is refusing connections. Add
  the high-water `Gauge` to the baseline first — it's cheap and needs no
  allocator change — to quantify the gap.
- **The swap.** Back `RequestArena` with an `mmap`-reserved VA range with
  lazy commit and explicit partial decommit — the technique from
  ["Untangling Lifetimes: The Arena Allocator"](https://www.dgtlgrove.com/p/untangling-lifetimes-the-arena-allocator)
  for the known-upper-bound case. Three Linux-only primitives in
  `core/os/core.cppm`/`.cpp` (`PLEXDB_OS_LINUX`-gated):

  ```
  U8*  reserve_virtual(U64 bytes);           // mmap(PROT_READ|WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)
  void decommit_virtual(void* p, U64 bytes); // madvise(MADV_DONTNEED) — return pages, keep the VA range
  void release_virtual(void* p, U64 bytes);  // munmap
  ```

  `reset()` decommits above a `request_arena_resident_floor` knob; the
  process byte counter then reflects genuinely-resident pages rather than
  reservations, so admission tightens automatically. A Windows port would
  need `VirtualAlloc(MEM_RESERVE)` + explicit `MEM_COMMIT` — a one-line
  `@note` when that port exists.
- **Why it's safe to defer.** `allocator()`/`try_reserve()`/`reset()`
  don't change shape — only `RequestArena`'s internals do. Nothing in
  `cql/engine`, `cql/native`, or the ambient hook changes when this lands.

---

### A4 — Arena-backed coroutine promise allocator

Depends entirely on A6 (the arena) and shares its gate — build only if the
benchmark shows non-elided promise frames are a measurable cost.

**Problem shape.** 184 `coroutine::Task<>` sites across `cql/engine` and
`cql/native`. Whether coroutines are the *right* structure is not the
question — they read well and the request-response shape maps cleanly onto
them. The problem is that any coroutine the C++ compiler does not HALO-elide
allocates its promise frame on the general heap. There is no
`coroutine::Task<>::promise_type::operator new` override.

**Redesign.** Override the coroutine promise's allocator so promise frames
come from the current thread's per-request arena (A6). Once
`threads::current_request_arena()` is maintained across every suspension,
this is one function:

```
void* Task<T,S>::promise_type::operator new(std::size_t sz) {
    return threads::current_request_arena()->allocate(sz);  // falls back to heap if null
}
```

Coroutines stay everywhere they are today. Async is not restructured. What
disappears is the per-suspend malloc pair for every non-elided frame, freed
in a single pointer bump at request end.

**Deletes.** Zero source-line deletions in the engine. Reduction in
per-query malloc traffic on suspending call chains — *if* the benchmark
shows there is traffic to reduce.

**Invariants.**
- `promise_type::operator new` reads the thread-local arena, which is
  per-request and reset when the response frame ships. A coroutine promise
  frame outliving its request is a bug (dangling pointer into freed arena).
- HALO remains an optimisation the compiler is free to apply.

**Verification.** Benchmark: prepared point-lookup on a warm cache
allocates ≤ 1 heap block (the response frame body) — the same target as
A6's arena win. Compile-time check: `coroutine::Task<>::promise_type` has
an `operator new` override.

**Note on scope.** Earlier drafts proposed rewriting the majority of the
184 coroutine sites to plain functions ("sync-default"). That was
overstated: HALO handles the elidable ones already, and this allocator
handles the rest. The pervasive-coroutine shape is a deliberate choice;
A4 makes it cheap, not different.
