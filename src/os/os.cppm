export module plexdb.os;

import plexdb.base;

export namespace plexdb::os {
    union Handle {
        U64 u64[1];
        U32 u32[2];
    };

    // ========================================================================
    // memory
    // ========================================================================
    U8* allocate(U64 size);
    void deallocate(void* ptr);

    void memory_copy(void* dst, const void* src, U64 bytes) noexcept;
    void memory_move(void* dst, const void* src, U64 bytes) noexcept;
    void memory_zero(void* dst, U64 bytes) noexcept;

    template<class To>
        requires (TriviallyCopyable<To> && TriviallyConstructible<To>)
    To memory_cast(void* src) noexcept {
        To dst;
        memory_copy(&dst, src, sizeof(To));
        return dst;
    }

    template<bool check_length=true, typename Size, typename Length>
    void memory_copy(const ArrayView<Size,Length>& dst, const ArrayView<Size,Length>& src) {
        if constexpr (check_length)
            assert_true(dst.length == src.length, "matching view lengths");
        assert_true(dst.el_size == src.el_size, "matching element sizes");
        memory_copy(dst.ptr, src.ptr, src.el_size*src.length);
    }
    template<typename Size, typename Length>
    void memory_copy(U8* dst, const ArrayView<Size,Length>& src) {
        memory_copy(dst, src.ptr, src.el_size*src.length);
    }
    template<typename Size, typename Length>
    void memory_shift_right(const ArrayView<Size,Length>& src, Length offset=1) {
        memory_move(src.ptr + src.el_size*offset, src.ptr, src.el_size*src.length);
    }
    template<typename Size, typename Length>
    void memory_shift_left(const ArrayView<Size,Length>& src, Length offset=1) {
        memory_move(src.ptr - src.el_size*offset, src.ptr, src.el_size*src.length);
    }

    template<bool check_length=true, typename T, typename Length>
    void memory_copy(const TArrayView<T,Length>& dst, const TArrayView<T,Length>& src) {
        if constexpr (check_length)
            assert_true(dst.length == src.length, "matching view lengths");
        memory_copy(dst.ptr, src.ptr, sizeof(T)*src.length);
    }
    template<typename T, typename Length>
    void memory_shift_right(const TArrayView<T,Length>& src, Length offset=1) {
        memory_move(src.ptr + offset, src.ptr, sizeof(T)*src.length);
    }
    template<typename T, typename Length>
    void memory_shift_left(const TArrayView<T,Length>& src, Length offset=1) {
        memory_move(src.ptr - offset, src.ptr, sizeof(T)*src.length);
    }

    // ========================================================================
    // file
    // ========================================================================
    enum AccessFlags {
        Read        = (1<<0),
        Write       = (1<<1),
        Execute     = (1<<2),
        Append      = (1<<3),
        ShareRead   = (1<<4),
        ShareWrite  = (1<<5),
        Inherited   = (1<<6),
    };
    Handle file_open(String8 path, AccessFlags flags=AccessFlags::Read|AccessFlags::Write);
    void   file_close(Handle file);
    U64    file_read(Handle file, Rng1U64 rng, U8* out);
    U64    file_write(Handle file, Rng1U64 rng, U8* in);
}