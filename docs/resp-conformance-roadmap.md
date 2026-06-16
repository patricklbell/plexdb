# RESP Keyvalue Conformance Roadmap

## Current State

Running `tools/resp_tests/run.sh` against `plexdb_keyvalue_server` (as of 2026-06-15):

- **46 passed** / **353 failed** / **28 errors** / **8 skipped** (435 total)
- Passing: PING, GET, SET (NX/XX only), DEL, EXISTS, MGET, MSET, KEYS, SCAN, DBSIZE,
  TYPE, FLUSHDB, FLUSHALL, CLIENT (stub), INFO (basic), COMMAND (empty array), SELECT (0 only)

## Goals

Reach 100% passing on `redis-py v4.6.0` test_commands.py by implementing the full
Redis command set across all data types and connection-level features.

Run conformance tests with the log plugin for crash diagnostics:
```bash
ninja -C build/debug plexdb_keyvalue_server
bash tools/resp_tests/run.sh \
  -b build/debug/keyvalue/plexdb_keyvalue_server \
  -L /tmp/resp_logs
cat /tmp/resp_logs/server.log
```

---

## Phase 0 — Test Infrastructure

Several tests ERROR because `conftest.py` does not define fixtures that the upstream
test file expects. Fix conftest.py before implementing any server changes.

### 0a. Missing mock cluster fixtures

All `test_cluster_*` tests use `mock_cluster_resp_ok`, `mock_cluster_resp_int`,
`mock_cluster_resp_info`, `mock_cluster_resp_nodes`, and `mock_cluster_resp_slaves`
fixtures. These tests mock the redis-py client's `execute_command` to return
predetermined responses; they do not communicate with the server at all. The tests are
unit tests for the redis-py client's response-parsing layer, not integration tests.

Add these fixtures to `conftest.py` using `mock.patch` or a subclass of `redis.Redis`
that intercepts `execute_command` and returns the appropriate type
(`True`, `int`, info-dict, nodes-dict, slaves-dict). Port the fixture definitions
from redis-py's upstream conftest.

### 0b. Replica-only tests

`test_sync` and `test_psync` are decorated `@pytest.mark.replica` and connect directly
to port 6380 (a separate replica process). These tests are gated on the
`pytest.mark.replica` mark. Add a conftest option `--no-replica` that registers the mark
as a skip reason, and make the session default to skipping replica tests when no replica
binary/port is provided. Alternatively, implement the full replication protocol
(see Phase 17) and provide a second server instance in the conftest.

### 0c. ACL tests that ERROR

Several `test_acl_*` tests ERROR during fixture setup because previous tests crash the
server (or because `r.flushall()` in `resp_test_isolation` returns an unexpected type
when the server returns an ACL error). The root fix is implementing ACL properly
(Phase 15), but isolating the crash recovery logic in `resp_test_isolation` to be more
tolerant will help unblock intermediate runs.

---

## Phase 1 — Engine Foundation

These changes are prerequisites for almost every later phase.

### 1a. Binary-safe key storage

**Problem:** The engine stores `hash(key)` as the BTree key
(`FixedKeyPolicy<U64>`). Different string keys can collide; binary keys with identical
hashes silently corrupt data. `test_binary_get_set` exposes this.

**Fix:** Migrate `InMemoryEngine` and `PagedEngine` to variable-length BTree keys using
the actual key bytes. The BTree already supports variable-length key policies; update
`engine.cppm` to use the appropriate `VarKeyPolicy` so key comparison is byte-exact.
The blob then stores only the value; the key lives in the BTree node.

### 1b. Multiple database support

**Problem:** `SelectDb` asserts-not-implemented for any index > 0, crashing the server.

**Fix:** The engine holds a `DynamicArray` (up to 16 entries) of independent
`InMemoryEngine` or `PagedEngine` instances. `SELECT n` switches the active database
index on the current connection. Each connection tracks its current database index in
`ConnectionState`. `FLUSHALL` iterates all databases.

### 1c. TTL infrastructure

**Problem:** No TTL tracking exists. All expire-related commands fail.

**Fix:** Attach an optional expiry (Unix milliseconds, `0` meaning no expiry) alongside
every stored entry. Store it in the BTree value: `[type: U8][expiry_ms: U64][blob_id: U64]`.

Two eviction strategies, both required:
- **Lazy eviction:** Every key access checks the stored expiry; if elapsed, delete the
  entry and return "not found".
- **Active eviction:** A background coroutine wakes every ~100 ms and samples a fraction
  of keys with non-zero expiries, deleting those that have elapsed. Mirrors Redis's
  probabilistic active expiry model.

### 1d. Per-connection state

**Problem:** CLIENT ID, SETNAME, GETNAME, LIST, INFO require per-connection metadata.

**Fix:** Add a `ConnectionState` struct to `resp.cppm` holding:
- `U64 id` — assigned from a global atomic counter at connection open
- `AutoString8 name` — set by CLIENT SETNAME
- `U64 db_index` — current SELECT target
- `U64 flags` — normal/pubsub/blocked/…
- `U64 created_at_ms`, `U64 last_cmd_at_ms`, `AutoString8 peer_addr`
- `bool no_evict` — set by CLIENT NO-EVICT
- tracking state for CLIENT TRACKING

A global connection registry (mutex-protected) lets CLIENT LIST iterate live connections.

### 1e. Type tagging

Every stored entry needs a `type` byte in the BTree value (see 1c layout above). Values:
`0=string, 1=list, 2=set, 3=zset, 4=hash, 6=stream, 14=hll, 15=geo`.

- `TYPE key` reads the tag.
- `SCAN TYPE filter` compares it during iteration.
- Every type-specific command verifies the tag and returns `WRONGTYPE` if it doesn't match.

---

## Phase 2 — String Commands

Implement all Redis string-type commands operating on the `string` type tag.

| Command group | Commands |
|---|---|
| Atomic counter | `INCR`, `INCRBY`, `INCRBYFLOAT`, `DECR`, `DECRBY` |
| String mutation | `APPEND`, `SETRANGE` |
| String read | `STRLEN`, `GETRANGE` / `SUBSTR` |
| Atomic get+set | `GETSET`, `GETDEL`, `GETEX` |
| Conditional set | `SETNX`, `SETEX`, `PSETEX` |
| SET extensions | `SET … EX / PX / EXAT / PXAT / KEEPTTL / GET` flags |
| Multi-set NX | `MSETNX` |

Counter commands read the current value, parse it as an integer or float (returning
`ERR not an integer` / `ERR not a float` / range-overflow errors appropriately), mutate,
and write back. `INCRBYFLOAT` stores the result as a minimal-precision decimal string.
`GETEX` applies TTL changes atomically with the read (`EX`, `PX`, `EXAT`, `PXAT`,
`PERSIST` sub-options). `SET … GET` returns the old value before overwriting.

---

## Phase 3 — TTL Commands

Implement all Redis key-expiry commands (depends on Phase 1c TTL infrastructure).

| Command | Notes |
|---|---|
| `EXPIRE key seconds [NX\|XX\|GT\|LT]` | Set TTL; conditional flags apply when condition holds against current TTL |
| `PEXPIRE key ms [NX\|XX\|GT\|LT]` | Millisecond variant |
| `EXPIREAT key unix-time-seconds [NX\|XX\|GT\|LT]` | Absolute Unix timestamp |
| `PEXPIREAT key unix-time-ms [NX\|XX\|GT\|LT]` | Absolute millisecond timestamp |
| `EXPIRETIME key` | Returns absolute Unix time of expiry, −1 (no expiry), or −2 (missing key) |
| `PEXPIRETIME key` | Millisecond variant |
| `TTL key` | Returns seconds remaining, −1 (no expiry), or −2 (missing key) |
| `PTTL key` | Millisecond variant |
| `PERSIST key` | Removes TTL; returns 1 if TTL was removed, 0 if key has no TTL |

---

## Phase 4 — Key Management Commands

| Command | Notes |
|---|---|
| `ECHO message` | Return message as bulk string |
| `RANDOMKEY` | Return a uniformly random key from the current DB; nil if empty |
| `RENAME src dst` | Atomic rename; transplant TTL and type; delete dst first; error if src missing |
| `RENAMENX src dst` | Like RENAME but return 0 if dst already exists, no mutation |
| `COPY src dst [DB n] [REPLACE]` | Deep-copy value, type tag, and TTL to destination key, optionally in another database |
| `UNLINK key [key …]` | Delete index entries synchronously, free blobs in a background coroutine |
| `OBJECT ENCODING key` | Derive encoding name from type+size: "int", "embstr", "raw", "listpack", "ziplist", "skiplist", "quicklist", etc. |
| `OBJECT REFCOUNT key` | Return 1 (all entries have a single owner) |
| `OBJECT IDLETIME key` | Return seconds elapsed since `last_cmd_at_ms` was last written for this key |
| `OBJECT FREQ key` | Return the LFU decay counter maintained per-entry (requires LFU field in BTree value) |
| `OBJECT HELP` | Return help text array |
| `DUMP key` | Serialize value as a Redis RDB payload (version-tagged binary blob) |
| `RESTORE key ttl payload [REPLACE] [ABSTTL] [IDLETIME n] [FREQ n]` | Deserialize RDB payload; set TTL; REPLACE overwrites existing key |
| `SORT key [BY …] [LIMIT …] [GET …] [ASC\|DESC] [ALPHA] [STORE dst]` | Sort list/set/zset elements with pattern-based BY and GET, optional destination STORE |

**SCAN extension:** Add `TYPE type` filter parameter; return only keys whose type tag
matches. Implement `COUNT` hint as a scan-step-size hint (not a hard limit).

**DUMP/RESTORE:** Implement the Redis RDB serialization format for each value type
(strings, lists, sets, zsets, hashes, streams, HLL, geo). Emit and accept RDB version 11.
The RESTORE `IDLETIME` and `FREQ` options write the corresponding per-entry fields.

---

## Phase 5 — Server / Connection Commands

| Command | Notes |
|---|---|
| `AUTH [user] password` | Single-user password model (set via `requirepass` config); error if no password set and password supplied; ACL multi-user via Phase 15 |
| `TIME` | Return `[seconds, microseconds]` derived from wall clock |
| `LASTSAVE` | Return Unix timestamp of the last completed `BGSAVE` (or server start if none) |
| `BGSAVE [SCHEDULE]` | Trigger a background save of the paged engine to disk; return "Background saving started"; update LASTSAVE on completion |
| `RESET` | Reset connection to initial state: db=0, name cleared, tracking off, pubsub unsubscribed; return simple string "RESET" |
| `ROLE` | Return `["master", replication_offset, replicas_list]` reflecting actual replication state |
| `LOLWUT [VERSION n]` | Print an ASCII art pattern (any deterministic pattern is acceptable) followed by `\nRedis ver. 7.0.0\n` |
| `WAIT numreplicas timeout` | Block until `numreplicas` replicas have acknowledged writes up to the current replication offset, or timeout elapses; return the count of replicas that acknowledged |

**CONFIG:** Implement `CONFIG GET pattern`, `CONFIG SET param value [param value …]`,
`CONFIG RESETSTAT`. Maintain an in-memory config map populated with all standard Redis
parameters and their current values. `GET` returns matching key-value pairs. `SET`
updates the map and applies changes to live server state where applicable (e.g.
`requirepass`, `hz`, `maxmemory`). `RESETSTAT` zeroes all stat counters (commands
processed, keyspace hits/misses, etc.).

**INFO:** Extend to support section arguments (`server`, `clients`, `memory`, `stats`,
`replication`, `cpu`, `keyspace`, `all`, `everything`, `default`). Populate each
section with real values derived from live server state: uptime, connected clients,
used memory (from allocator), keyspace per-DB key count and expiry count, replication
role and offset, etc.

**COMMAND:** Build a static command-metadata table registering every implemented command
with its arity, flags (read/write/admin/…), first/last/step key positions, ACL
categories, and argument documentation. Use this table to implement:
- `COMMAND COUNT` — return `len(command_table)`
- `COMMAND LIST [FILTERBY MODULE m | ACLCAT cat | PATTERN pat]` — filter the table
- `COMMAND GETKEYS cmd arg…` — extract key positions using the registered key spec
- `COMMAND DOCS [cmd …]` — return per-command documentation (raises `NotImplementedError`
  on the client side for this redis-py version; the server should return the error that
  triggers that exception)
- `COMMAND` (no subcommand) — return full metadata map

**SLOWLOG:** Implement a slow-query log ring buffer. Record every command whose
execution time exceeds `slowlog-log-slower-than` microseconds (configurable via
`CONFIG SET`). Implement `SLOWLOG GET [n]`, `SLOWLOG LEN`, `SLOWLOG RESET`.

**MEMORY:** Implement:
- `MEMORY USAGE key [SAMPLES n]` — traverse the value blob and compute the byte size
  of the stored data (key length + value encoding overhead); return as integer
- `MEMORY STATS` — return a dict of allocator and server memory statistics populated
  from OS-level memory info and per-DB key counts
- `MEMORY MALLOC-STATS` — return a string from the allocator's internal stats (e.g.
  from `malloc_info` or `mallinfo2` on Linux)
- `MEMORY DOCTOR` — return a health analysis string based on fragmentation ratio and
  other memory metrics
- `MEMORY HELP` — return the help text array

**LATENCY:** Implement a latency monitor that records sampled command latencies:
- `LATENCY LATEST` — return a list of `[event, latest_time, latest_ms, max_ms]`; empty
  if no events have fired
- `LATENCY HISTORY event` — return a list of `[timestamp, latency_ms]` samples; empty
  for unknown events
- `LATENCY RESET [event …]` — reset the monitor, return the count of events cleared

---

## Phase 6 — CLIENT Commands

Depends on Phase 1d (per-connection state).

| Command | Notes |
|---|---|
| `CLIENT ID` | Return the connection's integer ID |
| `CLIENT GETNAME` | Return current name (nil bulk string if unset) |
| `CLIENT SETNAME name` | Set name; validate: no spaces, printable ASCII |
| `CLIENT INFO` | Return the formatted CLIENT LIST entry for this connection |
| `CLIENT LIST [TYPE type] [ID id …]` | Iterate the connection registry; format each as `id=… addr=… fd=… name=… age=… idle=… flags=… db=… sub=… psub=… multi=… qbuf=… qbuf-free=… argv-mem=… multi-mem=… rbs=… rbp=… obl=… oll=… omem=… tot-mem=… redir=… user=…\n` |
| `CLIENT KILL [ID id] [ADDR addr] [LADDR addr] [USER user] [SKIPME yes/no]` | Close all matching connections; return count killed |
| `CLIENT PAUSE timeout [WRITE\|ALL]` | Pause processing of write commands (or all commands) for `timeout` milliseconds |
| `CLIENT UNPAUSE` | Resume paused clients immediately |
| `CLIENT NO-EVICT ON\|OFF` | Set the no-evict flag on the connection's `ConnectionState`; the memory eviction logic checks this flag before evicting keys on behalf of this client |
| `CLIENT GETREDIR` | Return the client ID of the tracking redirect target, or −1 if none |
| `CLIENT TRACKING ON\|OFF [REDIRECT id] [PREFIX prefix …] [BCAST] [OPTIN] [OPTOUT] [NOLOOP]` | Enable server-assisted client-side caching: record which keys this client reads; when those keys are written by any client, send an invalidation message to this client (or to the redirect target) via a push message |
| `CLIENT TRACKINGINFO` | Return a dict describing current tracking state: flags, redirect, prefixes |
| `CLIENT UNBLOCK id [TIMEOUT\|ERROR]` | Unblock a client that is blocked on a list/stream command |
| `CLIENT REPLY ON\|OFF\|SKIP` | Control whether the server sends replies; `SKIP` suppresses the next reply; `OFF` suppresses all replies until `ON` |

---

## Phase 7 — List Data Structure

Add the `list` type. Store lists as a length-prefixed sequence of bulk-string elements
in a blob. For small lists a flat blob is acceptable; for large lists a skip-list or
doubly-linked paged structure is preferred.

| Command group | Commands |
|---|---|
| Push | `LPUSH`, `RPUSH`, `LPUSHX`, `RPUSHX` |
| Pop | `LPOP [count]`, `RPOP [count]` |
| Read | `LLEN`, `LRANGE`, `LINDEX` |
| Mutation | `LINSERT BEFORE\|AFTER`, `LSET`, `LTRIM`, `LREM` |
| Move | `LMOVE src dst LEFT\|RIGHT LEFT\|RIGHT`, `RPOPLPUSH` |
| Multi-pop | `LMPOP numkeys key … LEFT\|RIGHT [COUNT n]` |
| Position search | `LPOS key element [RANK n] [COUNT n] [MAXLEN n]` |
| Blocking | `BLPOP key … timeout`, `BRPOP key … timeout`, `BLMOVE`, `BLMPOP` — block up to `timeout` seconds waiting for an element; return nil-array on timeout |
| LCS | `LCS key1 key2 [LEN] [IDX] [MINMATCHLEN n] [WITHMATCHLEN]` — longest common subsequence of two string values |

`SORT key … STORE dst` and the unblocking side of `BLPOP`/`BRPOP` also land here.

---

## Phase 8 — Hash Data Structure

Add the `hash` type. Encode as a sequence of field-length-prefixed pairs in a blob.
For large hashes, a nested BTree keyed by field name is preferred.

| Command group | Commands |
|---|---|
| Write | `HSET key field value [field value …]`, `HMSET`, `HSETNX` |
| Read | `HGET`, `HMGET`, `HGETALL`, `HKEYS`, `HVALS`, `HLEN`, `HSTRLEN` |
| Delete | `HDEL key field [field …]` |
| Exists | `HEXISTS key field` |
| Increment | `HINCRBY key field increment`, `HINCRBYFLOAT key field increment` |
| Random | `HRANDFIELD key [count [WITHVALUES]]` |
| Scan | `HSCAN key cursor [MATCH pattern] [COUNT count]` |

---

## Phase 9 — Set Data Structure

Add the `set` type. Encode as a deduplicated sequence of member strings in a blob or
a hash-set structure for large sets.

| Command group | Commands |
|---|---|
| Write | `SADD key member [member …]`, `SREM key member [member …]` |
| Read | `SMEMBERS`, `SCARD`, `SISMEMBER`, `SMISMEMBER key member [member …]` |
| Random | `SPOP key [count]`, `SRANDMEMBER key [count]` |
| Set ops | `SDIFF key [key …]`, `SDIFFSTORE dst key [key …]` |
| Set ops | `SINTER key [key …]`, `SINTERSTORE`, `SINTERCARD numkeys key … [LIMIT n]` |
| Set ops | `SUNION key [key …]`, `SUNIONSTORE dst key [key …]` |
| Move | `SMOVE src dst member` |
| Scan | `SSCAN key cursor [MATCH pattern] [COUNT count]` |

---

## Phase 10 — Sorted Set Data Structure

Add the `zset` type. Use a pair of parallel structures: a hash map of `member→score`
for O(1) score lookup and member existence, and a skip-list (or BTree) ordered by
`(score, member)` for range operations.

| Command group | Commands |
|---|---|
| Write | `ZADD key [NX\|XX] [GT\|LT] [CH] [INCR] score member …` |
| Delete | `ZREM key member [member …]`, `ZREMRANGEBYSCORE`, `ZREMRANGEBYRANK`, `ZREMRANGEBYLEX` |
| Increment | `ZINCRBY key increment member` |
| Score / rank | `ZSCORE key member`, `ZMSCORE key member [member …]`, `ZRANK key member`, `ZREVRANK key member` |
| Count | `ZCARD key`, `ZCOUNT key min max`, `ZLEXCOUNT key min max` |
| Range | `ZRANGE key min max [BYSCORE\|BYLEX] [REV] [LIMIT offset count] [WITHSCORES]` |
| Range (legacy) | `ZRANGEBYSCORE`, `ZREVRANGEBYSCORE`, `ZRANGEBYLEX`, `ZREVRANGEBYLEX`, `ZREVRANGE` |
| Pop | `ZPOPMIN key [count]`, `ZPOPMAX key [count]`, `ZMPOP numkeys key … MIN\|MAX [COUNT n]` |
| Blocking pop | `BZPOPMIN`, `BZPOPMAX`, `BZMPOP` |
| Random | `ZRANDMEMBER key [count [WITHSCORES]]` |
| Store | `ZRANGESTORE dst src min max [options…]` |
| Set ops | `ZDIFF numkeys key …`, `ZDIFFSTORE dst numkeys key …` |
| Set ops | `ZINTER numkeys key …`, `ZINTERSTORE dst numkeys key … [WEIGHTS …] [AGGREGATE MIN\|MAX\|SUM]`, `ZINTERCARD` |
| Set ops | `ZUNION numkeys key …`, `ZUNIONSTORE dst numkeys key … [WEIGHTS …] [AGGREGATE …]` |
| Scan | `ZSCAN key cursor [MATCH pattern] [COUNT count]` |

---

## Phase 11 — HyperLogLog

Add the `hll` type. Implement the HyperLogLog algorithm with both sparse (small
cardinality) and dense (large cardinality) representations, matching Redis's internal
encoding so that `PFMERGE` across keys produces consistent estimates.

| Command | Notes |
|---|---|
| `PFADD key element …` | Add elements; return 1 if the internal HLL register set changed |
| `PFCOUNT key [key …]` | Return estimated cardinality; for multiple keys compute union estimate |
| `PFMERGE destkey sourcekey …` | Merge multiple HLL structures into dest |

---

## Phase 12 — Geo Commands

Add the `geo` type, which is a sorted set internally with the score being a 52-bit
geohash integer encoding (longitude, latitude) via Mercator interleaving.

Distance calculations use the haversine formula. The geohash encoding and slot width
must match Redis's implementation exactly.

| Command | Notes |
|---|---|
| `GEOADD key [NX\|XX\|CH] long lat member …` | Encode coordinates as geohash score; add to sorted set |
| `GEODIST key m1 m2 [m\|km\|mi\|ft]` | Haversine distance between two members |
| `GEOHASH key member …` | Return 11-character base-32 geohash strings |
| `GEOPOS key member …` | Return (longitude, latitude) pairs decoded from score |
| `GEOSEARCH key FROMMEMBER\|FROMLONLAT … BYRADIUS\|BYBOX … [ASC\|DESC] [LIMIT n [ANY]] [WITHCOORD] [WITHDIST] [WITHHASH]` | Search by radius or bounding box |
| `GEOSEARCHSTORE dst src … ` | Search and write results to a sorted set destination |
| `GEORADIUS` / `GEORADIUSBYMEMBER` (deprecated) | Implement for backward compatibility |

---

## Phase 13 — Bit Operations

These operate on string values treated as bit arrays. Strings are zero-extended when
offsets exceed the current length.

| Command | Notes |
|---|---|
| `GETBIT key offset` | Return the bit value (0 or 1) at offset |
| `SETBIT key offset value` | Set bit at offset; return old value; extend string if needed |
| `BITCOUNT key [start end [BYTE\|BIT]]` | Count set bits in the specified range |
| `BITPOS key bit [start [end [BYTE\|BIT]]]` | Find the position of the first set or clear bit |
| `BITOP AND\|OR\|XOR\|NOT destkey srckey …` | Bitwise operation across one or more string values into dest |
| `BITFIELD key [GET type offset] [SET type offset value] [INCRBY type offset increment] [OVERFLOW WRAP\|SAT\|FAIL]` | Typed integer field read/write/increment operations within a string |
| `BITFIELD_RO key GET type offset` | Read-only variant of BITFIELD GET |

---

## Phase 14 — Stream Commands

Add the `stream` type. A stream is an append-only ordered log of entries, each with an
auto-generated or explicit `<milliseconds>-<sequence>` ID, and an arbitrary set of
field-value pairs. Consumer groups track a last-delivered ID and a pending-entry list
(PEL) per named consumer.

| Command group | Commands |
|---|---|
| Write | `XADD key [NOMKSTREAM] [MAXLEN\|MINID …] *\|id field value …` |
| Trim | `XDEL key id …`, `XTRIM key MAXLEN\|MINID [~\|=] threshold [LIMIT n]` |
| Read | `XRANGE key start end [COUNT n]`, `XREVRANGE key end start [COUNT n]`, `XREAD [COUNT n] [BLOCK ms] STREAMS key … id …`, `XLEN key` |
| Info | `XINFO STREAM key [FULL [COUNT n]]`, `XINFO CONSUMERS key group`, `XINFO GROUPS key` |
| Groups | `XGROUP CREATE key group id [MKSTREAM] [ENTRIESREAD n]` |
| Groups | `XGROUP SETID key group id [ENTRIESREAD n]`, `XGROUP DESTROY key group`, `XGROUP CREATECONSUMER key group consumer`, `XGROUP DELCONSUMER key group consumer` |
| Consumer read | `XREADGROUP GROUP group consumer [COUNT n] [BLOCK ms] [NOACK] STREAMS key … id …` |
| Ack / PEL | `XACK key group id …`, `XPENDING key group [IDLE ms] [start end count] [consumer]` |
| Claim | `XCLAIM key group consumer min-idle-time id … [options…]`, `XAUTOCLAIM key group consumer min-idle-time start [COUNT n] [JUSTID]` |

---

## Phase 15 — ACL Commands

Implement the ACL subsystem: user list, SHA-256 password hashing, command/key/channel
permission rules, and the ACL log.

| Command | Notes |
|---|---|
| `ACL WHOAMI` | Return the current authenticated user name |
| `ACL LIST` | Return ACL rule strings for all users (`user name on/off #hash ~keys &channels +cmds`) |
| `ACL USERS` | Return list of user names |
| `ACL GETUSER username` | Return dict with flags, passwords, commands, keys, channels, selectors |
| `ACL SETUSER username rule …` | Create or modify user: `on`/`off`, `>password`, `nopass`, `~key-pattern`, `&channel-pattern`, `+cmd`/`-cmd`, `allkeys`, `allchannels`, `allcommands`, `nocommands`, `reset*` rules |
| `ACL DELUSER username …` | Delete users; return count deleted |
| `ACL CAT [category]` | List all ACL categories or all commands in a specific category |
| `ACL GENPASS [bits]` | Return a cryptographically random hexadecimal string of `bits` length |
| `ACL LOG [count\|RESET]` | Return or reset the ACL violation log |
| `ACL DRYRUN username command arg …` | Simulate executing the command as the user; return OK or the permission-denied error |
| `ACL HELP` | Return the help text array |

---

## Phase 16 — Transaction Commands

| Command | Notes |
|---|---|
| `MULTI` | Begin transaction; queue subsequent commands in `ConnectionState` |
| `EXEC` | Execute queued commands atomically; return array of per-command results; return nil-array if any WATCH key was modified |
| `DISCARD` | Clear the queued command list; unwatch all keys |
| `WATCH key …` | Register keys to watch; any write to a watched key before EXEC causes EXEC to abort |

Per-connection state (Phase 1d) holds the queued command list and watched-key set.
On any write command, notify all connections that are watching the written key.

---

## Phase 17 — Replication

Implement master-replica replication to make `test_sync` and `test_psync` pass.
These tests connect to port 6380 (a second server instance) and verify it can sync.

- **REPLICAOF host port** — connect to a master and begin full or partial
  synchronization. Store replication state (master host/port, replication offset,
  replication ID) in server state.
- **REPLICAOF NO ONE** — promote this instance to standalone master.
- **SYNC** — legacy full-synchronization command; send an RDB snapshot followed by a
  replication stream.
- **PSYNC replication-id offset** — partial resynchronization if the replication ID
  matches and the offset is within the backlog; otherwise fall back to full sync
  (`FULLRESYNC id 0` response).
- Update `ROLE` and the `INFO replication` section to reflect live replication state.
- Update `WAIT numreplicas timeout` to block until replicas acknowledge writes.

The conftest test infrastructure needs a second server process (the replica) started
alongside the primary, with `--replica` mode pointing at the primary.

---

## Architectural Notes

### BTree value layout after Phase 1

```
BTree key  : raw key bytes (variable length, byte-lexicographic ordering)
BTree value: [type: U8][flags: U8][expiry_ms: U64][blob_id: U64]   (18 bytes fixed)
```

`flags` carries: `bit 0` = has-expiry. `type` is the Redis type tag. `blob_id` is the
handle into the blob store. Because the key bytes live in BTree nodes, `KEYS` and `SCAN`
can be answered by iterating BTree leaves without touching blobs at all.

For `OBJECT FREQ`, add a `freq: U8` LFU counter field (or a separate parallel structure
if the BTree value size is fixed). Update it on every read using the LFU approximation
decay formula from Redis.

### Scan cursor stability

After Phase 1a the cursor encodes the current BTree position as the raw key bytes of
the last-scanned key. Clients treat cursors as opaque tokens, so the change from the
old hash-based cursor is non-breaking.

### COMMAND table

Implement the command table as a compile-time `constexpr` array of `CommandSpec`
entries, each holding: name, arity, flags bitmask, ACL categories bitmask, key-spec
array (first/last/step or range-spec). `COMMAND GETKEYS` walks the key-spec to extract
keys from a live argument vector. `COMMAND LIST FILTERBY ACLCAT` consults the category
bitmask. The same table drives ACL permission checks in the request dispatcher.

### Testing

After each phase:
```bash
bash tools/resp_tests/run.sh \
  -b build/debug/keyvalue/plexdb_keyvalue_server \
  -m tools/resp_tests/mustpass.txt \
  -L /tmp/resp_logs
```

Regenerate mustpass after each phase:
```bash
bash tools/resp_tests/run.sh \
  -b build/debug/keyvalue/plexdb_keyvalue_server \
  -o tools/resp_tests/mustpass.txt
```

Enable the log plugin when diagnosing crashes:
```bash
cmake -B build/debug -DPLEXDB_ENABLE_PLUGINS=ON
ninja -C build/debug plexdb_keyvalue_server
bash tools/resp_tests/run.sh \
  -b build/debug/keyvalue/plexdb_keyvalue_server \
  -p build/debug/plugins/log/libplexdb_log_plugin.so \
  -L /tmp/resp_logs
```
