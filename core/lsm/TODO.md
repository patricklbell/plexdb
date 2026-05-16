## SSTable File Format
- [ ] Store key-value pairs sorted by key.
- [ ] Ensure immutability of SSTable files.
- [ ] Divide SSTables into blocks for efficient reading.
- [ ] Optional: compress blocks to save disk space.
- [ ] Include per-block checksums to detect corruption.
- [ ] Include sparse in-memory index mapping a subset of keys → block offsets.
- [ ] Include Bloom filters to optimize lookups for non-existent keys.
- [ ] Include metadata/statistics file (min/max key, number of keys, block sizes).

## Flush / Write Flow
- [ ] Flush memtable to new SSTable when threshold is reached.
- [ ] Optionally discard commit log (WAL) after successful flush.

## Compaction / Merging
- [ ] Merge multiple SSTables in background.
- [ ] Keep most recent value for duplicate keys.
- [ ] Remove tombstones for deleted keys.
- [ ] Write merged SSTable atomically.
- [ ] Delete old SSTables after successful merge.

## Read Flow
- [ ] Search memtable first, then newest SSTables to oldest.
- [ ] Use sparse index to narrow scanning range in blocks.
- [ ] Use Bloom filter to skip SSTables without key.

## Crash Safety
- [ ] Detect and ignore partially written blocks using checksums.
- [ ] SSTables are immutable → crashes do not corrupt previous data.
- [ ] Background compaction must write new SSTable before deleting old ones.

## Optional / Advanced Features
- [ ] Support configurable block sizes for SSTables.
- [ ] Metrics per SSTable: key count, size, compression ratio.
- [ ] Multi-level compaction strategy (leveled LSM).
- [ ] Support range queries efficiently using sorted SSTables.
