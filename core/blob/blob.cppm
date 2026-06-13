module;
#include <coroutine>

export module plexdb.blob;

export import plexdb.blob.paged;
export import plexdb.blob.in_memory;

import plexdb.blob.constraint;
import plexdb.blob.paged.detail;
import plexdb.blob.in_memory.detail;
import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.pager;

export namespace plexdb::blob {
    // ========================================================================
    // BlobCursor — streaming read cursor over a BlobDynamicPaged.
    // Holds only page indices (no live page pointers), so it is safe to keep
    // across transaction boundaries. Each read/skip_varlen call opens and
    // commits its own short transaction; skip() is pure arithmetic (no I/O).
    // ========================================================================
    struct BlobCursor {
        BlobDynamicPaged blob;
        U64              offset = 0;

        explicit operator bool() const {
            return blob.pager != nullptr;
        }

        // Advance offset without any I/O.
        void skip(U64 size) {
            offset += size;
        }
    };

    // ========================================================================
    // untyped interface
    // ========================================================================
    coroutine::Task<void> update(Blob auto& blob, const U8* in_value, U64 size, U64 offset = 0) {
        assert_true(offset + size <= blob.size_bytes, "update fits in blob");

        U64 value_offset = 0;
        while (value_offset < size) {
            TArrayView<U8> block = co_await rwblock(blob, offset + value_offset);
            os::memory_copy(block.ptr, in_value + value_offset, min(block.length, size - value_offset));
            value_offset += block.length;
        }
    }

    coroutine::Task<void> get(Blob auto& blob, U8* out_value, U64 size, U64 offset = 0) {
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

    coroutine::Task<void> insert(Blob auto& blob, const U8* in_value, U64 size, U64 offset = 0) {
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

    // ========================================================================
    // BlobCursor operations (defined after get so the template is visible)
    // ========================================================================

    // Load BlobCursor from a page. Must be called inside an active transaction.
    coroutine::Task<BlobCursor> create_cursor(pager::Pager* pager, U64 page_idx) {
        BlobCursor c;
        co_await load(c.blob, pager, page_idx);
        co_return c;
    }

    // Copy 'size' bytes at cursor.offset into 'dst'.
    // Opens its own short transaction if none is active; otherwise uses the caller's.
    // Advances cursor.offset by 'size'.
    coroutine::Task<void> read(BlobCursor& cursor, U8* dst, U64 size) {
        bool               own_tx = !cursor.blob.pager->transaction_active;
        pager::Transaction tx{cursor.blob.pager};
        if (own_tx) {
            co_await tx.begin();
        }
        co_await get(cursor.blob, dst, size, cursor.offset);
        if (own_tx) {
            co_await tx.commit();
        }
        cursor.offset += size;
    }

    // Read the U64 length prefix of a variable-length column and skip past it
    // (length prefix + payload).
    // Opens its own short transaction if none is active; otherwise uses the caller's.
    coroutine::Task<void> skip_varlen(BlobCursor& cursor) {
        U64                length;
        bool               own_tx = !cursor.blob.pager->transaction_active;
        pager::Transaction tx{cursor.blob.pager};
        if (own_tx) {
            co_await tx.begin();
        }
        co_await get(cursor.blob, reinterpret_cast<U8*>(&length), sizeof(U64), cursor.offset);
        if (own_tx) {
            co_await tx.commit();
        }
        cursor.offset += sizeof(U64) + length;
    }
}
