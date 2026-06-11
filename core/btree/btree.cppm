module;
#include <coroutine>

export module plexdb.btree;

export import plexdb.btree.in_memory;
export import plexdb.btree.paged;
export import plexdb.btree.policy;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

import plexdb.btree.types;
import plexdb.btree.iterator;
import plexdb.btree.detail;
import plexdb.btree.constraint;
import plexdb.btree.node;

export namespace plexdb::btree {
    // ========================================================================
    // Iterator<BT, T> — T is the value type returned by operator*
    // ========================================================================
    template<BTree BT, typename T>
    struct Iterator {
        IteratorImpl<BTreeKP<BT>, BTreeVP<BT>> impl;
        BT* btree;

        Iterator() = default;
        explicit Iterator(BT* in_btree) : btree(in_btree) {}

        coroutine::Task<void> advance() {
            co_await next_iterator_inplace(*this->btree, this->impl);
        }

        T operator*() const {
            auto v = leaf_value_const(impl.leaf, impl.node_size, impl.kp, impl.vp, impl.idx);
            T val;
            os::memory_copy(&val, v.ptr, sizeof(T));
            return val;
        }

        BTreeKeyType<BT> key() const {
            auto k = leaf_get_key_bytes(impl.leaf, impl.node_size, impl.kp, impl.vp, impl.idx);
            return read_key(impl.kp, k.ptr, k.length);
        }

        bool operator==(const Iterator& other) const { return impl == other.impl; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
    };

    // ========================================================================
    // Raw byte interface — key is BTreeKeyType<BT>, value is a byte view
    // ========================================================================
    template<BTree BT>
    coroutine::Task<void> insert(BT& t, BTreeKeyType<BT> key, TArrayView<const U8, U16> value) {
        co_await insert_impl(t, key, value);
    }

    template<BTree BT>
    coroutine::Task<bool> update(BT& t, BTreeKeyType<BT> key, TArrayView<const U8, U16> value) {
        if (Search<true> s = co_await search_impl<true>(t, key)) {
            os::memory_copy(s.value, value.ptr, value.length);
            co_return true;
        }
        co_return false;
    }

    template<BTree BT>
    coroutine::Task<U8*> update_it(BT& t, BTreeKeyType<BT> key) {
        if (Search<true> s = co_await search_impl<true>(t, key))
            co_return s.value;
        co_return nullptr;
    }

    template<BTree BT>
    coroutine::Task<bool> find(BT& t, BTreeKeyType<BT> key, U8* out_value, U16 max_size, U16* out_size = nullptr) {
        if (Search<false> s = co_await search_impl<false>(t, key)) {
            U16 copy_len = s.value_length < max_size ? s.value_length : max_size;
            os::memory_copy(out_value, s.value, copy_len);
            if (out_size) *out_size = s.value_length;
            co_return true;
        }
        co_return false;
    }

    // ========================================================================
    // Typed value wrappers — T is trivially copyable, key is BTreeKeyType<BT>
    // ========================================================================
    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tinsert(BT& t, BTreeKeyType<BT> key, T value) {
        co_await insert(t, key, {reinterpret_cast<const U8*>(&value), sizeof(T)});
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<bool> tupdate(BT& t, BTreeKeyType<BT> key, T value) {
        co_return co_await update(t, key, {reinterpret_cast<const U8*>(&value), sizeof(T)});
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T>
    coroutine::Task<T*> tupdate_it(BT& t, BTreeKeyType<BT> key) {
        co_return reinterpret_cast<T*>(co_await update_it(t, key));
    }

    template<typename T, BTree BT>
        requires TriviallyCopyable<T> && TriviallyConstructible<T>
    coroutine::Task<Optional<T>> tfind(BT& t, BTreeKeyType<BT> key) {
        T value;
        if (co_await find(t, key, reinterpret_cast<U8*>(&value), sizeof(T)))
            co_return Optional{value};
        co_return Optional<T>{};
    }

    // ========================================================================
    // Iterators — T is the value type for operator*
    // ========================================================================
    template<typename T, BTree BT>
    coroutine::Task<Iterator<BT, T>> begin(BT& t) {
        Iterator<BT, T> it{&t};
        it.impl = co_await begin_iterator_impl(*it.btree);
        co_return move(it);
    }

    template<typename T, BTree BT>
    Iterator<BT, T> end(BT&) {
        return {};
    }

    template<typename T, SearchStrategy Strategy, BTree BT>
    coroutine::Task<Iterator<BT, T>> find_it(BT& t, BTreeKeyType<BT> key) {
        Iterator<BT, T> it{&t};
        it.impl = co_await search_iterator_impl<Strategy>(*it.btree, key);
        co_return move(it);
    }

    // ========================================================================
    // size, truncate, remove
    // ========================================================================
    template<BTree BT>
    coroutine::Task<U64> size(BT& t) {
        co_return (co_await read_header(t))->size;
    }

    template<BTree BT>
    coroutine::Task<void> truncate(BT& t) {
        co_await truncate_impl(t);
    }

    template<BTree BT>
    coroutine::Task<bool> remove(BT& t, BTreeKeyType<BT> key) {
        co_return co_await remove_impl(t, key);
    }
}
