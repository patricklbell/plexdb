module;
#include <coroutine>

export module plexdb.blob;

export import plexdb.blob.types;
export import plexdb.blob.paged;
export import plexdb.blob.in_memory;
export import plexdb.blob.paged.detail;
export import plexdb.blob.in_memory.detail;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

export namespace plexdb::blob {
    // ========================================================================
    // untyped interface
    // ========================================================================
    coroutine::Task<void> update(Blob auto& blob, const U8* in_value, U64 size, U64 offset=0) {
        assert_true(offset + size <= blob.size_bytes, "update fits in blob");

        U64 value_offset = 0;
        while (value_offset < size) {
            TArrayView<U8> block = co_await rwblock(blob, offset + value_offset);
            os::memory_copy(block.ptr, in_value + value_offset, min(block.length, size - value_offset));
            value_offset += block.length;
        }
    }

    coroutine::Task<void> get(Blob auto& blob, U8* out_value, U64 size, U64 offset=0) {
        assert_true(offset + size <= blob.size_bytes, "gets fits in blob");

        U64 value_offset = 0;
        while (value_offset < size) {
            TArrayView<const U8> block = co_await rblock(blob, offset + value_offset);
            os::memory_copy(out_value + value_offset, block.ptr, min(block.length, size - value_offset));
            value_offset += block.length;
        }
    }

    coroutine::Task<void> remove(Blob auto& blob) {
        co_await remove_impl(blob);
    }

    coroutine::Task<void> resize(Blob auto& blob, U64 new_size) {
        static_assert(!SameAs<Decay<decltype(blob)>, BlobStaticPaged>, "cannot resize static blob");
        co_await resize_impl(blob, new_size);
    }

    coroutine::Task<void> append(Blob auto& blob, const U8* in_value, U64 size) {
        U64 offset = blob.size_bytes;
        co_await resize(blob, blob.size_bytes + size);
        co_await update(blob, in_value, size, offset);
    }

    coroutine::Task<void> insert(Blob auto& blob, const U8* in_value, U64 size, U64 offset=0) {
        if (offset + size > blob.size_bytes) {
            co_await resize(blob, offset + size);
        }
        co_await update(blob, in_value, size, offset);
    }

    // ========================================================================
    // typed interface
    // ========================================================================
    template<typename T>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tupdate(Blob auto& blob, const T* in_value) {
        co_await update(blob, reinterpret_cast<const U8*>(in_value), sizeof(T), 0);
    }

    template<typename T>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tupdate(Blob auto& blob, const T* in_value, U64* inout_offset) {
        co_await update(blob, reinterpret_cast<const U8*>(in_value), sizeof(T), *inout_offset);
        *inout_offset += sizeof(T);
    }

    template<typename T>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tget(Blob auto& blob, T* out_value) {
        co_await get(blob, reinterpret_cast<U8*>(out_value), sizeof(T), 0);
    }

    template<typename T>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tget(Blob auto& blob, T* out_value, U64* inout_offset) {
        co_await get(blob, reinterpret_cast<U8*>(out_value), sizeof(T), *inout_offset);
        *inout_offset += sizeof(T);
    }

    template<typename T>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tappend(Blob auto& blob, const T* in_value) {
        co_await append(blob, reinterpret_cast<const U8*>(in_value), sizeof(T));
    }

    template<typename T>
        requires TriviallyCopyable<T>
    coroutine::Task<void> tinsert(Blob auto& blob, const T* in_value) {
        co_await insert(blob, reinterpret_cast<const U8*>(in_value), sizeof(T));
    }
}
