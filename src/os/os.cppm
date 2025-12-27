export module plexdb.os;

import plexdb.base;

export namespace plexdb::os {
    // ========================================================================
    // handle
    // ========================================================================
    union Handle {
        U64 u64[1];
        U32 u32[2];
    };
    constexpr inline Handle zero_handle() { return {.u64 = {0} }; }
    constexpr inline bool is_zero_handle(Handle h) { return h.u64[0] == 0; }

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
        READ        = (1<<0),
        WRITE       = (1<<1),
        EXECUTE     = (1<<2),
        APPEND      = (1<<3),
        // @todo
        // SHAREREAD   = (1<<4),
        // SHAREWRITE  = (1<<5),
    };
    Handle file_open(String8 path, AccessFlags flags=AccessFlags(READ|WRITE));
    void   file_close(Handle file);
    void   file_read(Handle file, Rng1U64 rng, U8* out);
    void   file_write(Handle file, Rng1U64 rng, U8* in);
    void   file_sync(Handle file);
}