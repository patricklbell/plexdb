- Pager cache: consider page handle design as alternative to transaction-scoped arena cache
    - rpage/rwpage return a RAII handle that pins the cache entry; handle destruction releases the pin
    - On slot collision with a pinned entry, spill to arena overflow instead of evicting
    - Pro: rpage valid without transaction; pointers released early reclaim cache slots; memory bounded in common case
    - Con: two-tier cache lookup (slot array + arena overflow), handle lifetime management at all call sites
- Proper prepared (and non-prepared) query caching
- Avoid storing pager pointer per btree/blob
- Shard across cores
    - OS layer
        - Expose `sched_setaffinity` in `plexdb.os` for CPU pinning
        - Expose `SO_REUSEPORT` in `plexdb.os.socket`
    - Per-store shard coordinator
        - Detect core count, spawn pinned threads
        - Each thread: own `Pager`, `Engine`, `ThreadContext`, `io_uring Ring`
        - `SO_REUSEPORT` TCP listener per shard
        - Event loop: drain CQEs + drain SPSC inboxes + submit SQEs
    - Per-store request routing
        - Extract partition key (store-specific: CQL key, doc ID, vertex ID, …)
        - Hash to token via `plexdb::shard::token_of`
        - Map to shard via `plexdb::shard::owning_shard`
        - Local path: execute on receiving shard
        - Forward path: SPSC push to owning shard, await response
        - Scatter-gather for partition-unbound queries
    - Per-store schema consensus (Raft-Lite)
        - Leader on core 0, heartbeat via SPSC
        - Log replication: leader → all followers via SPSC
        - Commit on majority ack, apply on commit notification
        - Persist committed log to dedicated pager page
    - Abstract SPSC intra process communication, inter process (UDS), network communication
    - Recovery
        - Schema recovery from leader's committed log
- avoid allocation in iterator
- immutability check for cql frozen type
- COMPACT STORAGE: deprecated Cassandra 2.x wire format; currently accepted and ignored
- @perf Patch-supplied diff for collection-index maintenance. `update_indexes` diffs
  old/new column values element-by-element (O(n+m) on the nested loop in
  `engine.cpp`).
- Cassandra hashes partition keys with Murmur3 and orders partitions by token, not by raw
  PK bytes. plexdb currently orders by raw PK bytes inside the partition BTree, which is
  correct for point lookups and PK ranges but produces a different cross-partition order
  than Cassandra. Tests affected include `testIndexQueryWithCompositePartitionKey` and most
  multi-partition paging tests that observe ordering.
- `CUSTOM INDEX ... USING '...'` (SASI/SAI) and per-index `WITH OPTIONS`.** SASI and SAI
  are Cassandra-internal index implementations whose on-disk format and query semantics
  are tied to specific JVM-side data structures. Replicating them duplicates the role of
  the built-in B-tree index plus the collection indexes already in place, while adding
  large surface area for a single test category. Return `Invalid` with a clear error
  message and refuse the statement.
- Conditional BATCH and standalone LWT (`IF` on UPDATE / DELETE). Compare-and-swap
  semantics modeled on Paxos consensus. The unconditional Phase 9 path covers every
  unblocked BATCH test; LWT only unblocks a small number of conformance tests that all
  depend on multi-replica semantics plexdb is not designed for. Returning
  `assert_not_implemented` is the agreed behavior; standalone LWT applies the same
  rationale to single-statement `UPDATE … IF` / `DELETE … IF`.


# Dev notes
- aio proper separation between ownership, caller passes ring/ctx and arena
- Signal cannot interrupt in-progress `aio::drive` startup calls (pager init, engine init) — the signal notifier fd is not registered with `io_poll` until `create_notifier_consumer` is called, so a signal during e.g. a slow WAL recovery queues but does not abort the operation
- correlation or guarantees of primary key -> node mapping could improve conflict avoidance
