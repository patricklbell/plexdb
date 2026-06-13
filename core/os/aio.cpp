module;
#include <plexdb/macros/macros.h>

#if PLEXDB_OS_LINUX
#include <linux/aio_abi.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#endif

module plexdb.os.aio;

import plexdb.base;
import plexdb.os;

namespace plexdb::os {
#if PLEXDB_OS_LINUX
    static int handle_to_fd(Handle h) {
        return static_cast<int>(h.u32[0]);
    }
    static Handle fd_to_handle(int fd) {
        return Handle{.u32 = {static_cast<U32>(fd)}};
    }

    static long _io_setup(unsigned nr, aio_context_t* ctx) {
        return ::syscall(SYS_io_setup, nr, ctx);
    }
    static long _io_destroy(aio_context_t ctx) {
        return ::syscall(SYS_io_destroy, ctx);
    }
    static long _io_submit(aio_context_t ctx, long nr, struct iocb** iocbpp) {
        return ::syscall(SYS_io_submit, ctx, nr, iocbpp);
    }
    static long _io_getevents(aio_context_t ctx, long min_nr, long nr,
                              struct io_event* events, struct timespec* timeout) {
        return ::syscall(SYS_io_getevents, ctx, min_nr, nr, events, timeout);
    }

    static iocb* slot_ptr(AIOContext& ctx, U32 idx) {
        return static_cast<iocb*>(ctx.slots_ptr) + idx;
    }

    AIOContext::AIOContext(U32 max_ops, Handle notifier)
        : max_ops(max_ops)
        , notifier(notifier) {
        aio_context_t raw_ctx = 0;
        if (_io_setup(max_ops, &raw_ctx) < 0) {
            return;
        }

        ctx_ptr    = reinterpret_cast<void*>(raw_ctx);
        slots_ptr  = ::operator new(sizeof(iocb) * max_ops);
        in_use_ptr = new bool[max_ops]();
        ::memset(slots_ptr, 0, sizeof(iocb) * max_ops);
    }

    AIOContext::~AIOContext() {
        if (ctx_ptr) {
            _io_destroy(reinterpret_cast<aio_context_t>(ctx_ptr));
            ctx_ptr = nullptr;
        }
        if (slots_ptr) {
            ::operator delete(slots_ptr);
            slots_ptr = nullptr;
        }
        if (in_use_ptr) {
            delete[] in_use_ptr;
            in_use_ptr = nullptr;
        }
    }

    AIOContext::AIOContext(AIOContext&& o) noexcept
        : ctx_ptr(o.ctx_ptr)
        , slots_ptr(o.slots_ptr)
        , in_use_ptr(o.in_use_ptr)
        , max_ops(o.max_ops)
        , next_free(o.next_free)
        , notifier(o.notifier) {
        o.ctx_ptr = o.slots_ptr = o.in_use_ptr = nullptr;
        o.max_ops = o.next_free = 0;
        o.notifier              = zero_handle();
    }

    AIOContext& AIOContext::operator=(AIOContext&& o) noexcept {
        if (this != &o) {
            this->~AIOContext();
            new (this) AIOContext(plexdb::move(o));
        }
        return *this;
    }

    AIOContext::operator bool() const {
        return ctx_ptr != nullptr;
    }

    Handle aio_notifier_create() {
        int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        return fd >= 0 ? fd_to_handle(fd) : zero_handle();
    }

    void aio_notifier_drain(Handle notifier) {
        U64 val = 0;
        ::read(handle_to_fd(notifier), &val, sizeof(val));
    }

    static U64 aio_acquire_slot(AIOContext& ctx) {
        U32 start = ctx.next_free;
        U32 cur   = start;
        do {
            if (!ctx.in_use_ptr[cur]) {
                ctx.in_use_ptr[cur] = true;
                ctx.next_free       = (cur + 1) % ctx.max_ops;
                return cur;
            }
            cur = (cur + 1) % ctx.max_ops;
        } while (cur != start);
        return INVALID_AIO_SLOT;
    }

    static bool aio_submit_iocb(AIOContext& ctx, U32 slot) {
        iocb* cbp      = slot_ptr(ctx, slot);
        cbp->aio_flags = IOCB_FLAG_RESFD;
        cbp->aio_resfd = static_cast<U32>(handle_to_fd(ctx.notifier));
        cbp->aio_data  = static_cast<U64>(slot);

        if (_io_submit(reinterpret_cast<aio_context_t>(ctx.ctx_ptr), 1, &cbp) != 1) {
            ctx.in_use_ptr[slot] = false;
            return false;
        }
        return true;
    }

    U64 aio_submit_read(AIOContext& ctx, Handle file, Rng1U64 rng, void* out) {
        assert_true(rng.end > rng.start, "invalid aio file read range");

        U64 slot = aio_acquire_slot(ctx);
        if (slot == INVALID_AIO_SLOT) {
            return INVALID_AIO_SLOT;
        }

        iocb* cb = slot_ptr(ctx, U32(slot));
        ::memset(cb, 0, sizeof(*cb));
        cb->aio_fildes     = static_cast<U32>(handle_to_fd(file));
        cb->aio_lio_opcode = IOCB_CMD_PREAD;
        cb->aio_buf        = reinterpret_cast<U64>(out);
        cb->aio_nbytes     = rng.end - rng.start;
        cb->aio_offset     = static_cast<S64>(rng.start);

        return aio_submit_iocb(ctx, U32(slot)) ? slot : INVALID_AIO_SLOT;
    }

    U64 aio_submit_write(AIOContext& ctx, Handle file, Rng1U64 rng, const void* in) {
        assert_true(rng.end > rng.start, "invalid aio file write range");

        U64 slot = aio_acquire_slot(ctx);
        if (slot == INVALID_AIO_SLOT) {
            return INVALID_AIO_SLOT;
        }

        iocb* cb = slot_ptr(ctx, U32(slot));
        ::memset(cb, 0, sizeof(*cb));
        cb->aio_fildes     = static_cast<U32>(handle_to_fd(file));
        cb->aio_lio_opcode = IOCB_CMD_PWRITE;
        cb->aio_buf        = reinterpret_cast<U64>(in);
        cb->aio_nbytes     = rng.end - rng.start;
        cb->aio_offset     = static_cast<S64>(rng.start);

        return aio_submit_iocb(ctx, U32(slot)) ? slot : INVALID_AIO_SLOT;
    }

    U64 aio_submit_sync(AIOContext& ctx, Handle file) {
        U64 slot = aio_acquire_slot(ctx);
        if (slot == INVALID_AIO_SLOT) {
            return INVALID_AIO_SLOT;
        }

        iocb* cb = slot_ptr(ctx, U32(slot));
        ::memset(cb, 0, sizeof(*cb));
        cb->aio_fildes     = static_cast<U32>(handle_to_fd(file));
        cb->aio_lio_opcode = IOCB_CMD_FSYNC;

        return aio_submit_iocb(ctx, U32(slot)) ? slot : INVALID_AIO_SLOT;
    }

    U32 aio_collect_completions(AIOContext& ctx, U64* out_tokens, U32 max_count) {
        struct io_event events[64];
        U32             to_collect = max_count < 64 ? max_count : 64;
        struct timespec timeout    = {0, 0};
        long            n          = _io_getevents(
            reinterpret_cast<aio_context_t>(ctx.ctx_ptr),
            0, static_cast<long>(to_collect), events, &timeout);
        if (n <= 0) {
            return 0;
        }

        for (long i = 0; i < n; i++) {
            U32 slot             = static_cast<U32>(events[i].data);
            ctx.in_use_ptr[slot] = false;
            out_tokens[i]        = static_cast<U64>(slot);
        }
        return static_cast<U32>(n);
    }

#else // @todo

    AIOContext::AIOContext([[maybe_unused]] U32 max_ops, [[maybe_unused]] Handle notifier) {
    }
    AIOContext::~AIOContext() {
        static_assert(false);
    }
    AIOContext::AIOContext(AIOContext&&) noexcept            = default;
    AIOContext& AIOContext::operator=(AIOContext&&) noexcept = default;
    AIOContext::operator bool() const {
        return false;
    }

    Handle aio_notifier_create() {
        return zero_handle();
    }
    void aio_notifier_drain([[maybe_unused]] Handle) {
        static_assert(false);
    }
    U64 aio_submit_read([[maybe_unused]] AIOContext&, [[maybe_unused]] Handle, [[maybe_unused]] Rng1U64 rng, [[maybe_unused]] void* out) {
        static_assert(false);
        return INVALID_AIO_SLOT;
    }
    U64 aio_submit_write([[maybe_unused]] AIOContext&, [[maybe_unused]] Handle, [[maybe_unused]] Rng1U64 rng, [[maybe_unused]] const void* in) {
        static_assert(false);
        return INVALID_AIO_SLOT;
    }
    U64 aio_submit_sync([[maybe_unused]] AIOContext&, [[maybe_unused]] Handle) {
        static_assert(false);
        return INVALID_AIO_SLOT;
    }
    U32 aio_collect_completions([[maybe_unused]] AIOContext&, [[maybe_unused]] U64*, [[maybe_unused]] U32) {
        static_assert(false);
        return 0;
    }
#endif
}
