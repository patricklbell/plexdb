export module plexdb.os.aio;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb::os {
    inline constexpr U64 INVALID_AIO_SLOT = MAX_U64;

    // @todo hide implementation
    struct AIOContext {
        void*  ctx_ptr    = nullptr; // aio_context_t
        void*  slots_ptr  = nullptr; // pool of iocb structs
        bool*  in_use_ptr = nullptr;
        U32    max_ops    = 0;
        U32    next_free  = 0;
        Handle notifier   = zero_handle(); // eventfd for epoll notification

        AIOContext() = default;
        AIOContext(U32 max_ops, Handle notifier);
        ~AIOContext();

        AIOContext(const AIOContext&)            = delete;
        AIOContext& operator=(const AIOContext&) = delete;

        AIOContext(AIOContext&& o) noexcept;
        AIOContext& operator=(AIOContext&& o) noexcept;

        explicit operator bool() const;
    };

    Handle aio_notifier_create();
    void   aio_notifier_drain(Handle notifier);

    // submit async operations. Returns the slot index (completion token),
    // or INVALID_AIO_SLOT if no slots are available or submission fails.
    U64 aio_submit_read(AIOContext& ctx, Handle file, Rng1U64 rng, void* out);
    U64 aio_submit_write(AIOContext& ctx, Handle file, Rng1U64 rng, const void* in);
    U64 aio_submit_sync(AIOContext& ctx, Handle file);

    // collect completed operations (non-blocking). Returns the count written to out_tokens.
    U32 aio_collect_completions(AIOContext& ctx, U64* out_tokens, U32 max_count);
}
