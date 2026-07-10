module;
#include <plexdb/macros/macros.h>
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

        struct State {
            os::AIOContext*                aio_ctx;
            std::coroutine_handle<>*       handles; // arena[max_ops]
            Deque<std::coroutine_handle<>> slot_waiters{};

            void wake_one_waiter() {
                if (slot_waiters.length > 0) {
                    auto h = *front(slot_waiters);
                    pop_front(slot_waiters);
                    h.resume();
                }
            }

            coroutine::Task<> wait_for_slot() {
                // @note node is added then unconditionally removed, so the memory remains valid
                Deque<std::coroutine_handle<>>::Node node{};

                co_await coroutine::Awaitable{
                    [s = this, n = &node](std::coroutine_handle<> h) { n->value = h; push_back(s->slot_waiters, n); },
                    []() {},
                    [s = this, n = &node]() {
                        remove(s->slot_waiters, n);
                    }
                };
            }
        };

        U64   max_events = aio_ctx->max_ops;
        auto* handles    = default_construct_placement_array(arena::push_array_no_zero<std::coroutine_handle<>>(arena, max_events), max_events);
        auto* tokens     = arena::push_array_no_zero<U64>(arena, max_events);
        auto* state      = new (arena::push_array_no_zero<State>(arena, 1)) State{aio_ctx, handles};

        os::poll_unblock_on(poll, aio_ctx->notifier);

        auto read_fn = FileReadFunctor{[state](os::Handle f, Rng1U64 rng, void* out) -> coroutine::Task<> {
            U64 slot;
            while ((slot = os::aio_submit_read(*state->aio_ctx, f, rng, out)) == os::INVALID_AIO_SLOT) {
                co_await state->wait_for_slot();
            }
            co_await coroutine::Awaitable{
                [state, slot](std::coroutine_handle<> h) { state->handles[slot] = h; },
                []() {},
                [state, slot]() {
                    state->handles[slot] = {};
                }
            };
        }};

        auto write_fn = FileWriteFunctor{[state](os::Handle f, Rng1U64 rng, const void* in) -> coroutine::Task<> {
            U64 slot;
            while ((slot = os::aio_submit_write(*state->aio_ctx, f, rng, in)) == os::INVALID_AIO_SLOT) {
                co_await state->wait_for_slot();
            }
            co_await coroutine::Awaitable{
                [state, slot](std::coroutine_handle<> h) { state->handles[slot] = h; },
                []() {},
                [state, slot]() {
                    state->handles[slot] = {};
                }
            };
        }};

        auto sync_fn = FileSyncFunctor{[state](os::Handle f) -> coroutine::Task<> {
            U64 slot;
            while ((slot = os::aio_submit_sync(*state->aio_ctx, f)) == os::INVALID_AIO_SLOT) {
                co_await state->wait_for_slot();
            }
            co_await coroutine::Awaitable{
                [state, slot](std::coroutine_handle<> h) { state->handles[slot] = h; },
                []() {},
                [state, slot]() {
                    state->handles[slot] = {};
                }
            };
        }};

        EventConsumer consumer{
            .max_events = max_events,
            .on_unblock = OnUnblockFunctor{[state, tokens]([[maybe_unused]] const TArrayView<os::PollEvent>&) -> bool {
                os::aio_notifier_drain(state->aio_ctx->notifier);

                U32 n = os::aio_collect_completions(*state->aio_ctx, tokens, state->aio_ctx->max_ops);
                for (U32 i = 0; i < n; i++) {
                    U32  slot            = U32(tokens[i]);
                    auto h               = state->handles[slot];
                    state->handles[slot] = std::coroutine_handle<>{};
                    state->wake_one_waiter();
                    if (h) {
                        h.resume();
                    }
                }
                return true;
            }}
        };

        return {
            FileIOContext{plexdb::move(read_fn), plexdb::move(write_fn), plexdb::move(sync_fn)},
            plexdb::move(consumer)
        };
    }

    Pair<FileIOContext, EventConsumer> create_uring_async_file_io_context(uring::Ring* ring, arena::Arena& arena, os::Poll& poll) {
        assert_true(ring && static_cast<bool>(*ring) && !os::is_zero_handle(ring->event_fd), "invalid ring for async file io context");

        static constexpr U32 MAX_SYNC_SLOTS = 64;

        struct PendingOp {
            std::coroutine_handle<> handle;
            void*                   dst;
            U32                     bytes;
        };

        struct State {
            uring::Ring*                   ring;
            U64                            buf_size;
            U32                            buf_count;
            PendingOp*                     ops;        // arena[buf_count]
            bool*                          buf_in_use; // arena[buf_count]
            U32                            next_free_buf = 0;
            Deque<std::coroutine_handle<>> buf_waiters{};
            std::coroutine_handle<>        sync_handles[MAX_SYNC_SLOTS]{};
            U32                            next_sync_slot = 0;

            U32 try_acquire_buf() {
                U32 cur = next_free_buf;
                for (U32 i = 0; i < buf_count; ++i) {
                    if (!buf_in_use[cur]) {
                        buf_in_use[cur] = true;
                        next_free_buf   = (cur + 1) % buf_count;
                        return cur;
                    }
                    cur = (cur + 1) % buf_count;
                }
                return MAX_U32;
            }

            coroutine::Task<U32> acquire_buf() {
                while (true) {
                    U32 idx = try_acquire_buf();
                    if (idx != MAX_U32) {
                        co_return idx;
                    }
                    // Block scope ensures node outlives ~Awaitable (reverse construction order).
                    Deque<std::coroutine_handle<>>::Node node{};
                    co_await coroutine::Awaitable{
                        [s = this, n = &node](std::coroutine_handle<> h) { n->value = h; push_back(s->buf_waiters, n); },
                        []() {},
                        [s = this, n = &node]() {
                            remove(s->buf_waiters, n);
                        }
                    };
                }
            }

            void release_buf(U32 idx) {
                buf_in_use[idx] = false;
                if (buf_waiters.length > 0) {
                    auto h = *front(buf_waiters);
                    pop_front(buf_waiters);
                    h.resume();
                }
            }
        };

        os::poll_unblock_on(poll, ring->event_fd);

        U32 buf_count = ring->buffer_count;
        U64 buf_size  = ring->buffer_size;

        auto* ops    = arena::push_array<PendingOp>(arena, buf_count);
        auto* in_use = arena::push_array<bool>(arena, buf_count);
        auto* state  = new (arena::push_array_no_zero<State>(arena, 1)) State{ring, buf_size, buf_count, ops, in_use};

        auto read_fn = FileReadFunctor{[state](os::Handle file, Rng1U64 rng, void* dst) -> coroutine::Task<> {
            assert_true(rng.end - rng.start <= state->buf_size, "file read larger than ring buffer");
            U32 buf_idx = co_await state->acquire_buf();

            auto& op = state->ops[buf_idx];
            op.dst   = dst;
            op.bytes = 0;

            bool pushed = uring::sqe_push_file_read(*state->ring, file, buf_idx, rng.start, U32(rng.end - rng.start), U64(buf_idx));
            assert_true_always(pushed, "failed to submit io_uring file read SQE");
            co_await coroutine::Awaitable{
                [s = state, buf_idx](std::coroutine_handle<> h) { s->ops[buf_idx].handle = h; },
                []() {},
                [s = state, buf_idx]() {
                    s->ops[buf_idx].handle = {};
                }
            };

            if (op.dst && op.bytes > 0) {
                os::memory_copy(op.dst, state->ring->buffers + U64(buf_idx) * state->buf_size, op.bytes);
            }
            state->release_buf(buf_idx);
        }};

        auto write_one_buffer = [state](os::Handle file, Rng1U64 rng, const void* src) -> coroutine::Task<> {
            U32 buf_idx = co_await state->acquire_buf();

            U32 count = U32(rng.end - rng.start);
            os::memory_copy(state->ring->buffers + U64(buf_idx) * state->buf_size, src, count);

            auto& op = state->ops[buf_idx];
            op.dst   = nullptr;
            op.bytes = 0;

            bool pushed = uring::sqe_push_file_write(*state->ring, file, buf_idx, rng.start, count, U64(buf_idx));
            assert_true_always(pushed, "failed to submit io_uring file write SQE");
            co_await coroutine::Awaitable{
                [s = state, buf_idx](std::coroutine_handle<> h) { s->ops[buf_idx].handle = h; },
                []() {},
                [s = state, buf_idx]() {
                    s->ops[buf_idx].handle = {};
                }
            };

            state->release_buf(buf_idx);
        };

        auto write_fn = FileWriteFunctor{[state, write_one_buffer](os::Handle file, Rng1U64 rng, const void* src) -> coroutine::Task<> {
            U64 total = rng.end - rng.start;
            if (total <= state->buf_size) {
                co_await write_one_buffer(file, rng, src);
                co_return;
            }

            struct Chunk {
                Rng1U64     rng;
                const void* src;
            };
            U64 chunk_count = (total + state->buf_size - 1) / state->buf_size;

            threads::Scope    sc            = threads::scratch();
            Chunk*            chunk_storage = arena::push_array_no_zero<Chunk>(*sc.arena, chunk_count);
            TArrayView<Chunk> chunks{chunk_storage, chunk_count};

            for (U64 i = 0; i < chunk_count; i++) {
                U64 start = rng.start + i * state->buf_size;
                U64 end   = min(start + state->buf_size, rng.end);
                chunks[i] = Chunk{
                    Rng1U64{.start = start, .end = end},
                    static_cast<const U8*>(src) + (start - rng.start)
                };
            }

            auto write_chunk = [file, &write_one_buffer](const Chunk& c) -> coroutine::Task<> {
                co_await write_one_buffer(file, c.rng, c.src);
            };

            co_await coroutine::when_all(chunks, write_chunk);
        }};

        auto sync_fn = FileSyncFunctor{[state](os::Handle file) -> coroutine::Task<> {
            U32  sync_idx = state->next_sync_slot++ % MAX_SYNC_SLOTS;
            bool pushed   = uring::sqe_push_file_sync(*state->ring, file, U64(sync_idx));
            assert_true_always(pushed, "failed to submit io_uring file sync SQE");
            co_await coroutine::Awaitable{
                [s = state, sync_idx](std::coroutine_handle<> h) { s->sync_handles[sync_idx] = h; },
                []() {},
                [s = state, sync_idx]() {
                    s->sync_handles[sync_idx] = {};
                }
            };
        }};

        EventConsumer consumer{
            .max_events = U64(buf_count) + MAX_SYNC_SLOTS,
            .on_unblock = OnUnblockFunctor{[state]([[maybe_unused]] const TArrayView<os::PollEvent>&) -> bool {
                uring::sqe_submit_non_blocking(*state->ring);

                bool has_new_events = uring::ring_drain_event_fd(*state->ring);
                if (!has_new_events) {
                    return true;
                }

                while (uring::cqe_get_size(*state->ring) > 0) {
                    auto cqe = uring::cqe_top(*state->ring);
                    uring::cqe_pop(*state->ring, 1);

                    visit(cqe, [state](const auto& ev) {
                        using T = RemoveCVRef<decltype(ev)>;
                        if constexpr (SameAs<T, uring::FileReadEvent>) {
                            auto& op  = state->ops[ev.buffer_idx];
                            op.bytes  = ev.bytes_read;
                            auto h    = op.handle;
                            op.handle = {};
                            // h == {} if the coroutine was destroyed in-flight; release the buffer instead
                            if (h) {
                                h.resume();
                            } else {
                                state->release_buf(ev.buffer_idx);
                            }
                        } else if constexpr (SameAs<T, uring::FileWriteEvent>) {
                            auto& op  = state->ops[ev.buffer_idx];
                            op.bytes  = ev.bytes_written;
                            auto h    = op.handle;
                            op.handle = {};
                            if (h) {
                                h.resume();
                            } else {
                                state->release_buf(ev.buffer_idx);
                            }
                        } else if constexpr (SameAs<T, uring::FileSyncEvent>) {
                            U32  idx                 = U32(ev.token) % MAX_SYNC_SLOTS;
                            auto h                   = state->sync_handles[idx];
                            state->sync_handles[idx] = {};
                            if (h) {
                                h.resume();
                            }
                        }
                    });
                }

                uring::sqe_submit_non_blocking(*state->ring);

                return true;
            }}
        };

        return {
            FileIOContext{plexdb::move(read_fn), plexdb::move(write_fn), plexdb::move(sync_fn)},
            plexdb::move(consumer)
        };
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
            FileSyncFunctor{[](os::Handle f) -> coroutine::Task<> {
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
