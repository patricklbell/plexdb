export module plexdb.os.core;

import plexdb.base;

export namespace plexdb::os {
    // ========================================================================
    // handle
    // ========================================================================
    union Handle {
        U64 u64[1];
        U32 u32[2];
    };
    constexpr inline Handle zero_handle() { return {.u64 = {~0_u64} }; }
    constexpr inline bool is_zero_handle(Handle h) { return h.u64[0] == ~0_u64; }
    constexpr inline bool operator==(Handle a, Handle b) { return a.u64[0] == b.u64[0]; }
    constexpr inline bool operator!=(Handle a, Handle b) { return a.u64[0] != b.u64[0]; }
    constexpr inline U64 hash(Handle h) { return plexdb::hash(h.u64[0]); }

    // ========================================================================
    // memory
    // ========================================================================
    U8* allocate(U64 bytes);
    U8* allocate_zero(U64 bytes);
    void deallocate(void* ptr);

    void* memory_copy(void* dst, const void* src, U64 bytes) noexcept;
    void* memory_move(void* dst, const void* src, U64 bytes) noexcept;
    void* memory_zero(void* dst, U64 bytes) noexcept;

    template<class To>
        requires (TriviallyCopyable<To> && TriviallyConstructible<To>)
    To memory_cast(const void* src) noexcept {
        To dst;
        memory_copy(&dst, src, sizeof(To));
        return dst;
    }

    template<typename T>
    void memory_copy(U8* dst, const T* src) {
        memory_copy(dst, src, sizeof(T));
    }
    template<bool check_length=true, typename Length, typename Size, typename Byte>
    void memory_copy(const ArrayView<Length,Size,Byte>& dst, const ArrayView<Length,Size,Byte>& src) {
        if constexpr (check_length)
            assert_true(dst.length == src.length, "matching view lengths");
        assert_true(dst.el_size == src.el_size, "matching element sizes");
        memory_copy(dst.ptr, src.ptr, src.el_size*src.length);
    }
    template<typename Length, typename Size, typename Byte>
    void memory_copy(U8* dst, const ArrayView<Length,Size,Byte>& src) {
        memory_copy(dst, src.ptr, src.el_size*src.length);
    }
    template<typename Length, typename Size, typename Byte>
    void memory_shift_up(const ArrayView<Length,Size,Byte>& src, Length offset=1) {
        memory_move(src.ptr + src.el_size*offset, src.ptr, src.el_size*src.length);
    }
    template<typename Length, typename Size, typename Byte>
    void memory_shift_down(const ArrayView<Length,Size, Byte>& src, Length offset=1) {
        memory_move(src.ptr - src.el_size*offset, src.ptr, src.el_size*src.length);
    }
    template<typename Length, typename Size, typename Byte>
    void memory_shift_up_and_expand_view(ArrayView<Length,Size,Byte>& src, Length offset=1) {
        memory_shift_up(src, offset);
        src.length++;
    }
    template<typename Length, typename Size, typename Byte>
    void memory_shift_down_and_expand_view(ArrayView<Length,Size, Byte>& src, Length offset=1) {
        memory_shift_down(src, offset);
        src.ptr -= src.el_size*offset;
        src.length++;
    }

    template<bool check_length=true, typename U, typename V, typename Length>
        requires TriviallyCopyable<V> && SameAs<RemoveCV<U>,RemoveCV<V>>
    void memory_copy(const TArrayView<U,Length>& dst, const TArrayView<V,Length>& src) {
        if constexpr (check_length)
            assert_true(dst.length == src.length, "matching view lengths");
        memory_copy(dst.ptr, src.ptr, sizeof(V)*src.length);
    }
    template<typename T, typename Length>
    void memory_shift_up(const TArrayView<T,Length>& src, Length offset=1) {
        memory_move(src.ptr + offset, src.ptr, sizeof(T)*src.length);
    }
    template<typename T, typename Length>
    void memory_shift_down(const TArrayView<T,Length>& src, Length offset=1) {
        memory_move(src.ptr - offset, src.ptr, sizeof(T)*src.length);
    }
    template<typename T, typename Length>
    void memory_shift_up_and_expand_view(TArrayView<T,Length>& src, Length offset=1) {
        memory_shift_up(src, offset);
        src.length++;
    }
    template<typename T, typename Length>
    void memory_shift_down_and_expand_view(TArrayView<T,Length>& src, Length offset=1) {
        memory_shift_down(src, offset);
        src.ptr -= offset;
        src.length++;
    }

    template<typename U, typename V, typename L1, typename L2>
        requires TriviallyCopyable<V> && SameAs<RemoveCV<U>,RemoveCV<V>>
    void memory_append_copy(TArrayView<U,L1>& dst, const TArrayView<V,L2>& src) {
        static_assert(sizeof(U) == sizeof(V));
        assert_true(dst.ptr != nullptr, "cannot append to empty view");
        assert_true(src.length > 0 && src.ptr != nullptr, "cannot append empty view");
        memory_copy(dst.ptr + dst.length, src.ptr, sizeof(U)*src.length);
        dst.length += src.length;
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

    struct FileStats {
        U64 byte_count;
    };

    Handle    file_open(String8 path, AccessFlags flags=AccessFlags(READ|WRITE));
    Handle    file_tmp(bool delete_on_close=true);
    void      file_close(Handle file);
    void      file_delete(String8 path);
    void      file_read(Handle file, Rng1U64 rng, void* out);
    void      file_write(Handle file, Rng1U64 rng, const void* in);
    void      file_sync(Handle file);
    bool      file_exists(String8 path);
    void      file_seek(Handle file, U64 offset);
    void      file_resize_zero(Handle file, U64 new_byte_count);
    U64       file_get_offset(Handle file);
    FileStats file_get_stats(Handle file);

    struct File {
        Handle handle;

        ~File();

        operator Handle() const;
        operator bool() const { return !os::is_zero_handle(this->handle); }
    };

    // ========================================================================
    // process
    // ========================================================================
    // @note returns empty optional on failure, zero handle for child process
    // and the child process handle for parent.
    Optional<Handle> process_fork();
    bool             process_wait(Handle process_to_wait_on);
    Handle           process_get_handle();
    void             process_exit(int code);
    void             process_pause();

    // ========================================================================
    // streams
    // ========================================================================
    Handle stdin_stream();
    Handle stdout_stream();

    void   stream_write(Handle h, const void* data, U64 length);
    void   stream_write(Handle h, String8 s);
    U64    stream_read(Handle h, void* out, U64 max_length);
    void   stream_close(Handle h);
}
