module plexdb.os;

namespace plexdb::os {
    // ========================================================================
    // memory
    // ========================================================================
    U8* allocate(U64 size) {
        return reinterpret_cast<U8*>(malloc(size));
    }
    void deallocate(void* ptr) {
        free(ptr);
    }
    void memory_copy(void* dst, const void* src, U64 bytes) noexcept {
        __builtin_memcpy(dst, src, bytes);
    }
    void memory_move(void* dst, const void* src, U64 bytes) noexcept {
        __builtin_memmove(dst, src, bytes);
    }
    void memory_zero(void* dst, U64 bytes) noexcept {
        __builtin_memset(dst, 0, bytes);
    }

    // ========================================================================
    // file
    // ========================================================================
    static Handle fd_to_handle(int fd) {
        assert_true(fd != -1, "invalid linux file descriptor");
        return Handle{.u32={static_cast<U32>(fd)}};
    }
    static int handle_to_fd(Handle handle) {
        assert_true(!is_zero_handle(handle), "invalid file handle");
        return static_cast<int>(handle.u32[0]);
    }

    Handle file_open(String8 path, AccessFlags flags) {
        int l_flags = O_RDWR;
        if (!(flags&READ))  l_flags  = O_WRONLY;
        if (!(flags&WRITE)) l_flags  = O_RDONLY;
        l_flags |= O_CREAT;
        if (flags&APPEND)   l_flags |= O_APPEND;

        mode_t l_mode = S_IRUSR | S_IWUSR;
        if (flags&EXECUTE) l_mode |= S_IXUSR;

        int fd = open(path.c_str, l_flags, l_mode);
        if (fd == -1) {
            PLEXDB_DEBUG_X(int tmp = errno);
            return zero_handle();
        }

        return fd_to_handle(fd);
    }
    void file_close(Handle file) {
        int fd = handle_to_fd(file);
        int err = close(fd);
        assert_true(err == 0, "file close error"); // @todo
    }
    void file_read(Handle file, Rng1U64 rng, void* out) {
        int fd = handle_to_fd(file);
        off_t res_off = lseek(fd, rng.start, SEEK_SET);
        assert_true(static_cast<U64>(res_off) == rng.start, "file seek error");
        ssize_t bytes_read = read(fd, out, rng.end - rng.start);
        assert_true(static_cast<U64>(bytes_read) == rng.end - rng.start, "file read error");
    }
    void file_write(Handle file, Rng1U64 rng, const void* in) {
        int fd = handle_to_fd(file);
        off_t res_off = lseek(fd, rng.start, SEEK_SET);
        assert_true(static_cast<U64>(res_off) == rng.start, "file seek error");
        ssize_t bytes_written = write(fd, in, rng.end - rng.start);
        assert_true(static_cast<U64>(bytes_written) == rng.end - rng.start, "file write error");
    }
    void file_sync(Handle file) {
        int fd = handle_to_fd(file);
        int err = fsync(fd);
        assert_true(err == 0, "file sync error"); // @todo
    }
    U64 file_get_offset(Handle file) {
        int fd = handle_to_fd(file);
        return static_cast<U64>(lseek(fd, 0, SEEK_CUR));
    }
    void file_seek(Handle file, U64 offset) {
        int fd = handle_to_fd(file);
        off_t res_off = lseek(fd, offset, SEEK_SET);
        assert_true(static_cast<U64>(res_off) == offset, "file seek error");
    }
}