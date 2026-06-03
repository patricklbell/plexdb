## Token Ring

Every table has a **partition key** (already represented by `primary_col_idx` in `schema::Table`). The partition key is hashed to a 64-bit **token** using a deterministic hash (xxhash64 via `plexdb::shard::token_of`). The token space `[0, 2^64)` is split into contiguous ranges, one per shard.

```
Token space:  0 ──────────────── 2^64
              |  Shard 0  |  Shard 1  |  ...  |  Shard N  |
```

With *N* shards the boundaries are simply `i * (2^64 / N)` for uniform distribution. A partition with token *t* is owned by shard `t / (2^64 / N)`.

Consistent hashing with virtual nodes is used when shards are added or removed so that only `1/N` of data migrates. Virtual nodes means each physical node `N` is given `V` virtual nodes beginning at a random point in the ring. Adding a node involves adding `V` virtual nodes, moving data from the existing ranges. Similarly for removing a node.

## Token Calculation

```
token(partition_key) = hash(partition_key)
owning_shard(token)  = token / (2^64 / N*V)
```

This runs in the coordinating shard (received the client connection). If the receiving shard does not own the token, it forwards the request.

## Request Routing

```mermaid
flowchart TB
    Recv["Shard receives request"]
    Parse["Parse, extract partition key"]
    Hash["token = hash(partition_key)"]
    Own{"Owns token?"}
    Exec["Execute locally"]
    Fwd["Forward via mailbox to owning shard"]
    Resp["Return response to client"]

    Recv --> Parse --> Hash --> Own
    Own -->|Yes| Exec --> Resp
    Own -->|No| Fwd
    Fwd -->|Response mailbox| Resp
```

### Scatter-gather (range scans, `SELECT *`)

Requests without a partition key constraint (full table scans) are fanned out to all shards. Each shard returns its local partition of rows. The coordinating shard merges results and responds. This is the slow path — queries should include a partition key whenever possible.

## Inter-Shard Communication

### Lock-Free SPSC Mailboxes

Between every ordered pair of shards `(i, j)` there is a **single-producer single-consumer (SPSC) ring buffer**. Shard *i* produces; shard *j* consumes. This gives `N*(N-1)` queues total — acceptable for typical core counts (e.g. 16 cores → 240 queues, each just a few cache lines of metadata each). For machines with more than ~64 cores, a hub-based topology (one coordinator shard that multiplexes messages) may need to replace the full mesh to reduce memory usage.

```
Shard 0 ──SPSC──▶ Shard 1
Shard 0 ──SPSC──▶ Shard 2
Shard 1 ──SPSC──▶ Shard 0
Shard 1 ──SPSC──▶ Shard 2
...
```

Each SPSC queue is a power-of-two ring buffer with atomic `head` (written by producer) and `tail` (written by consumer) indices on **separate cache lines** to avoid false sharing.

### Message Types

```
CrossShardRequest  { source_shard, request_id, statement, partition_key_bytes }
CrossShardResponse { request_id, execution_result }
SchemaChangeMsg    { raft_term, raft_index, schema_delta }
```

### Polling

Each shard's event loop polls its inbound SPSC queues alongside `io_uring` CQEs. A single `epoll`/`io_uring` iteration handles both network I/O and inter-shard messages:

```
loop:
    drain io_uring CQEs  → handle network events, file I/O completions
    drain SPSC inboxes   → execute forwarded requests, apply schema changes
    submit io_uring SQEs → new reads, writes, accepts
```

## Schema Consensus

Schema mutations (`CREATE KEYSPACE`, `CREATE TABLE`, `DROP`, `ALTER`) must be applied consistently across all shards. Assume all shards live in the same process for now.

- **Leader election**: the shard on core 0 starts as leader. If it dies, the lowest-numbered live shard takes over. Failure is detected by the absence of periodic heartbeat messages: each shard sends a heartbeat on its outbound SPSC queues at a fixed interval (e.g. 100ms). If a shard's inbound queue from the leader carries no heartbeat for a configurable timeout (e.g. 500ms), it considers the leader dead and initiates election.
- **Log replication**: the leader appends the schema mutation to its log, then pushes `SchemaChangeMsg` to all followers via SPSC queues.
- **Commit**: once a majority acknowledge (via response SPSC), the leader marks the entry committed and applies it locally. Followers apply on receiving the commit notification.
- **Persistence**: the leader writes the committed schema log to a dedicated schema pager page. On recovery, all shards replay the log.

Schema changes are rare relative to data operations. The Raft overhead (a few SPSC messages per DDL) is negligible.

### Failure Handling

- **Shard crash**: other shards detect via SPSC heartbeat timeout. The crashed shard's token range becomes temporarily unavailable (fail-fast). The main thread automatically restarts the shard on the same core — the new shard replays its WAL and re-joins the Raft group. Downtime for the affected token range is bounded by WAL replay time. This is acceptable for single-node deployments; multi-node deployments use replicas to avoid any downtime (see §6.3).
- **Full process crash**: on restart, each shard replays its WAL independently. Schema is recovered from the leader's committed log.
- **Multi-node**: Raft leader election handles node failure. Partition replicas on surviving nodes continue serving reads.

## Remaining design work
To match cassandra
- Replication
  - Tune replication factor (RF)
  - Per read/write consistency level
    - ONE → talk to 1 replica (fast, possibly stale)
    - QUORUM → majority of replicas (balanced)
    - ALL → all replicas (strongest, slowest)
  - Read repair and Anti-entropy repair (Merkle trees)
  - Hinted handoff
- Compare and set using paxos ([see](https://docs.datastax.com/en/cassandra-oss/3.0/cassandra/dml/dmlLtwtTransactions.html))

Additional features
- Generic Cross partition transactions
- Global ordering
  - e.g. read-time skew caused by a scatter read not being synchronized with per-shard operations.
