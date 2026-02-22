export module plexdb.blob;

export import plexdb.blob.paged;

import plexdb.base;
import plexdb.os;
import plexdb.blob.paged.detail;

export namespace plexdb::blob {
    // ========================================================================
    // untyped interface
    // ========================================================================
    template<typename Blob>
    void update(Blob& blob, const U8* in_value, U64 size, U64 offset=0) {
        assert_true(offset + size <= blob.size_bytes, "update fits in blob");

        U64 value_offset = 0;
        while (value_offset < size) {
            TArrayView<U8> block = rwblock(blob, offset + value_offset);
            os::memory_copy(block.ptr, in_value + value_offset, min(block.length, size - value_offset));
            value_offset += block.length;
        }
    }

    template<typename Blob>
    void get(Blob& blob, U8* out_value, U64 size, U64 offset=0) {
        assert_true(offset + size <= blob.size_bytes, "gets fits in blob");

        U64 value_offset = 0;
        while (value_offset < size) {
            TArrayView<const U8> block = rblock(blob, offset + value_offset);
            os::memory_copy(out_value + value_offset, block.ptr, min(block.length, size - value_offset));
            value_offset += block.length;
        }
    }

    template<typename Blob>
    void remove(Blob& blob) {
        remove_impl(blob);
    }

    template<typename Blob>
    void resize(Blob& blob, U64 new_size) {
        static_assert(!SameAs<Blob, BlobStaticPaged>, "cannot resize static blob");
        resize_impl(blob, new_size);
    }

    template<typename Blob>
    void append(Blob& blob, const U8* in_value, U64 size) {
        U64 offset = blob.size_bytes;
        resize(blob, blob.size_bytes + size);
        update(blob, in_value, size, offset);
    }

    template<typename Blob>
    void insert(Blob& blob, const U8* in_value, U64 size, U64 offset=0) {
        if (offset + size > blob.size_bytes) {
            resize(blob, offset + size);
        }
        update(blob, in_value, size, offset);
    }

    // ========================================================================
    // typed interface
    // ========================================================================
    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tupdate(Blob& blob, const T* in_value) {
        update(blob, reinterpret_cast<const U8*>(in_value), sizeof(T), 0);
    }

    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tupdate(Blob& blob, const T* in_value, U64* inout_offset) {
        update(blob, reinterpret_cast<const U8*>(in_value), sizeof(T), *inout_offset);
        *inout_offset += sizeof(T);
    }

    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tget(Blob& blob, T* out_value) {
        get(blob, reinterpret_cast<U8*>(out_value), sizeof(T), 0);
    }

    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tget(Blob& blob, T* out_value, U64* inout_offset) {
        get(blob, reinterpret_cast<U8*>(out_value), sizeof(T), *inout_offset);
        *inout_offset += sizeof(T);
    }

    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tappend(Blob& blob, const T* in_value) {
        append(blob, reinterpret_cast<const U8*>(in_value), sizeof(T));
    }

    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tinsert(Blob& blob, const T* in_value) {
        insert(blob, reinterpret_cast<const U8*>(in_value), sizeof(T));
    }
}