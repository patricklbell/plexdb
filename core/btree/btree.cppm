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
    U8* view(BT& btree, KeyType key) {
        typename BT::Transaction t{&btree};

        if (Search s = search_impl(t, key)) {
            return s.value;
        }
        return nullptr;
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

    template<BTree BT, typename T = U8>
    struct Iterator {
        typename BT::Transaction t;
        IteratorImpl impl;

        Iterator() = default;
        explicit Iterator(BT& btree) : t(&btree) {}

        Iterator& operator++() { this->impl = next_iterator_impl(this->t, impl); return *this; }

        const T& operator*() { return *reinterpret_cast<const T*>(values(this->impl.leaf, *read_header(this->t))[this->impl.idx]); }

        // @note does not compare transactions
        bool operator==(const Iterator& other) const { return impl == other.impl; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
    };

    template<BTree BT>
    Iterator<BT> begin(BT& btree) {
        Iterator it{btree};
        it.impl = begin_iterator_impl(it.t);
        return move(it);
    }

    template<BTree BT>
    Iterator<BT> end(BT&) {
        Iterator<BT> it{};
        it.impl = end_iterator_impl();
        return move(it);
    }

    template<SearchStrategy Strategy, BTree BT>
    Iterator<BT> find_it(BT& btree, KeyType key) {
        Iterator<BT> it{btree};
        it.impl = search_iterator_impl<Strategy>(it.t, key);
        return move(it);
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

    template<typename T, BTree BT>
    Iterator<BT,T> tbegin(BT& btree) {
        Iterator<BT,T> it{btree};
        it.impl = begin_iterator_impl(it.t);
        return move(it);
    }

    template<typename T, BTree BT>
    Iterator<BT,T> tend(BT&) {
        Iterator<BT,T> it{};
        it.impl = end_iterator_impl();
        return move(it);
    }

    template<typename T, SearchStrategy Strategy, BTree BT>
    Iterator<BT,T> tfind_it(BT& btree, KeyType key) {
        Iterator<BT,T> it{btree};
        it.impl = search_iterator_impl<Strategy>(it.t, key);
        return move(it);
    }

    template<BTree BT>
    U64 size(BT& btree) {
        typename BT::Transaction t{&btree};
        return read_header(t)->size;
    }

    template<BTree BT>
    void truncate(BT& btree) {
        typename BT::Transaction t{&btree};
        truncate_impl(t);
    }
}
