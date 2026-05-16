## Core Data Structure
- [ ] Implement a balanced tree (Red-Black, AVL, or skip list) for memtable.
  - [ ] Reuse in memory btree implementation? 
- [ ] Maintain sorted key-value pairs in memory.
- [ ] Support basic operations:
  - [ ] `put(key, value)` → insert or update key.
  - [ ] `get(key)` → search for key in sorted order.
  - [ ] `delete(key)` → insert tombstone for key.

## Size Management
- [ ] Track current memtable size in bytes.
- [ ] Trigger flush to disk when memtable exceeds configured threshold.

## Crash Recovery
- [ ] Implement append-only write-ahead log (WAL) for all writes.
- [ ] On startup, reconstruct memtable from WAL.
- [ ] Truncate WAL after memtable flush to SSTable.

## Optional / Advanced Features
- [ ] Support compression of in-memory blocks before flushing.
- [ ] Metrics: number of keys, memory usage.
- [ ] Optionally maintain multiple memtables during flush to avoid blocking writes.
