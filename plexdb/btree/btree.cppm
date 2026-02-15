export module plexdb.btree;

// should transactions be exported? probably not
export import plexdb.btree.in_memory;
export import plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.btree.detail;

export namespace plexdb::btree {
    // ========================================================================
    // untyped interface
    //      basic interface, @todo dont rw lock for all searches and allow
    //      updating with offsets
    // ========================================================================
    template<BTree BT>
    void insert(BT& btree, KeyType key, const U8* in_value, U64 size, U64 offset=0) {
        typename BT::Transaction t{&btree};

        U8* value = insert_impl(t, key);

        assert_true(offset + size <= read_header(t)->value_stride, "enough space in value");
        os::memory_copy(value + offset, in_value, size);
    }

    template<BTree BT>
    bool update(BT& btree, KeyType key, const U8* in_value, U64 size, U64 offset=0) {
        typename BT::Transaction t{&btree};

        if (Search s = search_impl(t, key)) {
            assert_true(offset + size <= read_header(t)->value_stride, "enough space in value");
            os::memory_copy(s.value + offset, in_value, size);
            return true;
        }
        return false;
    }

    template<BTree BT>
    bool find(BT& btree, KeyType key, U8* out_value, U64 size, U64 offset=0) {
        typename BT::Transaction t{&btree};

        if (Search s = search_impl(t, key)) {
            assert_true(offset + size <= read_header(t)->value_stride, "enough space in value");
            os::memory_copy(out_value, s.value + offset, size);
            return true;
        }
        return false;
    }

    template<BTree BT>
    bool remove(BT& btree, KeyType key) {
        typename BT::Transaction t{&btree};

        return remove_impl(t, key);
    }

    // ========================================================================
    // typed interface
    // ========================================================================
    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    void tinsert(BT& btree, KeyType key, const T& in_value) {
        insert(btree, key, reinterpret_cast<const U8*>(&in_value), sizeof(T));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    bool tupdate(BT& btree, KeyType key, const T& value) {
        return update(btree, key, reinterpret_cast<const U8*>(&value), sizeof(T));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T> && TriviallyConstructible<T>
    Optional<T> tfind(BT& btree, KeyType key) {
        T value;
        if (find(btree, key, reinterpret_cast<U8*>(&value), sizeof(T)))
            return Optional{value};
        return {};
    }
}

export namespace plexdb {
    using BTInMemory = btree::BTreeInMemory;
    using BTPaged = btree::BTreePaged;
}