export module plexdb.btree;

import plexdb.base;
import plexdb.os;
import plexdb.btree.detail;

// implements a b+tree
// references:
// - https://bobson.ludost.net/books/algo/book6/chap19.htm
// - https://planetscale.com/blog/btrees-and-database-indexes
// - https://www.cs.emory.edu/~cheung/Courses/554/Syllabus/3-index/B-tree=delete3.html
// 
// internal keys are the minimum value for the subtree at the next index
// @todo configurable left-right bias on split
// @todo append-right, append-left (or as bias)
export namespace plexdb::btree {
    // ========================================================================
    // in memory
    // ========================================================================
    struct BTreeInMemory {
        Node* root = nullptr;
        Node* leaves = nullptr;
        CountType depth = 0;
        CountType size = 0;

        BTreeSettings settings;

        explicit BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride);
        ~BTreeInMemory();
        BTreeInMemory(const BTreeInMemory& other) = delete;
        BTreeInMemory& operator=(const BTreeInMemory& other) = delete;
    };

    U8* insert(BTreeInMemory& btree, KeyType key);
    void insert(BTreeInMemory& btree, KeyType key, U8* value);

    Search search(const BTreeInMemory& btree, KeyType key);
    template<typename T>
    T get(const BTreeInMemory& btree, KeyType key) {
        return os::memory_cast<T>(search(btree, key).value);
    }

    bool remove(BTreeInMemory& btree, KeyType key);

    inline CountType size(BTreeInMemory& btree) {return btree.size;}
    inline CountType depth(BTreeInMemory& btree) {return btree.depth;}
}