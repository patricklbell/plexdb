module;
#include <coroutine>

export module plexdb.btree;

export import plexdb.btree.in_memory;
export import plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

import plexdb.btree.detail;
import plexdb.btree.types;
import plexdb.btree.constraint;
import plexdb.btree.node;

export namespace plexdb::btree {
    // ========================================================================
    // untyped interface — key is always U64 for the default policy
    // ========================================================================
    template<BTree BT>
    coroutine::Task<void> insert(BT& t, U64 key, const U8* in_value, U64 size, U64 offset=0) {
        U8* value = co_await insert_impl(t, key);
        os::memory_copy(value + offset, in_value, size);
    }

    template<BTree BT>
    coroutine::Task<bool> update(BT& t, U64 key, const U8* in_value, U64 size, U64 offset=0) {
        if (Search<true> s = co_await search_impl<true>(t, key)) {
            os::memory_copy(s.value + offset, in_value, size);
            co_return true;
        }
        co_return false;
    }

    template<BTree BT>
    coroutine::Task<U8*> update_it(BT& t, U64 key) {
        if (Search<true> s = co_await search_impl<true>(t, key))
            co_return s.value;
        co_return nullptr;
    }

    template<BTree BT>
    coroutine::Task<bool> find(BT& t, U64 key, U8* out_value, U64 size, U64 offset=0) {
        if (Search<false> s = co_await search_impl<false>(t, key)) {
            os::memory_copy(out_value, s.value + offset, size);
            co_return true;
        }
        co_return false;
    }

    template<BTree BT>
    coroutine::Task<bool> remove(BT& t, U64 key) {
        bool result = co_await remove_impl(t, key);
        co_return result;
    }

    template<BTree BT, typename T = U8>
    struct Iterator {
        IteratorImpl impl;
        BT* btree;

        Iterator() = default;
        explicit Iterator(BT* in_btree) : btree(in_btree) {}

        coroutine::Task<void> advance() {
            this->impl = co_await next_iterator_impl(*this->btree, impl);
        }

        const T& operator*() const {
            return *reinterpret_cast<const T*>(values(impl.leaf, impl.node_size, impl.vp.stride)[impl.idx]);
        }

        bool operator==(const Iterator& other) const { return impl == other.impl; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
    };

    template<BTree BT>
    coroutine::Task<Iterator<BT>> begin(BT& t) {
        Iterator<BT> it{&t};
        it.impl = co_await begin_iterator_impl(*it.btree);
        co_return move(it);
    }

    template<BTree BT>
    Iterator<BT> end(BT&) {
        Iterator<BT> it{};
        it.impl = end_iterator_impl();
        return move(it);
    }

    template<SearchStrategy Strategy, BTree BT>
    coroutine::Task<Iterator<BT>> find_it(BT& t, U64 key) {
        Iterator<BT> it{&t};
        it.impl = co_await search_iterator_impl<Strategy>(*it.btree, key);
        co_return move(it);
    }

    // ========================================================================
    // typed interface
    // ========================================================================
    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tinsert(BT& t, U64 key, const T& in_value) {
        co_await insert(t, key, reinterpret_cast<const U8*>(&in_value), sizeof(T));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<bool> tupdate(BT& t, U64 key, const T& value) {
        co_return co_await update(t, key, reinterpret_cast<const U8*>(&value), sizeof(T));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<T*> tupdate_it(BT& t, U64 key) {
        co_return reinterpret_cast<T*>(co_await update_it(t, key));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T> && TriviallyConstructible<T>
    coroutine::Task<Optional<T>> tfind(BT& t, U64 key) {
        T value;
        if (co_await find(t, key, reinterpret_cast<U8*>(&value), sizeof(T)))
            co_return Optional{value};
        co_return Optional<T>{};
    }

    template<typename T, BTree BT>
    coroutine::Task<Iterator<BT,T>> tbegin(BT& t) {
        Iterator<BT,T> it{&t};
        it.impl = co_await begin_iterator_impl(*it.btree);
        co_return move(it);
    }

    template<typename T, BTree BT>
    Iterator<BT,T> tend(BT&) {
        Iterator<BT,T> it{};
        it.impl = end_iterator_impl();
        return move(it);
    }

    template<typename T, SearchStrategy Strategy, BTree BT>
    coroutine::Task<Iterator<BT,T>> tfind_it(BT& t, U64 key) {
        Iterator<BT,T> it{&t};
        it.impl = co_await search_iterator_impl<Strategy>(*it.btree, key);
        co_return move(it);
    }

    template<BTree BT>
    coroutine::Task<U64> size(BT& t) {
        co_return (co_await read_header(t))->size;
    }

    template<BTree BT>
    coroutine::Task<void> truncate(BT& t) {
        co_await truncate_impl(t);
    }
}
