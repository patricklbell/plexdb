module;
#include "macros.h"
#include <coroutine>

module plexdb.aio;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.arena;
import plexdb.threads;
import plexdb.os.uring;
import plexdb.tagged_union;

namespace plexdb::aio {
    coroutine::Task<> file_read(FileIOContext& ctx, os::Handle file, Rng1U64 rng, void* dst) {
        assert_true(ctx.read && ctx.sync, "Invalid io ctx for read");
        co_await ctx.read(file, rng, dst);
    }

    coroutine::Task<> file_write(FileIOContext& ctx, os::Handle file, Rng1U64 rng, const void* src) {
        assert_true(ctx.write && ctx.sync, "Invalid io ctx for write");
        co_await ctx.write(file, rng, src);
    }

    coroutine::Task<> file_sync(FileIOContext& ctx, os::Handle file) {
        assert_true(!!ctx.sync, "Invalid io ctx for sync");
        co_await ctx.sync(file);
    }

    Pair<FileIOContext, EventConsumer> create_aio_async_file_io_context(os::AIOContext* aio_ctx, arena::Arena& arena, os::Poll& poll) {
        assert_true(aio_ctx && static_cast<bool>(*aio_ctx), "invalid os aio ctx, cannot create async io file io interface");

        U64 max_events = aio_ctx->max_ops;
        auto* handles = default_construct_placement_array(arena::push_array_no_zero<std::coroutine_handle<>>(arena, max_events), max_events);
        auto* tokens  = arena::push_array_no_zero<U64>(arena, max_events);

        os::poll_unblock_on(poll, aio_ctx->notifier);

        return {
            FileIOContext{
                FileReadFunctor{[handles, aio_ctx](os::Handle f, Rng1U64 rng, void* out) -> coroutine::Task<> {
                    co_await coroutine::Awaitable{
                        [handles, aio_ctx, f, rng, out](std::coroutine_handle<> h) {
                            U64 slot = os::aio_submit_read(*aio_ctx, f, rng, out);
                            assert_true(slot != os::INVALID_AIO_SLOT, "no free slots for read, the aio context may be corrupted");
                            handles[slot] = h;
                        },
                        []() {}
                    };
                }},
                FileWriteFunctor{[handles, aio_ctx](os::Handle f, Rng1U64 rng, const void* in) -> coroutine::Task<> {
                    co_await coroutine::Awaitable{
                        [handles, aio_ctx, f, rng, in](std::coroutine_handle<> h) {
                            U64 slot = os::aio_submit_write(*aio_ctx, f, rng, in);
                            assert_true(slot != os::INVALID_AIO_SLOT, "no free slots for write, the aio context may be corrupted");
                            handles[slot] = h;
                        },
                        []() {}
                    };
                }},
                FileSyncFunctor{[handles, aio_ctx](os::Handle f) -> coroutine::Task<> {
                    co_await coroutine::Awaitable{
                        [handles, aio_ctx, f](std::coroutine_handle<> h) {
                            U64 slot = os::aio_submit_sync(*aio_ctx, f);
                            assert_true(slot != os::INVALID_AIO_SLOT, "no free slots for sync, the aio context may be corrupted");
                            handles[slot] = h;
                        },
                        []() {}
                    };
                }},
            },
            EventConsumer{
                .max_events = max_events,
                .on_unblock = OnUnblockFunctor{[handles, aio_ctx, tokens]([[maybe_unused]] const TArrayView<os::PollEvent>&) -> bool {
                    os::aio_notifier_drain(aio_ctx->notifier);

                    U32 n = os::aio_collect_completions(*aio_ctx, tokens, aio_ctx->max_ops);
                    for (U32 i = 0; i < n; i++) {
                        U32 slot = U32(tokens[i]);
                        auto h = handles[slot];
                        handles[slot] = std::coroutine_handle<>{};
                        if (h) h.resume();
                    }
                    return true;
                }}
            }
        };
    }

    Pair<FileIOContext, EventConsumer> create_uring_async_file_io_context(uring::Ring* ring, arena::Arena& arena, os::Poll& poll) {
        assert_true(ring && static_cast<bool>(*ring) && !os::is_zero_handle(ring->event_fd),
                    "invalid ring for async file io context");

        static constexpr U32 MAX_SYNC_SLOTS = 64;
        static constexpr U64 MAX_WAITERS    = 256;

        struct PendingOp {
            std::coroutine_handle<> handle;
            void* dst;
            U32 bytes;
        };

        struct State {
            uring::Ring* ring;
            U64 buf_size;
            U32 buf_count;
            PendingOp* ops;       // arena[buf_count]
            bool* buf_in_use;     // arena[buf_count]
            U32 next_free_buf = 0;
            RingFifo<std::coroutine_handle<>, MAX_WAITERS> buf_waiters{};
            std::coroutine_handle<> sync_handles[MAX_SYNC_SLOTS]{};
            U32 next_sync_slot = 0;

            U32 try_acquire_buf() {
                U32 cur = next_free_buf;
                for (U32 i = 0; i < buf_count; ++i) {
                    if (!buf_in_use[cur]) {
                        buf_in_use[cur] = true;
                        next_free_buf = (cur + 1) % buf_count;
                        return cur;
                    }
                    cur = (cur + 1) % buf_count;
                }
                return MAX_U32;
            }

            coroutine::Task<U32> acquire_buf() {
                while (true) {
                    U32 idx = try_acquire_buf();
                    if (idx != MAX_U32) co_return idx;
                    co_await coroutine::Awaitable{
                        [this](std::coroutine_handle<> h) { push_front(buf_waiters, h); },
                        []() {}
                    };
                }
            }

            void release_buf(U32 idx) {
                buf_in_use[idx] = false;
                if (!empty(buf_waiters)) pop_front(buf_waiters).resume();
            }
        };

        os::poll_unblock_on(poll, ring->event_fd);

        U32 buf_count = ring->buffer_count;
        U64 buf_size  = ring->buffer_size;

        auto* ops    = arena::push_array<PendingOp>(arena, buf_count);
        auto* in_use = arena::push_array<bool>(arena, buf_count);
        auto* state  = new(arena::push_array_no_zero<State>(arena, 1)) State{ring, buf_size, buf_count, ops, in_use};

        auto read_fn = FileReadFunctor{[state](os::Handle file, Rng1U64 rng, void* dst) -> coroutine::Task<> {
            assert_true(rng.end - rng.start <= state->buf_size, "file read larger than ring buffer");
            U32 buf_idx = co_await state->acquire_buf();

            auto& op = state->ops[buf_idx];
            op.dst   = dst;
            op.bytes = 0;

            co_await coroutine::Awaitable{
                [state, file, buf_idx, rng, count = U32(rng.end - rng.start)](std::coroutine_handle<> h) {
                    state->ops[buf_idx].handle = h;
                    uring::sqe_push_file_read(*state->ring, file, buf_idx, rng.start, count, U64(buf_idx));
                    uring::sqe_submit_non_blocking(*state->ring);
                },
                [state, buf_idx]() {
                    auto& op = state->ops[buf_idx];
                    if (op.dst && op.bytes > 0)
                        os::memory_copy(op.dst, state->ring->buffers + U64(buf_idx) * state->buf_size, op.bytes);
                    state->release_buf(buf_idx);
                }
            };
        }};

        auto write_fn = FileWriteFunctor{[state](os::Handle file, Rng1U64 rng, const void* src) -> coroutine::Task<> {
            assert_true(rng.end - rng.start <= state->buf_size, "file write larger than ring buffer");
            U32 buf_idx = co_await state->acquire_buf();

            U32 count = U32(rng.end - rng.start);
            os::memory_copy(state->ring->buffers + U64(buf_idx) * state->buf_size, src, count);

            auto& op = state->ops[buf_idx];
            op.dst   = nullptr;
            op.bytes = 0;

            co_await coroutine::Awaitable{
                [state, file, buf_idx, rng, count](std::coroutine_handle<> h) {
                    state->ops[buf_idx].handle = h;
                    uring::sqe_push_file_write(*state->ring, file, buf_idx, rng.start, count, U64(buf_idx));
                    uring::sqe_submit_non_blocking(*state->ring);
                },
                [state, buf_idx]() {
                    state->release_buf(buf_idx);
                }
            };
        }};

        auto sync_fn = FileSyncFunctor{[state](os::Handle file) -> coroutine::Task<> {
            U32 sync_idx = state->next_sync_slot++ % MAX_SYNC_SLOTS;
            co_await coroutine::Awaitable{
                [state, file, sync_idx](std::coroutine_handle<> h) {
                    state->sync_handles[sync_idx] = h;
                    uring::sqe_push_file_sync(*state->ring, file, U64(sync_idx));
                    uring::sqe_submit_non_blocking(*state->ring);
                },
                []() {}
            };
        }};

        EventConsumer consumer{
            .max_events = U64(buf_count) + MAX_SYNC_SLOTS,
            .on_unblock = OnUnblockFunctor{[state]([[maybe_unused]] const TArrayView<os::PollEvent>&) -> bool {
                if (!uring::ring_drain_event_fd(*state->ring)) return true;

                uring::sqe_submit_non_blocking(*state->ring);
                while (uring::cqe_get_size(*state->ring) > 0) {
                    auto cqe = uring::cqe_top(*state->ring);
                    uring::cqe_pop(*state->ring, 1);

                    visit(cqe, [state](const auto& ev) {
                        using T = RemoveCVRef<decltype(ev)>;
                        if constexpr (SameAs<T, uring::FileReadEvent>) {
                            auto& op = state->ops[ev.buffer_idx];
                            op.bytes = ev.bytes_read;
                            auto h = op.handle;
                            op.handle = {};
                            h.resume();
                        } else if constexpr (SameAs<T, uring::FileWriteEvent>) {
                            auto& op = state->ops[ev.buffer_idx];
                            op.bytes = ev.bytes_written;
                            auto h = op.handle;
                            op.handle = {};
                            h.resume();
                        } else if constexpr (SameAs<T, uring::FileSyncEvent>) {
                            U32 idx = U32(ev.token) % MAX_SYNC_SLOTS;
                            auto h = state->sync_handles[idx];
                            state->sync_handles[idx] = {};
                            if (h) h.resume();
                        }
                    });
                }

                uring::sqe_submit_non_blocking(*state->ring);
                return true;
            }}
        };

        return {FileIOContext{plexdb::move(read_fn), plexdb::move(write_fn), plexdb::move(sync_fn)}, plexdb::move(consumer)};
    }

    FileIOContext create_sync_file_io_context() {
        return FileIOContext{
            FileReadFunctor{[](os::Handle f, Rng1U64 rng, void* dst) -> coroutine::Task<> {
                os::file_read(f, rng, dst);
                co_return;
            }},
            FileWriteFunctor{[](os::Handle f, Rng1U64 rng, const void* src) -> coroutine::Task<> {
                os::file_write(f, rng, src);
                co_return;
            }},
            FileSyncFunctor {[](os::Handle f) -> coroutine::Task<> {
                os::file_sync(f);
                co_return;
            }}
        };
    }

    EventConsumer create_notifier_consumer(os::Notifier& notifier, os::Poll& poll) {
        os::poll_unblock_on(poll, notifier);
        return EventConsumer{
            .max_events = 1,
            .on_unblock = OnUnblockFunctor{[&notifier](const TArrayView<os::PollEvent>& events) -> bool {
                for (const auto& ev : events) {
                    if (ev.handle == notifier.read) {
                        os::drain_notifier(notifier);
                        return false;
                    }
                }
                return true;
            }}
        };
    }
}
