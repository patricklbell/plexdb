export module plexdb.btree;

export import plexdb.btree.in_memory;
export import plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.btree.detail;

export namespace plexdb::btree {
    template<typename BTree>
    U8* insert(BTree& btree, KeyType key) {
        return insert_impl(btree, key);
    }

    template<typename BTree>
    void insert(BTree& btree, KeyType key, U8* value) {
        U8* view = insert(btree, key);
        os::memory_copy(view, value, get_settings(btree).value_stride);
    }

    template<typename BTree>
    Search search(BTree& btree, KeyType key) {
        return search_impl(btree, key);
    }

    template<typename T, typename BTree>
    T get(BTree& btree, KeyType key) {
        return os::memory_cast<T>(search(btree, key).value);
    }

    template<typename BTree>
    bool remove(BTree& btree, KeyType key) {
        return remove_impl(btree, key);
    }
}

export namespace plexdb {
    using btree::BTreeInMemory;
    using btree::BTreePaged;
}