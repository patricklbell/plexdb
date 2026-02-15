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

    // ========================================================================
    // typed interface
    // ========================================================================
    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tupdate(Blob& blob, const T* in_value, U64 offset=0) {
        update(blob, reinterpret_cast<U8*>(in_value), sizeof(T), offset);
    }

    template<typename T, typename Blob>
        requires TriviallyCopyable<T>
    void tget(Blob& blob, T* out_value, U64 offset=0) {
        get(blob, reinterpret_cast<T*>(out_value), sizeof(T), offset);
    }
}