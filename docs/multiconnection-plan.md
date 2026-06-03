# Multi-connection concurrency plan

## Problem

The engine and pager are shared across all connections. Connections use cooperative coroutines that yield at `co_await` points. Two connections can be executing concurrently and interleave freely at any yield point. This creates correctness hazards:

1. **Pager transaction exclusion** — `begin_transaction` asserts `!transaction_active`. If connection A yields inside a transaction (e.g., at `co_await btree::tfind`) and connection B then calls `begin_transaction`, the assert fires and the process crashes with SIGILL.

2. **Per-connection keyspace** — `engine.current_keyspace` was a single field shared by all connections. Fixed: `post_startup_loop` now owns an `AutoString8 conn_keyspace` and passes it by reference through `frame_handler` and all `engine::execute` / `engine::prepare` / `collect_bind_variables` overloads.

3. **Schema mutations** — `engine.schema` is read and written by DDL statements (CREATE/DROP/ALTER TABLE). A concurrent SELECT on another connection could observe a partially-modified schema.

---

## Phase 1 — Pager transaction serialization (write path)

**Goal:** allow at most one pager transaction to be active at a time; other writers wait asynchronously rather than crashing.

### Design

Add a mutex-like awaitable to `Pager`:

```cpp
// pager.cppm (additions)
struct Pager {
    ...
    bool transaction_active = false;
    DynamicArray<coroutine_handle<>> tx_waiters;  // queued writers
};
```

Replace the synchronous `begin_transaction` with a coroutine awaitable:

```cpp
// await_begin_transaction(pager) — suspends if transaction_active, resumes when clear
struct TransactionWaitAwaitable {
    Pager& pager;
    bool await_ready() const noexcept { return !pager.transaction_active; }
    void await_suspend(coroutine_handle<> h) { pager.tx_waiters.push(h); }
    void await_resume() {
        pager.saved_header = pager.header;
        pager.transaction_active = true;
    }
};
inline TransactionWaitAwaitable await_begin_transaction(Pager& pager) { return {pager}; }
```

`commit_transaction` and `rollback_transaction` must resume the next waiter:

```cpp
void finish_transaction(Pager& pager) {
    pager.transaction_active = false;
    if (!pager.tx_waiters.empty()) {
        auto h = pager.tx_waiters.pop_front();
        pager.saved_header = pager.header;
        pager.transaction_active = true;
        h.resume();
    }
}
```

`Transaction::begin()` becomes `co_await await_begin_transaction(*p)`.

### Correctness invariant

Because coroutines are cooperative, `finish_transaction` resumes the next waiter synchronously (no actual scheduling interleaving before it sets `transaction_active = true`). This is safe as long as the event loop does not nest coroutine resumptions in a re-entrant way.

### Limitations

- All writers are fully serialized (one at a time). This is the correct starting point for correctness.
- Reads (SELECT) also go through a transaction today (btree reads touch the pager). See Phase 2.

---

## Phase 2 — Read-only bypass

**Goal:** concurrent SELECTs do not block each other.

The pager WAL design supports read-from-last-checkpoint without a write lock. Reads only need the pager header (page count, root pages) to be stable during the read.

Options:
- A reader/writer lock: readers can proceed in parallel; a writer waits for all readers to finish.
- Snapshot reads: each reader takes a snapshot of the pager header at query start and reads pages from the checkpoint image; writers operate on the WAL and do not disturb the stable image.

The snapshot approach aligns better with the existing WAL architecture and avoids priority inversion between readers and a waiting writer.

Implementation sketch:
- Add `U64 reader_count` to `Pager`.
- SELECTs call `begin_read(pager)` (increments counter, copies header) and `end_read(pager)` (decrements, wakes a waiting writer if count reaches zero).
- `await_begin_transaction` additionally waits for `reader_count == 0`.

---

## Phase 3 — Schema concurrency

**Goal:** DDL statements do not corrupt concurrent readers.

`engine.schema` is a live in-memory structure. A CREATE TABLE modifies `schema.tables` while a concurrent SELECT may be iterating it.

Options:
- **Schema version / epoch**: bump a generation counter on any DDL. Readers check the counter before and after; retry on mismatch. Simple but requires making DDL rare or short.
- **Copy-on-write schema**: DDL operates on a private copy; atomic pointer swap replaces the active schema once the DDL commits. Readers hold a shared pointer to the old schema for the duration of their query.
- **DDL serialization**: serialize all DDL statements behind a separate DDL mutex. DDL and DML can then run concurrently as long as DDL completes atomically. This is the pragmatic starting point given low DDL frequency.

Recommended starting point: serialize DDL statements behind a `bool ddl_active` flag (same awaitable pattern as Phase 1) while leaving DML concurrent. Add schema version check on the read path and `assert_true` version has not changed during a SELECT scan.

---

## Shared state summary

| State | Owner | Concurrent-access issue | Fix |
|---|---|---|---|
| `engine.current_keyspace` | was Engine; now per-connection | race on USE + EXECUTE interleave | Fixed: `conn_keyspace` per `post_startup_loop` frame |
| `pager.transaction_active` | Pager | assert-crash if two writers overlap | Phase 1: awaitable queue |
| `engine.schema` | Engine | DDL + SELECT interleave | Phase 3: DDL serialization + version check |
| `engine.prepared_cache` | Engine | read-mostly; eviction could race | Low priority; fix when observed |
