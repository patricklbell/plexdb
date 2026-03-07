module;
#include "macros.h"
#if PLEXDB_OS_LINUX
    #include <stdlib.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
#endif

module plexdb.os.core;

namespace plexdb::os {
    File::~File()                   { file_close(this->handle); }
    File::operator Handle() const   { return this->handle; }
    
#if PLEXDB_OS_LINUX
    // ========================================================================
    // memory
    // ========================================================================
    U8* allocate(U64 bytes) {
        return reinterpret_cast<U8*>(malloc(bytes));
    }
    U8* allocate_zero(U64 bytes) {
        // @perf
        U8* data = allocate(bytes);
        memory_zero(data, bytes);
        return data;
    }
    void deallocate(void* ptr) {
        free(ptr);
    }
    void* memory_copy(void* dst, const void* src, U64 bytes) noexcept {
        return __builtin_memcpy(dst, src, bytes);
    }
    void* memory_move(void* dst, const void* src, U64 bytes) noexcept {
        return __builtin_memmove(dst, src, bytes);
    }
    void* memory_zero(void* dst, U64 bytes) noexcept {
        return __builtin_memset(dst, 0, bytes);
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

        int fd = open(path.c_str(), l_flags, l_mode);
        if (fd == -1) {
            return zero_handle();
        }

        return fd_to_handle(fd);
    }
    Handle file_tmp(bool delete_on_close) {
        char filename[] = "plexdb_XXXXXX";
        int fd = mkstemp(filename);
        if (fd == -1) {
            return zero_handle();
        }
        if (delete_on_close) {
            int err = unlink(filename);
            assert_true(err == 0, "tmp file delete error");
        }
        return fd_to_handle(fd);
    }
    void file_close(Handle file) {
        int fd = handle_to_fd(file);
        int err = close(fd);
        assert_true(err == 0, "file close error"); // @todo
    }
    void file_delete(String8 path) {
        int err = unlink(path.c_str());
        assert_true(err == 0, "file delete error");
    }
    void file_read(Handle file, Rng1U64 rng, void* out) {
        assert_true(rng.end > rng.start, "non-positive range");
        assert_true(static_cast<U64>((S64)rng.start) == rng.start, "range overflow");

        int fd = handle_to_fd(file);
        off_t res_off = lseek(fd, off_t{(S64)rng.start}, SEEK_SET);
        assert_true(res_off == (S64)rng.start, "file seek error");
        ssize_t bytes_read = read(fd, out, rng.end - rng.start);
        assert_true(static_cast<U64>(bytes_read) == rng.end - rng.start, "file read error");
    }
    void file_write(Handle file, Rng1U64 rng, const void* in) {
        assert_true(rng.end > rng.start, "non-positive range");
        assert_true(static_cast<U64>((S64)rng.start) == rng.start, "range overflow");

        int fd = handle_to_fd(file);
        off_t res_off = lseek(fd, off_t{(S64)rng.start}, SEEK_SET);
        assert_true(res_off == (S64)rng.start, "file seek error");
        ssize_t bytes_written = write(fd, in, rng.end - rng.start);
        assert_true(static_cast<U64>(bytes_written) == rng.end - rng.start, "file write error");
    }
    void file_sync(Handle file) {
        int fd = handle_to_fd(file);
        int err = fsync(fd);
        assert_true(err == 0, "file sync error");
    }
    bool file_exists(String8 path) {
        struct stat st;
        int err = stat(path.c_str(), &st);
        return err == 0;
    }
    void file_seek(Handle file, U64 offset) {
        assert_true(static_cast<U64>((S64)offset) == offset, "range overflow");
        
        int fd = handle_to_fd(file);
        off_t res_off = lseek(fd, off_t{(S64)offset}, SEEK_SET);
        assert_true(res_off == (S64)offset, "file seek error");
    }
    void file_resize_zero(Handle file, U64 new_byte_count) {
        assert_true(static_cast<U64>((S64)new_byte_count) == new_byte_count, "range overflow");

        int fd = handle_to_fd(file);
        int err = ftruncate(fd, off_t{(S64)new_byte_count});
        assert_true(err == 0, "file truncate error");
    }
    U64 file_get_offset(Handle file) {
        int fd = handle_to_fd(file);
        return static_cast<U64>(lseek(fd, 0, SEEK_CUR));
    }
    FileStats file_get_stats(Handle file) {
        int fd = handle_to_fd(file);

        struct stat stat;
        int err = fstat(fd, &stat);
        assert_true(err == 0, "file stat error");
        return FileStats{
            .byte_count = static_cast<U64>(stat.st_size),
        };
    }

    // ========================================================================
    // streams
    // ========================================================================
    Handle stdin_stream() {
        return fd_to_handle(STDIN_FILENO);
    }

    Handle stdout_stream() {
        return fd_to_handle(STDOUT_FILENO);
    }

    void stream_write(Handle h, const void* data, U64 length) {
        int fd = handle_to_fd(h);
        U64 written = 0;
        while (written < length) {
            ssize_t n = write(fd, static_cast<const char*>(data) + written, length - written);
            if (n <= 0) break;
            written += static_cast<U64>(n);
        }
    }

    void stream_write(Handle h, String8 s) {
        stream_write(h, s.data, s.length);
    }

    U64 stream_read(Handle h, void* out, U64 max_length) {
        int fd = handle_to_fd(h);
        ssize_t n = read(fd, out, max_length);
        if (n <= 0) return 0;
        return static_cast<U64>(n);
    }

    void stream_close(Handle h) {
        int fd = handle_to_fd(h);
        close(fd);
    }

#else
    #error "OS library not implemented."
#endif

}