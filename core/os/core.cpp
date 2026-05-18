module;
#include <plexdb/macros/macros.h>
#include <plexdb/support/tracy/tracy.hpp>
#if PLEXDB_OS_LINUX
    #include <stdlib.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <sys/eventfd.h>
    #include <linux/aio_abi.h>
    #include <errno.h>
#endif

module plexdb.os.core;

import plexdb.base;

namespace plexdb::os {
    File::File(Handle h): handle(h) {}
    File::File(File&& other) noexcept: handle(other.handle) {
        other.handle = zero_handle();
    }
    File& File::operator=(File&& other) noexcept {
        if (this != &other) {
            if (!is_zero_handle(this->handle)) file_close(this->handle);

            handle = other.handle;
            other.handle = zero_handle();
        }

        return *this;
    }
    File::~File() {
        if (!is_zero_handle(this->handle)) file_close(this->handle);
    }
    File::operator Handle() const {
        return this->handle;
    }

#if PLEXDB_OS_LINUX
    // ========================================================================
    // memory
    // ========================================================================
    U8* allocate(U64 bytes) {
        void* ptr = malloc(bytes);
        TracyAlloc(ptr, bytes) ;
        return reinterpret_cast<U8*>(ptr);
    }
    void deallocate(void* ptr) {
        TracyFree(ptr);
        free(ptr);
    }
    U8* allocate_zero(U64 bytes) {
        // @perf
        U8* data = allocate(bytes);
        memory_zero(data, bytes);
        return data;
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
    void* memory_set(void* dst, U8 value, U64 bytes) noexcept {
        return __builtin_memset(dst, value, bytes);
    }
    S32 memory_compare(const void* a, const void* b, U64 bytes) noexcept {
        return __builtin_memcmp(a, b, bytes);
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
        if (flags&TRUNCATE) l_flags |= O_TRUNC;

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
    // async file I/O
    // ========================================================================
    constexpr U32 MAX_PENDING_AIO = 256;

    struct AsyncFileCtx {
        aio_context_t aio_ctx;
        U32 max_events;
        U32 pending_count;
        Handle wake_fd;
        U32 pending_idx;
        iocb* pending_iocbs[MAX_PENDING_AIO];
    };

    AsyncFileCtx* async_file_ctx_create(U32 max_events) {
        #if PLEXDB_OS_LINUX
            assert_true(max_events > 0, "max_events must be positive");
            assert_true(max_events <= MAX_PENDING_AIO, "max_events exceeds limit");

            AsyncFileCtx* ctx = reinterpret_cast<AsyncFileCtx*>(os::allocate_zero(sizeof(AsyncFileCtx)));
            assert_true(ctx != nullptr, "failed to allocate async file context");

            ctx->max_events = max_events;
            ctx->pending_count = 0;
            ctx->pending_idx = 0;

            long ret = syscall(7, max_events, &ctx->aio_ctx); // __NR_io_setup
            assert_true(ret >= 0, "io_setup syscall failed");

            ctx->wake_fd = Handle{.u32={static_cast<U32>(eventfd(0, EFD_NONBLOCK))}};
            assert_true(!os::is_zero_handle(ctx->wake_fd), "eventfd creation failed");

            return ctx;
        #else
            assert_true(false, "async file I/O not supported on this platform");
            return nullptr;
        #endif
    }

    void async_file_ctx_destroy(AsyncFileCtx* ctx) {
        #if PLEXDB_OS_LINUX
            assert_true(ctx != nullptr, "null context in async_file_ctx_destroy");

            if (!os::is_zero_handle(ctx->wake_fd)) {
                int err = close(static_cast<int>(ctx->wake_fd.u32[0]));
                assert_true(err == 0, "eventfd close failed");
            }

            long ret = syscall(6, ctx->aio_ctx); // __NR_io_destroy
            assert_true(ret == 0, "io_destroy syscall failed");

            os::deallocate(ctx);
        #else
            assert_true(false, "async file I/O not supported on this platform");
        #endif
    }

    bool async_file_submit_read(AsyncFileCtx* ctx, Handle file, U64 offset, U32 count, U8* buf, U64 token) {
        #if PLEXDB_OS_LINUX
            assert_true(ctx != nullptr, "null context in async_file_submit_read");
            assert_true(!os::is_zero_handle(file), "invalid file handle in async_file_submit_read");
            assert_true(buf != nullptr, "null buffer in async_file_submit_read");
            assert_true(count > 0, "zero count in async_file_submit_read");

            iocb* iocb_ptr = reinterpret_cast<iocb*>(os::allocate_zero(sizeof(iocb)));
            assert_true(iocb_ptr != nullptr, "failed to allocate iocb");

            iocb_ptr->aio_fildes = handle_to_fd(file);
            iocb_ptr->aio_offset = static_cast<off_t>(offset);
            iocb_ptr->aio_nbytes = count;
            iocb_ptr->aio_buf = reinterpret_cast<U64>(buf);
            iocb_ptr->aio_data = token;
            iocb_ptr->aio_flags = IOCB_FLAG_RESFD;
            iocb_ptr->aio_resfd = static_cast<U32>(ctx->wake_fd.u32[0]);

            iocb* iocbs[1] = { iocb_ptr };
            long ret = syscall(18, ctx->aio_ctx, 1, iocbs); // __NR_io_submit
            assert_true(ret == 1, "io_submit failed for read");

            ctx->pending_count++;
            ctx->pending_iocbs[ctx->pending_idx % MAX_PENDING_AIO] = iocb_ptr;
            ctx->pending_idx++;
            return true;
        #else
            assert_true(false, "async file I/O not supported on this platform");
            return false;
        #endif
    }

    bool async_file_submit_write(AsyncFileCtx* ctx, Handle file, U64 offset, U32 count, const U8* buf, U64 token) {
        #if PLEXDB_OS_LINUX
            assert_true(ctx != nullptr, "null context in async_file_submit_write");
            assert_true(!os::is_zero_handle(file), "invalid file handle in async_file_submit_write");
            assert_true(buf != nullptr, "null buffer in async_file_submit_write");
            assert_true(count > 0, "zero count in async_file_submit_write");

            iocb* iocb_ptr = reinterpret_cast<iocb*>(os::allocate_zero(sizeof(iocb)));
            assert_true(iocb_ptr != nullptr, "failed to allocate iocb");

            iocb_ptr->aio_fildes = handle_to_fd(file);
            iocb_ptr->aio_offset = static_cast<off_t>(offset);
            iocb_ptr->aio_nbytes = count;
            iocb_ptr->aio_buf = reinterpret_cast<U64>(const_cast<U8*>(buf));
            iocb_ptr->aio_data = token;
            iocb_ptr->aio_flags = IOCB_FLAG_RESFD;
            iocb_ptr->aio_resfd = static_cast<U32>(ctx->wake_fd.u32[0]);
            iocb_ptr->aio_lio_opcode = IOCB_CMD_PWRITEV;

            iocb* iocbs[1] = { iocb_ptr };
            long ret = syscall(18, ctx->aio_ctx, 1, iocbs); // __NR_io_submit
            assert_true(ret == 1, "io_submit failed for write");

            ctx->pending_count++;
            ctx->pending_iocbs[ctx->pending_idx % MAX_PENDING_AIO] = iocb_ptr;
            ctx->pending_idx++;
            return true;
        #else
            assert_true(false, "async file I/O not supported on this platform");
            return false;
        #endif
    }

    bool async_file_submit_sync(AsyncFileCtx* ctx, Handle file, U64 token) {
        #if PLEXDB_OS_LINUX
            assert_true(ctx != nullptr, "null context in async_file_submit_sync");
            assert_true(!os::is_zero_handle(file), "invalid file handle in async_file_submit_sync");

            iocb* iocb_ptr = reinterpret_cast<iocb*>(os::allocate_zero(sizeof(iocb)));
            assert_true(iocb_ptr != nullptr, "failed to allocate iocb");

            iocb_ptr->aio_fildes = handle_to_fd(file);
            iocb_ptr->aio_offset = 0;
            iocb_ptr->aio_nbytes = 0;
            iocb_ptr->aio_buf = token;
            iocb_ptr->aio_data = token;
            iocb_ptr->aio_flags = IOCB_FLAG_RESFD;
            iocb_ptr->aio_resfd = static_cast<U32>(ctx->wake_fd.u32[0]);
            iocb_ptr->aio_lio_opcode = IOCB_CMD_FSYNC;

            iocb* iocbs[1] = { iocb_ptr };
            long ret = syscall(18, ctx->aio_ctx, 1, iocbs); // __NR_io_submit
            assert_true(ret == 1, "io_submit failed for sync");

            ctx->pending_count++;
            ctx->pending_iocbs[ctx->pending_idx % MAX_PENDING_AIO] = iocb_ptr;
            ctx->pending_idx++;
            return true;
        #else
            assert_true(false, "async file I/O not supported on this platform");
            return false;
        #endif
    }

    bool async_file_submit_flush(AsyncFileCtx* ctx) {
        assert_true(ctx != nullptr, "null context in async_file_submit_flush");
        return true;
    }

    U32 async_file_drain(AsyncFileCtx* ctx, AsyncFileEvent* out, U32 max) {
        #if PLEXDB_OS_LINUX
            assert_true(ctx != nullptr, "null context in async_file_drain");
            assert_true(out != nullptr, "null output buffer in async_file_drain");
            assert_true(max > 0, "zero max in async_file_drain");

            io_event events[16];
            U32 batch = min(max, U32(16));

            timespec timeout = { .tv_sec = 0, .tv_nsec = 0 };
            long ret = syscall(29, ctx->aio_ctx, 0, batch, events, &timeout); // __NR_io_getevents
            assert_true(ret >= 0, "io_getevents syscall failed");

            if (ret <= 0) return 0;

            U32 count = 0;
            for (long i = 0; i < ret && count < max; i++) {
                assert_true(events[i].res >= 0 || events[i].res == -EINPROGRESS,
                    "unexpected negative result in drain");

                out[count].token = events[i].data;
                out[count].bytes = static_cast<U32>(events[i].res >= 0 ? events[i].res : 0);
                out[count].error = (events[i].res < 0) ? AsyncFileError::IO : AsyncFileError::None;

                if (ctx->pending_count > 0) {
                    U32 idx = (ctx->pending_idx - ctx->pending_count) % MAX_PENDING_AIO;
                    U8* iocb_ptr = reinterpret_cast<U8*>(ctx->pending_iocbs[idx]);
                    os::deallocate(iocb_ptr);
                }
                ctx->pending_count--;
                count++;
            }

            return count;
        #else
            assert_true(false, "async file I/O not supported on this platform");
            return 0;
        #endif
    }

    void async_file_wait_one(AsyncFileCtx* ctx) {
        #if PLEXDB_OS_LINUX
            assert_true(ctx != nullptr, "null context in async_file_wait_one");
            assert_true(!os::is_zero_handle(ctx->wake_fd), "invalid wake fd in async_file_wait_one");

            U64 val;
            ssize_t n = read(static_cast<int>(ctx->wake_fd.u32[0]), &val, sizeof(val));
            assert_true(n == sizeof(val) || n == -1, "unexpected read result from eventfd");
        #else
            assert_true(false, "async file I/O not supported on this platform");
        #endif
    }

    Handle async_file_wake_fd(AsyncFileCtx* ctx) {
        assert_true(ctx != nullptr, "null context in async_file_wake_fd");
        return ctx->wake_fd;
    }

    // ========================================================================
    // process
    // ========================================================================
    static_assert(sizeof(pid_t) == sizeof(U32));
    static Handle linux_pid_to_handle(pid_t pid    ) { return Handle{.u32 = {memory_cast<U32>(&pid)} }; }
    static pid_t  linux_handle_to_pid(Handle handle) { return memory_cast<S32>(&handle.u32[0]); }

    Optional<Handle> process_fork() {
        pid_t pid = ::fork();
        static_assert(sizeof(pid_t) < sizeof(U64), "zero handle sentinel is guaranteed to be out of range for pid");
        if (pid == -1) return {};
        if (pid == 0) return {zero_handle()};
        return {linux_pid_to_handle(pid)};
    }
    bool process_wait(Handle process_to_wait_on) {
        int status = 0;
        ::waitpid(linux_handle_to_pid(process_to_wait_on), &status, 0);
        if (WIFEXITED(status))   return true;
        if (WIFSIGNALED(status)) return true;
        return false;
    }
    Handle process_get_handle() {
        return linux_pid_to_handle(::getpid());
    }
    void process_exit(int code) {
        ::_exit(code);
    }
    void process_pause() {
        ::pause();
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
