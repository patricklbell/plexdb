export module plexdb.btree;

// should transactions be exported? probably not
export import plexdb.btree.in_memory;
export import plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.btree.detail;

export namespace plexdb::btree {
    // ========================================================================
    // untyped helpers
    //      basic interface, @todo dont rw lock for all searches and allow
    //      updating with offsets
    // ========================================================================
    template<typename BTree>
    void insert(BTree& btree, KeyType key, U8* in_value, U64 size, U64 offset=0) {
        using Transaction = typename BTree::Transaction;
        Transaction t{&btree};

        U8* value = insert_impl(t, key);

        assert_true(offset + size <= rheader(t)->value_stride, "enough space in value");
        os::memory_copy(value + offset, in_value, size);
    }

    template<typename BTree>
    bool update(BTree& btree, KeyType key, U8* in_value, U64 size, U64 offset=0) {
        using Transaction = typename BTree::Transaction;
        Transaction t{&btree};

        if (Search s = search_impl(t, key)) {
            assert_true(offset + size <= rheader(t)->value_stride, "enough space in value");
            os::memory_copy(s.value + offset, in_value, size);
            return true;
        }
        return false;
    }

    template<typename BTree>
    bool find(BTree& btree, KeyType key, U8* out_value, U64 size, U64 offset=0) {
        using Transaction = typename BTree::Transaction;
        Transaction t{&btree};

        if (Search s = search_impl(t, key)) {
            assert_true(offset + size <= rheader(t)->value_stride, "enough space in value");
            os::memory_copy(out_value, s.value + offset, size);
            return true;
        }
        return false;
    }

    template<typename BTree>
    bool remove(BTree& btree, KeyType key) {
        using Transaction = typename BTree::Transaction;
        Transaction t{&btree};

        return remove_impl(t, key);
    }

    // ========================================================================
    // typed helpers
    // ========================================================================
    template<typename T, typename BTree>
        requires TriviallyCopyable<T>
    void tinsert(BTree& btree, KeyType key, T& in_value) {
        insert(btree, key, reinterpret_cast<U8*>(&in_value), sizeof(T));
    }

    template<typename T, typename BTree>
        requires TriviallyCopyable<T>
    bool tupdate(BTree& btree, KeyType key, T value) {
        return update(btree, key, reinterpret_cast<U8*>(&value), sizeof(T));
    }

    template<typename T, typename BTree>
        requires TriviallyCopyable<T> && TriviallyConstructible<T>
    Optional<T> tfind(BTree& btree, KeyType key) {
        T value;
        if (find(btree, key, reinterpret_cast<U8*>(&value), sizeof(T)))
            return Optional{value};
        return {};
    }
}

export namespace plexdb {
    using btree::BTreeInMemory;
    using btree::BTreePaged;
}