module keyvalue.store;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.os;

using namespace plexdb;

namespace keyvalue::store {
    // ========================================================================
    // glob matching (* = any chars, ? = any single char)
    // ========================================================================
    static bool glob_match(const char* pattern, U64 plen, const char* text, U64 tlen) {
        if (plen == 1 && pattern[0] == '*') return true;

        U64 pi = 0, ti = 0;
        U64 star_pi = U64(-1), star_ti = 0;

        while (ti < tlen) {
            if (pi < plen && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
                pi++; ti++;
            } else if (pi < plen && pattern[pi] == '*') {
                star_pi = pi++;
                star_ti = ti;
            } else if (star_pi != U64(-1)) {
                pi = star_pi + 1;
                ti = ++star_ti;
            } else {
                return false;
            }
        }

        while (pi < plen && pattern[pi] == '*') pi++;
        return pi == plen;
    }

    static bool key_matches(const AutoString8& key, String8 pattern) {
        return glob_match(pattern.data, pattern.length, key.c_str, key.length);
    }

    Optional<String8> get(const Store& store, String8 key) {
        AutoString8 k{key};
        auto it = find_it(store.data, k);
        if (it != store.data.end())
            return String8{(*it).second.c_str, (*it).second.length};
        return {};
    }

    void set(Store& store, String8 key, String8 value) {
        insert(store.data, AutoString8{key}, AutoString8{value});
    }

    U64 del_one(Store& store, String8 key) {
        AutoString8 k{key};
        U64 slot_idx = 0, pair_idx = 0;
        if (!find_slot_and_pair(store.data, k, slot_idx, pair_idx)) return 0;

        // swap-and-pop within the bucket
        DynamicArray<Pair<AutoString8, AutoString8>>& bucket = store.data.slots[slot_idx];
        if (pair_idx + 1 < bucket.length) {
            bucket[pair_idx] = plexdb::move(bucket[bucket.length - 1]);
        }
        pop_back(bucket);
        return 1;
    }

    U64 exists(const Store& store, String8 key) {
        AutoString8 k{key};
        U64 si = 0, pi = 0;
        return find_slot_and_pair(store.data, k, si, pi) ? 1 : 0;
    }

    DynamicArray<AutoString8> keys(const Store& store, String8 pattern) {
        DynamicArray<AutoString8> result;
        for (const auto& slot : store.data.slots) {
            for (const auto& pair : slot) {
                if (key_matches(pair.first, pattern))
                    push_back(result, pair.first);
            }
        }
        return result;
    }

    Pair<U64, DynamicArray<AutoString8>> scan(const Store& store, U64 cursor, String8 pattern, U64 count) {
        DynamicArray<AutoString8> result;
        if (count == 0) count = 10;

        U64 pos     = 0;
        U64 scanned = 0;
        U64 next_cursor = 0;

        for (U64 si = 0; si < store.data.slots.length; si++) {
            const auto& slot = store.data.slots[si];
            for (U64 pi = 0; pi < slot.length; pi++) {
                if (pos < cursor) { pos++; continue; }

                if (key_matches(slot[pi].first, pattern))
                    push_back(result, slot[pi].first);

                pos++;
                scanned++;

                if (scanned >= count) {
                    next_cursor = pos;
                    // check if there are more entries
                    bool more = (si < store.data.slots.length - 1 || pi + 1 < slot.length);
                    if (!more) next_cursor = 0;
                    return {next_cursor, plexdb::move(result)};
                }
            }
        }

        return {0, plexdb::move(result)};
    }

    U64 dbsize(const Store& store) {
        return plexdb::length(store.data);
    }

    void flush(Store& store) {
        clear(store.data);
    }
}
