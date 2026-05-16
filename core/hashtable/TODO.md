Bitcask-Inspired Hash-Indexed Append-Only Key-Value Store

## In-Memory Cache
- [ ] Implement a hash map to store `key → file_offset`.
- [ ] Maintain a separate hash map for each segment file.
- [ ] Support basic operations:
  - [ ] `put(key, value)` → update hash map with new offset.
  - [ ] `get(key)` → fetch file offset from hash map.
  - [ ] `delete(key)` → append tombstone and update hash map.
- [ ] Load segment hash map snapshots on startup for fast recovery.
- [ ] Ensure cache can handle multiple read threads safely.

## Storage Layer
- [ ] Implement append-only log file format:
  - [ ] Binary format: `[length of key][key][length of value][value]`.
  - [ ] Include checksums for each record to detect partial writes.
  - [ ] Tombstones for deletions.
- [ ] File segmentation:
  - [ ] Sequentially numbered segment files (e.g., `000001.data`).
  - [ ] Fixed-size segments configurable at startup.
  - [ ] Immutable segments once written.
- [ ] Compaction:
  - [ ] Remove duplicate keys within a segment, keep the most recent value.
  - [ ] Merge multiple segments into a new segment file.
  - [ ] Perform merging and compaction in a background thread.
  - [ ] Atomically switch reads to the new merged segment before deleting old segments.
  - [ ] Update segment hash maps after merge.
  - [ ] Track segment stats (number of keys, space reclaimed).
- [ ] Recovery:
  - [ ] Scan segments or load snapshots on startup.
  - [ ] Truncate logs at first corrupted record detected via checksum.
- [ ] Concurrency:
  - [ ] Single writer thread for sequential writes.
  - [ ] Multiple readers can read concurrently without blocking.
- [ ] Crash-safety:
  - [ ] Writes are strictly sequential to simplify recovery.
  - [ ] Hash map snapshots written to disk after segment flush or merge.
  - [ ] Partially written records detected and ignored/truncated.
- [ ] Performance optimizations:
  - [ ] Sequential writes for both append and merging (good for SSD/HDD).
  - [ ] Avoid random writes to improve disk throughput.

## Optional / Advanced Features
- [ ] Configurable segment size for compaction tuning.
- [ ] Periodic hash map snapshotting for faster restart.
- [ ] Metrics per segment (e.g., number of keys, disk usage).
- [ ] Support for large values that are read from disk on demand.
- [ ] Background garbage collection of fully merged segments.

## API / Design Considerations
- [ ] Clean separation of in-memory cache and storage layer.
- [ ] Abstract storage interface to allow swapping append-only backend if needed.
- [ ] Ensure atomic updates when merging segments to maintain consistency.
