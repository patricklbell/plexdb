module;
#include <coroutine>

export module plexdb.btree;

export import plexdb.btree.in_memory;
export import plexdb.btree.paged;
export import plexdb.btree.detail;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

export namespace plexdb::btree {
    // ========================================================================
    // untyped interface
    // ========================================================================
    template<BTree BT>
    coroutine::Task<void> insert(BT& btree, KeyType key, const U8* in_value, U64 size, U64 offset=0) {
        typename BT::Transaction t{&btree};
        co_await t.begin();
        U8* value = co_await insert_impl(t, key);
        os::memory_copy(value + offset, in_value, size);
        co_await t.commit();
    }

    template<BTree BT>
    coroutine::Task<bool> update(BT& btree, KeyType key, const U8* in_value, U64 size, U64 offset=0) {
        typename BT::Transaction t{&btree};
        co_await t.begin();
        bool found = false;
        if (Search s = co_await search_impl(t, key)) {
            os::memory_copy(s.value + offset, in_value, size);
            found = true;
        }
        co_await t.commit();
        co_return found;
    }

    template<BTree BT>
    coroutine::Task<U8*> view(BT& btree, KeyType key) {
        typename BT::Transaction t{&btree};

        if (Search s = co_await search_impl(t, key)) {
            co_return s.value;
        }
        co_return nullptr;
    }

    template<BTree BT>
    coroutine::Task<bool> find(BT& btree, KeyType key, U8* out_value, U64 size, U64 offset=0) {
        typename BT::Transaction t{&btree};

        if (Search s = co_await search_impl(t, key)) {
            os::memory_copy(out_value, s.value + offset, size);
            co_return true;
        }
        co_return false;
    }

    template<BTree BT>
    coroutine::Task<bool> remove(BT& btree, KeyType key) {
        typename BT::Transaction t{&btree};
        co_await t.begin();
        bool result = co_await remove_impl(t, key);
        co_await t.commit();
        co_return result;
    }

    template<BTree BT, typename T = U8>
    struct Iterator {
        typename BT::Transaction t;
        IteratorImpl impl;

        Iterator() = default;
        explicit Iterator(BT& btree) : t(&btree) {}

        coroutine::Task<void> advance() {
            this->impl = co_await next_iterator_impl(this->t, impl);
        }

        const T& operator*() const {
            // Use header fields cached in impl to avoid async header read
            Header h{};
            h.value_stride = impl.value_stride;
            h.max_keys_per_leaf = impl.max_keys_per_leaf;
            return *reinterpret_cast<const T*>(values(this->impl.leaf, h)[this->impl.idx]);
        }

        // @note does not compare transactions
        bool operator==(const Iterator& other) const { return impl == other.impl; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
    };

    template<BTree BT>
    coroutine::Task<Iterator<BT>> begin(BT& btree) {
        Iterator<BT> it{btree};
        it.impl = co_await begin_iterator_impl(it.t);
        co_return move(it);
    }

    template<BTree BT>
    Iterator<BT> end(BT&) {
        Iterator<BT> it{};
        it.impl = end_iterator_impl();
        return move(it);
    }

    template<SearchStrategy Strategy, BTree BT>
    coroutine::Task<Iterator<BT>> find_it(BT& btree, KeyType key) {
        Iterator<BT> it{btree};
        it.impl = co_await search_iterator_impl<Strategy>(it.t, key);
        co_return move(it);
    }

    // ========================================================================
    // typed interface
    // ========================================================================
    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tinsert(BT& btree, KeyType key, const T& in_value) {
        co_await insert(btree, key, reinterpret_cast<const U8*>(&in_value), sizeof(T));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<bool> tupdate(BT& btree, KeyType key, const T& value) {
        co_return co_await update(btree, key, reinterpret_cast<const U8*>(&value), sizeof(T));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T> && TriviallyConstructible<T>
    coroutine::Task<Optional<T>> tfind(BT& btree, KeyType key) {
        T value;
        if (co_await find(btree, key, reinterpret_cast<U8*>(&value), sizeof(T)))
            co_return Optional{value};
        co_return Optional<T>{};
    }

    template<typename T, BTree BT>
    coroutine::Task<Iterator<BT,T>> tbegin(BT& btree) {
        Iterator<BT,T> it{btree};
        it.impl = co_await begin_iterator_impl(it.t);
        co_return move(it);
    }

    template<typename T, BTree BT>
    Iterator<BT,T> tend(BT&) {
        Iterator<BT,T> it{};
        it.impl = end_iterator_impl();
        return move(it);
    }

    template<typename T, SearchStrategy Strategy, BTree BT>
    coroutine::Task<Iterator<BT,T>> tfind_it(BT& btree, KeyType key) {
        Iterator<BT,T> it{btree};
        it.impl = co_await search_iterator_impl<Strategy>(it.t, key);
        co_return move(it);
    }

    template<BTree BT>
    coroutine::Task<U64> size(BT& btree) {
        typename BT::Transaction t{&btree};
        co_return (co_await read_header(t))->size;
    }

    template<BTree BT>
    coroutine::Task<void> truncate(BT& btree) {
        typename BT::Transaction t{&btree};
        co_await t.begin();
        co_await truncate_impl(t);
        co_await t.commit();
    }

    // ========================================================================
    // transaction-accepting overloads — caller owns begin/commit
    // ========================================================================
    template<Transaction Tx>
    coroutine::Task<void> insert(Tx& t, KeyType key, const U8* in_value, U64 size, U64 offset=0) {
        U8* value = co_await insert_impl(t, key);
        os::memory_copy(value + offset, in_value, size);
    }

    template<Transaction Tx>
    coroutine::Task<bool> update(Tx& t, KeyType key, const U8* in_value, U64 size, U64 offset=0) {
        bool found = false;
        if (Search s = co_await search_impl(t, key)) {
            os::memory_copy(s.value + offset, in_value, size);
            found = true;
        }
        co_return found;
    }

    template<Transaction Tx>
    coroutine::Task<bool> find(Tx& t, KeyType key, U8* out_value, U64 size, U64 offset=0) {
        if (Search s = co_await search_impl(t, key)) {
            os::memory_copy(out_value, s.value + offset, size);
            co_return true;
        }
        co_return false;
    }

    template<Transaction Tx>
    coroutine::Task<bool> remove(Tx& t, KeyType key) {
        co_return co_await remove_impl(t, key);
    }

    template<Transaction Tx>
    coroutine::Task<void> truncate(Tx& t) {
        co_await truncate_impl(t);
    }

    template<typename V, Transaction Tx>
        requires TriviallyCopyable<V>
    coroutine::Task<void> tinsert(Tx& t, KeyType key, const V& in_value) {
        co_await insert(t, key, reinterpret_cast<const U8*>(&in_value), sizeof(V));
    }

    template<typename V, Transaction Tx>
        requires TriviallyCopyable<V>
    coroutine::Task<bool> tupdate(Tx& t, KeyType key, const V& value) {
        co_return co_await update(t, key, reinterpret_cast<const U8*>(&value), sizeof(V));
    }

    template<typename V, Transaction Tx>
        requires TriviallyCopyable<V> && TriviallyConstructible<V>
    coroutine::Task<Optional<V>> tfind(Tx& t, KeyType key) {
        V value;
        if (co_await find(t, key, reinterpret_cast<U8*>(&value), sizeof(V)))
            co_return Optional{value};
        co_return Optional<V>{};
    }
}
