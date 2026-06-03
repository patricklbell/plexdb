# set-associative cache

The current cache is direct-mapped: `slot = page_idx % read_cache_count`. Two pages whose indices share a slot unconditionally evict each other, even when other slots are free. B-tree access patterns make this worse than random — pages at the same tree level tend to have regularly-spaced indices, causing systematic conflicts on hot paths.

A **k-way set-associative cache** groups slots into sets of k. A page maps to a fixed set (`set = page_idx % (read_cache_count / k)`), and eviction picks a victim from within that set using CLOCK or LRU:

```
set_idx  = page_idx % num_sets          // num_sets = read_cache_count / k
ways     = cache[set_idx * k .. (set_idx+1) * k]

lookup:  scan k ways for matching idx   // k is small (2, 4, 8) — still near O(1)
evict:   CLOCK or LRU within the set
```

**Pros:**
- Eliminates most conflict misses with small k (2-way captures ~80% of full-associativity benefit).
- Minimal structural change — the flat buffer stays; only the index arithmetic and the within-set eviction policy are new.
- No hash table or pointer chasing; cache-friendly sequential scan over k entries.
- Naturally accommodates `in_write_set` tracking: prefer evicting a clean way before a dirty one within the set.

**Cons:**
- Eviction is still local to a set — a globally cold page in a hot set can be evicted while a globally hot page in a cold set sits untouched.
- CLOCK within a set requires a per-set clock hand; LRU requires a small per-set ordering (manageable at k=4, awkward at k=8+).
- Does not help if the working set simply exceeds total cache capacity.

k=4 is a practical starting point: large enough to absorb most conflict misses, small enough that the within-set scan is a single cache line.
