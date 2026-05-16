export module keyvalue.store;

import plexdb.base;
import plexdb.dynamic.containers;

using namespace plexdb;

export namespace keyvalue::store {
    struct Store {
        DynamicMap<AutoString8, AutoString8> data;
    };

    // Returns a view into the stored value, or empty Optional.
    Optional<String8> get(const Store& store, String8 key);

    // Insert or overwrite key.
    void set(Store& store, String8 key, String8 value);

    // Delete one key. Returns 1 if it existed, 0 otherwise.
    U64 del_one(Store& store, String8 key);

    // Returns count of keys that exist.
    U64 exists(const Store& store, String8 key);

    // Returns all keys matching a glob pattern (supports * and ?).
    DynamicArray<AutoString8> keys(const Store& store, String8 pattern);

    // Cursor-based scan: returns {next_cursor, matched_keys}.
    // cursor=0 starts a new scan. next_cursor=0 means scan complete.
    Pair<U64, DynamicArray<AutoString8>> scan(const Store& store, U64 cursor, String8 pattern, U64 count);

    // Total number of keys.
    U64 dbsize(const Store& store);

    // Remove all keys.
    void flush(Store& store);
}
