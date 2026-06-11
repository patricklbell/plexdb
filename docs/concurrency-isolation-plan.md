# Concurrency and Isolation Plan

## Philosophy

plexdb's shared-nothing, per-shard architecture (one shard per core, disjoint token ranges) means **write throughput scales with shard count** — each shard owns its token range independently. Within a shard, a single write lock serializes writers; the cooperative coroutine scheduler ensures within-shard interleavings only occur at explicit `co_await` yield points (I/O operations).

The performance strategy for the early phases has two parts. First, minimize the write lock hold time: private write buffers keep the transaction body lock-free; non-suspending cache reads eliminate yield points on the warm-cache path. Second, amortize the unavoidable I/O cost across concurrent clients: group commit batches multiple transactions' WAL records into one fsync; batched io_uring submission lets NVMe service multiple concurrent reads in parallel. Reads are decoupled from writes via committed-state snapshots, so read and write throughput scale independently.

Rather than picking a single isolation level and baking it in, this plan layers isolation so users can opt into stronger (more expensive) guarantees only where they need them. This mirrors Cassandra/ScyllaDB's approach: default to fast and loosely ordered, expose stronger primitives (LWT, timestamps) as explicit opt-ins.

---

## Isolation taxonomy (Kleppmann terminology)

| Level | Hazards prevented | Cost | plexdb mechanism |
|---|---|---|---|
| Read Uncommitted | — | Free | Not offered |
| Read Committed | Dirty reads | Low | Snapshot read from last committed pager state |
| Snapshot Isolation (SI) | Dirty/non-repeatable reads, read skew | Low–medium | Per-query pager header snapshot |
| Serializable (per shard) | Write skew, phantoms (within shard) | Low* | Serial execution — free with cooperative scheduler |
| Linearizable (per partition) | Stale reads, lost updates across clients | Medium | Per-partition LWT via Paxos |
| Causal consistency | Cross-partition ordering | Low | Lamport timestamps propagated via SPSC queues |
| Serializable (cross-partition) | Full ACID across partitions | High | Stretch goal: 2PC + Paxos |

\* Free because within a shard the scheduler is cooperative and write operations on a partition are sequential once the write lock is held.

---

## Phase 1 — Concurrent reads, group commit, and I/O batching

**Goal:** reads never block behind writers; write throughput scales with batch depth rather than one fsync per transaction; NVMe I/O parallelism via batched SQE submission.

The primary target is the cassandra-stress workload: 16+ concurrent threads issuing point writes and reads against distributed keys. For this workload the bottleneck is fsync latency per transaction (writes) and read/write lock contention (mixed). Group commit and concurrent snapshot reads address these directly.

### Non-suspending cache reads

Every `co_await read_page(...)` call in the btree and engine is a potential context switch. Most hit the page cache — suspending for them is unnecessary overhead. The existing `Awaitable<OnSuspend, OnResume>` in `coroutine.base` always suspends. A minimal extension — `MaybeAwaitable<OnReady, OnSuspend, OnResume>` — adds an `on_ready` predicate that skips suspension when the result is immediately available:

```cpp
// coroutine/base.cppm — addition alongside Awaitable
template<typename OnReady, typename OnSuspend, typename OnResume>
struct [[nodiscard]] MaybeAwaitable {
    OnReady   on_ready;
    OnSuspend on_suspend;
    OnResume  on_resume;

    bool await_ready() noexcept { return on_ready(); }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        on_suspend(h);
    }
    decltype(auto) await_resume() noexcept {
        return on_resume();
    }
};
template<typename OnReady, typename OnSuspend, typename OnResume>
MaybeAwaitable(OnReady, OnSuspend, OnResume) -> MaybeAwaitable<OnReady, OnSuspend, OnResume>;
```

Page reads in the pager become:

```cpp
Page* cached = nullptr;
co_await MaybeAwaitable{
    [&]{ return (cached = pager.cache.get(page_id)) != nullptr; },
    [&](std::coroutine_handle<> h) { pager.io.read_async(page_id, h); },
    [&]() -> Page* { return cached ? cached : pager.cache.get(page_id); }
};
```

A cache-warm transaction then has no yield points in its body at all: it runs to commit as a single uninterrupted execution, holding the write lock for the minimum possible time.

### Private write buffer and dirty page coalescing

Writes during a transaction accumulate in a per-transaction in-memory buffer rather than going directly to the WAL. This decouples btree modification from I/O:

- **Transaction body:** reads from cache (no yield for hits), writes to private buffer (no I/O). Write lock is held but no coroutine switch occurs.
- **Commit:** dirty pages are flushed from the buffer to the WAL in one batch (yields here — unavoidable), then fsync, then lock released.

Multiple writes to the same page within one transaction coalesce: only the final version is emitted to the WAL, reducing write amplification independently of batching.

**Spill threshold:** if the dirty page buffer exceeds a configurable limit (e.g. 64 pages), pages are flushed to the WAL early with the write lock still held. This is the degraded path for large transactions.

### Group commit

When transaction A is waiting for its WAL fsync, transactions B and C may have completed their btree work and be queued to commit. Group commit batches them:

1. A finishes btree work, moves dirty pages to the group committer, begins WAL flush (yields).
2. B and C finish their btree work and join the committer: their dirty pages are appended to the pending set and they suspend.
3. A's WAL write completes. Before fsyncing, A includes all pending dirty pages from B and C in the same WAL batch.
4. One fsync. A advances `committed_header`, wakes B and C. All three complete simultaneously.

```cpp
struct GroupCommitter {
    bool flush_in_progress = false;
    DirtyPageBuffer pending_dirty;
    DynamicArray<coroutine_handle<>> pending_waiters;
};
```

The transaction that wins the flush race writes all accumulated dirty pages as a single `writev()` to the WAL — one syscall, one sequential write for multiple transactions' records, one fsync. Transactions that join an in-progress flush pay zero I/O cost.

```
Connection A:  [btree work, no yields] [WAL writev+fsync, yields] [wake B+C] [done]
Connection B:  [btree work, no yields] [joins pending, suspends  ] [          woken] [done]
Connection C:  [btree work, no yields]          [joins pending, suspends] [  woken] [done]
```

**Throughput model:** fsync latency (≈50–200 µs on NVMe) is paid once per batch. With 16 concurrent writers, each fsync round-trip amortizes across all queued transactions, multiplying effective write TPS proportionally.

### Batched I/O submission (NVMe parallelism)

io_uring allows multiple SQEs to be queued before a single `io_uring_submit()` syscall. The event loop should collect all pending I/O requests within one iteration before submitting:

```
event loop iteration:
  1. process_completions()   — drain CQEs, resume waiting coroutines
  2. resume_ready()          — run resumed coroutines; they may queue new SQEs
  3. io_uring_submit()       — flush all queued SQEs in one syscall
  4. io_uring_wait_cqe()     — block until next completion
```

This batching applies to:
- **Read misses:** concurrent transactions waiting for different pages queue their read SQEs; NVMe services them in parallel within the device's internal queue.
- **WAL writes:** group commit's `writev` and fsync are submitted together; reads from unblocked transactions queue behind them in the same ring without extra syscalls.

For read-heavy workloads (cassandra-stress read), the event loop naturally groups all in-flight reads from concurrent connections into one submit call per iteration. For write-heavy workloads, group commit's single `writev` per batch already minimises WAL write syscalls.

### Snapshot reads for concurrent SELECTs

The WAL separates committed state (main database file) from in-flight state (WAL + private write buffer). Readers can safely read from the last committed snapshot:

- A **reader** snapshots `committed_header` at query start. All btree reads go through committed pages only.
- A **writer** holds the write lock and works in its private buffer; readers never see in-flight changes.
- On commit, the writer atomically advances `committed_header` after flushing and fsyncing.

```cpp
// Pager additions
PagerHeader committed_header;   // advanced only on commit
U32 reader_count = 0;

ReadSnapshot begin_read(Pager& pager) {
    ++pager.reader_count;
    return { pager.committed_header, pager };
}

void end_read(ReadSnapshot& snap) {
    if (--snap.pager.reader_count == 0)
        maybe_wake_pending_writer(snap.pager);
}
```

`await_begin_transaction` waits for `reader_count == 0` before starting a write, so the committed snapshot remains stable for active readers until they finish.

### Schema mutations under snapshot reads

DDL holds the `ddl_active` flag for its duration. Readers capture the current `schema_epoch` at query start. If the epoch changes during a scan, the query retries (DDL is rare; retries are negligible).

### Isolation level provided

- Reads: **Snapshot Isolation** — a SELECT sees a consistent point-in-time view; it never observes partial writes from a concurrent writer.
- Writes: **Serializable** within a shard — writers still execute one at a time, but the critical section is I/O-only and the I/O cost is shared across all queued writers.
- Cross-shard: **Read Committed** — scatter-gather SELECTs fan out per-shard snapshot reads. No global snapshot across shards (see Phase 3 for causal reads).

---

## Phase 2 — Per-partition linearizable operations (LWT)

**Goal:** compare-and-set and read-modify-write operations that are linearizable within a single partition, across concurrent clients and across nodes.

### Mechanism: Paxos per partition

Each partition (identified by its token) runs an independent Paxos instance. This is the same approach used by Cassandra LWT and ScyllaDB. Because tokens map to exactly one owning shard, the Paxos leader is always the owning shard — no cross-shard coordination is needed for the common case.

**CQL surface (existing syntax, implementing correct Cassandra semantics):**

```sql
-- Conditional insert (IF NOT EXISTS)
INSERT INTO orders (id, status) VALUES (uuid(), 'pending') IF NOT EXISTS;

-- Conditional update (IF <condition>)
UPDATE accounts SET balance = 900 WHERE id = 42 IF balance = 1000;
```

Both return a `[applied]` boolean column. These go through Paxos:
1. **Prepare** — owning shard broadcasts a prepare with ballot to all replicas (once replication exists; on single node, trivially self-prepares).
2. **Promise / read** — read current value.
3. **Propose** — send conditional write.
4. **Commit** — apply if condition holds.

For single-node deployments with no replication, Paxos degenerates to a simple in-memory CAS on the owning shard with a per-partition lock (no network round trips needed).

### OCC with user-managed version column

For optimistic read-modify-write patterns, users add an explicit version column and use the existing `IF condition` syntax:

```sql
SELECT version, balance FROM accounts WHERE id = 42;
-- returns version=7, balance=1000

UPDATE accounts SET balance = 900, version = 8 WHERE id = 42 IF version = 7;
-- succeeds only if no concurrent write has occurred since the read
```

No hidden system columns are introduced — this is standard CQL and works with existing drivers and conformance tests. The `IF condition` check and the Paxos path underneath give linearizability per partition.

### Isolation level provided

**Linearizability per partition** — operations on a single partition appear to take effect at a single point in real time. Concurrent clients see a total order.

---

## Phase 3 — Causal consistency via Lamport timestamps

**Goal:** cross-shard and cross-node causal ordering without a global serialization point.

### Lamport clock per shard

Each shard maintains a logical clock `U64 lamport_clock`. Every operation (read or write) increments the clock. Inter-shard messages (via SPSC queues) carry the sender's clock; the receiver advances to `max(local, received) + 1`.

### Client-facing timestamp API

`USING TIMESTAMP <value>` on writes is existing CQL syntax that sets the cell-level write timestamp for last-write-wins conflict resolution. Its semantics are unchanged.

For causal reads, new syntax is introduced on SELECT — `AFTER TIMESTAMP <value>` — which does not exist in CQL today:

```sql
-- Write: server returns the Lamport timestamp at which the write committed
-- (new response metadata field, no change to existing USING TIMESTAMP semantics)
INSERT INTO events (id, payload) VALUES (...);
-- server returns in response metadata: commit_timestamp=<lamport_ts>

-- Causal read: new syntax, stalls until shard clock >= lamport_ts
SELECT * FROM events WHERE id = ... AFTER TIMESTAMP <lamport_ts>;
```

The `AFTER TIMESTAMP T` clause stalls the coroutine (yields) until the shard's local Lamport clock reaches `T`. Because the SPSC message delivering the write also advances the receiving shard's clock, any read with `T` on any shard that has processed the message will see the write. This gives **causal consistency** (read-your-writes, monotonic reads, writes follow reads) without a global lock.

### Scatter-gather with timestamps

For a full-table scan across all shards, the coordinator issues reads to all shards with a common timestamp `T` (the max seen by the client). Each shard stalls until clock ≥ `T` before returning results. The coordinator merges. This prevents read-time skew.

### Isolation level provided

**Causal consistency** — all operations that causally precede a write are visible to reads that observe that write. Concurrent writes are not ordered (conflicts are possible; use Phase 2 LWT to detect them).

---

## Phase 4 (stretch) — Cross-partition atomic batches

**Goal:** atomically apply writes to multiple partitions without full distributed serializability.

This maps to Cassandra's **logged batch**: the coordinator writes a batch log entry before forwarding mutations. On failure, a batch replay mechanism ensures all mutations are eventually applied. This is **atomic** (all-or-nothing from a durability perspective) but **not isolated** — other readers may observe partial application during the batch.

CQL syntax (existing):
```sql
BEGIN BATCH
  INSERT INTO orders (id, status) VALUES (1, 'created');
  UPDATE inventory SET qty = qty - 1 WHERE sku = 'ABC';
APPLY BATCH;
```

Implementation sketch:
1. Coordinator shard writes batch to a dedicated batch-log table (local pager).
2. Forwards each mutation to its owning shard via SPSC mailbox.
3. Each shard executes independently (no coordination for isolation).
4. Once all shards acknowledge, coordinator marks batch log committed.
5. On recovery, uncommitted batch logs are replayed.

For serializable cross-partition transactions (e.g. Percolator-style), add a separate opt-in syntax and implement as a future phase. This requires 2PC and is intentionally left unscoped here given complexity concerns.

---

## Configuration API

`CONSISTENCY` is an existing CQL keyword that controls replication (how many replicas must agree). It is left unchanged. Isolation behaviour is controlled through separate new syntax that does not overlap with any existing CQL keywords.

### New syntax: `SET ISOLATION LEVEL`

A new session-level statement (not in Cassandra CQL):

```sql
-- default: fast path
SET ISOLATION LEVEL DEFAULT;

-- all writes on this connection go through the Paxos LWT path
SET ISOLATION LEVEL LINEARIZABLE;
```

### New syntax: statement-level isolation hint

```sql
-- SELECT with a causal barrier (new AFTER TIMESTAMP clause)
SELECT * FROM t WHERE pk = 1 AFTER TIMESTAMP <lamport_ts>;

-- Conditional writes use existing IF syntax — no new syntax needed
UPDATE t SET v = 2 WHERE pk = 1 IF v = 1;
```

Internally this maps to:

| Mechanism | Trigger | Guarantee |
|---|---|---|
| Phase 1 (default) | all reads/writes | SI reads, serializable writes per shard |
| Phase 2 Paxos LWT | `IF` clause on write, or `SET ISOLATION LEVEL LINEARIZABLE` | Linearizable per partition |
| Phase 3 Lamport stall | `AFTER TIMESTAMP <ts>` on SELECT (new syntax) | Causal consistency |
| Phase 4 logged batch | `BEGIN BATCH ... APPLY BATCH` (existing syntax) | Atomic (not isolated) cross-partition |

---

## Multi-node considerations

The plan maps cleanly onto multi-node because each shard (core) already maps to a token range:

- **Phase 1** — purely intra-shard; no change for multi-node.
- **Phase 2 LWT** — Paxos instance per partition; on multi-node, add replication factor to the quorum calculation. Single-node degenerates to a no-network CAS.
- **Phase 3 Lamport** — SPSC queues (intra-node) become TCP/io_uring connections (inter-node). Lamport clock propagation is identical; just adds network latency to the stall.
- **Phase 4 batches** — coordinator writes batch log locally; inter-node delivery uses the same message types as the partitioning plan's `CrossShardRequest`.

Linearizability across the whole cluster (every read sees the globally latest write) requires Raft consensus or a global sequencer and is not offered by default. It can be added per-partition once replication is implemented (RF > 1, QUORUM reads+writes gives linearizability per Cassandra's rule).

---

## Shared state summary

| State | Issue | Fix | Phase |
|---|---|---|---|
| `engine.schema` | DDL + SELECT interleave | `ddl_active` flag + schema epoch + retry | 1 |
| `pager committed_header` | readers see in-flight writes | snapshot from `committed_header` | 1 |
| `GroupCommitter` | multiple writers pay per-transaction fsync | batch dirty pages, single writev+fsync | 1 |
| io_uring submission | one SQE submit per I/O operation | batch SQEs per event loop iteration | 1 |
| per-partition row version | lost updates across clients | user-managed version column + LWT | 2 |
| shard `lamport_clock` | cross-shard causal ordering | propagate via SPSC / network messages | 3 |
| `engine.prepared_cache` | eviction race | low priority; serialize eviction path | — |

---

## What is intentionally out of scope

- **Optimistic concurrency control (OCC) with read/write set tracking** — OCC helps when transaction body lock contention is the bottleneck. For the target workload (cassandra-stress, warm cache, distributed keys), transaction bodies on a warm cache have no yield points and already complete atomically; the bottleneck is fsync latency, not body contention. Group commit addresses the actual bottleneck. Revisit if profiling shows contention on hot keys.
- **Serializable Snapshot Isolation (SSI) with read/write set tracking** — requires full MVCC with conflict detection at commit time. Too complex for the current pager design. Revisit if the pager is refactored to support multiple concurrent write transactions.
- **Two-phase locking (2PL) across shards** — defeats the shared-nothing design. Not offered.
- **Global linearizability** — requires a global sequencer or full Raft consensus across all shards. Offered only per-partition via LWT.
- **Write-heavy workload optimization beyond group commit** — serial execution within a shard bounds write throughput per shard. If write throughput remains the bottleneck after group commit, the correct solution is more shards (more cores / more nodes), not concurrent writers to the same btree.
