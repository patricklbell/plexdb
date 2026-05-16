# TODO: Variable-Length Keys + Compression for B+Tree

## Generic Key Support
- [ ] Replace `u64` keys with comparator-based generic key API
- [ ] Store keys as `(len, bytes)` or varlen slices
- [ ] Add pluggable collation / comparison support
- [ ] Convert fixed-array node layout to slotted pages
- [ ] Split/merge pages by used bytes instead of key count

References:
- [PostgreSQL B-Tree docs](https://www.postgresql.org/docs/current/btree.html?utm_source=chatgpt.com)
- [InnoDB index page structure](https://dev.mysql.com/doc/en/innodb-physical-structure.html?utm_source=chatgpt.com)
- [WiredTiger page structure](https://source.wiredtiger.com/develop/tune_page_size_and_comp.html?utm_source=chatgpt.com)

## Internal Node Compression
- [ ] Store separator/fence keys instead of full copied keys
- [ ] Implement prefix truncation for internal separators
- [ ] Add suffix truncation for pivot keys
- [ ] Support abbreviated search keys

References:
- [PostgreSQL prefix truncation design notes](https://wiki.postgresql.org/wiki/NBTree_Prefix_Truncation?utm_source=chatgpt.com)
- [PostgreSQL B-Tree implementation docs](https://www.postgresql.org/docs/13/btree-implementation.html?utm_source=chatgpt.com)

## Leaf Compression
- [ ] Prefix-compress adjacent keys in leaves
- [ ] Add duplicate-key deduplication
- [ ] Support overflow pages for huge keys
- [ ] Add optional page-level compression

References:
- [PostgreSQL deduplication docs](https://www.postgresql.org/docs/current/btree.html?utm_source=chatgpt.com)
- [WiredTiger compression tuning](https://source.wiredtiger.com/develop/tune_page_size_and_comp.html?utm_source=chatgpt.com)
- [InnoDB physical index structure](https://dev.mysql.com/doc/en/innodb-physical-structure.html?utm_source=chatgpt.com)

## Page Management
- [ ] Implement slot directory + variable-size cells
- [ ] Add page compaction / defragmentation
- [ ] Track free-space fragmentation
- [ ] Benchmark fanout before/after compression

## Testing
- [ ] Fuzz split/merge with random-length strings
- [ ] Test long common-prefix workloads
- [ ] Validate UTF-8 + custom comparator ordering
- [ ] Benchmark compressed vs uncompressed trees
