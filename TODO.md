- Pager algorithm insert/delete should be modified for varlen key case to use two passes which avoids pre-emptive splitting
- Pager cache: consider page handle design as alternative to transaction-scoped arena cache
    - rpage/rwpage return a RAII handle that pins the cache entry; handle destruction releases the pin
    - On slot collision with a pinned entry, spill to arena overflow instead of evicting
    - Pro: rpage valid without transaction; pointers released early reclaim cache slots; memory bounded in common case
    - Con: two-tier cache lookup (slot array + arena overflow), handle lifetime management at all call sites
- BTree prefix optimization: extend the BTree API to avoid serializing the full key
    - The CQL conformance plan (`docs/cql-conformance-plan.md`, R2) lands on keys using the
      same serialization as column values, compared by a type-aware comparator policy on
      `VarlenKeyPolicy` (`core/btree/btree.policy.cppm:84,98`). This deliberately gives up the
      byte-lexicographic prefix optimizations, which plexdb does not implement today anyway
      (separators are full key copies, `core/btree/btree.types.cppm:24`).
    - Prefix compression (front-code shared leading bytes of adjacent leaf keys) and separator
      suffix truncation (store only the shortest distinguishing prefix in internal-node
      separators) both shrink internal nodes → higher fanout → shallower tree → fewer page
      reads. Both need only the ability to compare/store a *partial* key, so the BTree API must
      stop assuming a separator is a full serialized key.
    - Byte-level (memcmp + order-preserving encoding, à la RocksDB/CockroachDB) truncates to the
      exact distinguishing byte; column-level (type-aware comparator, à la PostgreSQL PG-12
      attribute suffix truncation) can only drop whole trailing key columns. Byte-level packs
      tighter when individual column values are long and share prefixes.
    - Tradeoff to benchmark: memcmp-over-encoded-bytes comparison cost + order-preserving codec
      complexity (the `key.cppm` surface) vs. type-aware comparator cost on the hot path +
      weaker (column-granular) truncation. Need a key-comparison microbench and an
      internal-node-fanout/tree-depth measurement on representative composite keys (long text
      prefixes, wide clustering keys) before choosing.
    - Conformance constraint on any memcmp path: the order-preserving encoding must reproduce
      Cassandra's `AbstractType.compare` per clustering type, including the non-lexicographic
      ones — `timeuuid` by embedded timestamp (not raw bytes), `decimal`/`varint` numerically,
      `float`/`double` sign-magnitude. That codec is the cost R2's comparator avoids; a
      prefix-optimized memcmp path must pay it back.
- BTree hybrid comparator model: once the API no longer serializes full keys, allow the
  comparator policy to be chosen per table/index (`VarlenKeyPolicy`'s comparator is already a
  stateful template parameter). E.g. memcmp + order-preserving encoding for tables whose keys
  are long and prefix-heavy (best fanout), type-aware wire-format keys elsewhere (format
  unification, memcpy key projection). Also covers a PostgreSQL-style abbreviated-key fast path
  (fixed-width proxy compared first, full comparator on tie). Gated on the same benchmarks as
  the prefix-optimization item; only worth the two-path complexity if a workload shows one
  policy is not uniformly best.
- Proper prepared (and non-prepared) query caching
- CQL native protocol: stream multiplexing/pipelining. `post_startup_loop` awaits each
  frame's `frame_handler` to completion before reading the next frame
  (`cql/native/native.cppm:600,636`); stream IDs are parsed and echoed back correctly
  (`native.cpp:960` etc.) but never used to dispatch concurrent in-flight requests on one
  connection.
- Allow io_uring to submit multiple file io requests at once between connections. Currently only one connection
  can have a file op in flight because the transaction lock blocks it.
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
