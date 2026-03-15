module;
#include "macros.h"
#include <coroutine>

export module objstore.tcp.coro;

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.coro;
import plexdb.tagged_union;
import objstore.tcp.types;

using namespace plexdb;

// ============================================================================
// Coroutine-based TCP connection handler
//
// Design:
//   The caller provides one coroutine callback per accepted connection.
//   The callback receives a CoroConnectionIO & that exposes awaitable
//   recv/send primitives backed by the same io_uring ring used for accepts.
//
//   Usage:
//
//       auto handler = [](tcp::CoroConnectionIO& io) -> coro::Task {
//           while (true) {
//               auto chunk = co_await io.recv();
//               if (!chunk.valid()) co_return;
//               io.send(chunk.buffer_idx, 0, response_size);
//               io.release_recv(chunk.buffer_idx);
//           }
//       };
//       tcp::listen_coro(ring, handler, signal, exit_flag);
//
// Buffer ownership:
//   recv() acquires a buffer before suspending.  The caller must either:
//     a) call io.release_recv(chunk.buffer_idx) when done with the data, or
//     b) pass the buffer_idx to io.send() which releases it after the write.
//
// Lifetime:
//   CoroConnectionIO lives inside a fixed-size table entry.  The table
//   entry (and the coroutine frame) are destroyed only after the task
//   completes.  Pending io_uring SQEs must complete (or be cancelled) before
//   the entry is removed; see the close_conn() function in listen_coro_impl.
// ============================================================================
namespace objstore::tcp {
    // ========================================================================
    // Internal helpers (not exported)
    // ========================================================================
    constexpr U32 CORO_INVALID_BUFFER_IDX = MAX_U32;

    // Same logic as get_next_free_buffer_idx in tcp.detail, kept local to
    // avoid depending on that module.
    static U32 coro_get_free_buffer(
        U32& next, const DynamicArray<BufferInfo>& infos, U32 count) noexcept
    {
        U32 start = next;
        while (!os::is_zero_handle(infos[next].client)) {
            next = (next + 1) % count;
            if (next == start) return CORO_INVALID_BUFFER_IDX;
        }
        return next;
    }
}

export namespace objstore::tcp {
    // ========================================================================
    // RecvResult — data returned by co_await CoroConnectionIO::recv()
    // ========================================================================
    struct RecvResult {
        U8*  data       = nullptr;
        int  byte_count = 0;
        U32  buffer_idx = CORO_INVALID_BUFFER_IDX;

        bool valid() const noexcept { return byte_count > 0 && data != nullptr; }
    };

    // ========================================================================
    // CoroConnectionIO
    //   Stable per-connection object (stored in the event-loop table).
    //   Provides awaitable recv/send on the underlying io_uring Ring.
    // ========================================================================
    struct CoroConnectionIO {
        // ----------------------------------------------------------------
        // RecvAwaitable
        //   co_await io.recv() suspends the coroutine and submits a TYPE_CORO
        //   read SQE.  The event loop resumes the coroutine when the CQE
        //   arrives.
        // ----------------------------------------------------------------
        struct RecvAwaitable : coro::IoAwaitable {
            CoroConnectionIO* io         = nullptr;
            U32               buffer_idx = CORO_INVALID_BUFFER_IDX;
            int               bytes_read = 0;

            RecvAwaitable() : coro::IoAwaitable(recv_complete_fn) {}

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) noexcept {
                continuation = h;

                U32 idx = coro_get_free_buffer(
                    *io->next_free_buffer_idx, *io->buffer_infos,
                    io->ring->buffer_count);

                if (idx == CORO_INVALID_BUFFER_IDX) {
                    // No buffer — complete immediately as a disconnect.
                    bytes_read = 0;
                    h.resume();
                    return;
                }

                buffer_idx = idx;
                (*io->buffer_infos)[idx].client = io->connection->client;
                PLEXDB_DEBUG_X((*io->buffer_infos)[idx].buffer_idx = idx;)

                io->pending_recv = this;
                uring::sqe_push_coro_recv(
                    *io->ring, io->connection->client, buffer_idx, this);
            }

            RecvResult await_resume() noexcept {
                if (bytes_read <= 0 || buffer_idx == CORO_INVALID_BUFFER_IDX)
                    return {};
                return {
                    uring::get_buffer_ptr(*io->ring, buffer_idx),
                    bytes_read,
                    buffer_idx
                };
            }

        private:
            static void recv_complete_fn(coro::IoAwaitable* base, int result) noexcept {
                auto* self = static_cast<RecvAwaitable*>(base);
                self->bytes_read = result;
                // Clear pending_recv before resuming so that close_conn
                // cannot call complete() a second time after the coroutine
                // has already handled this completion.
                self->io->pending_recv = nullptr;
                if (base->continuation)
                    base->continuation.resume();
            }
        };

        // ----------------------------------------------------------------
        // SendAwaitable
        //   co_await io.send_async(...) suspends the coroutine and submits a
        //   TYPE_CORO write SQE.  The buffer is released when the write
        //   completes.
        // ----------------------------------------------------------------
        struct SendAwaitable : coro::IoAwaitable {
            CoroConnectionIO* io         = nullptr;
            U32               buffer_idx = CORO_INVALID_BUFFER_IDX;
            U32               offset     = 0;
            U32               byte_count = 0;

            SendAwaitable() : coro::IoAwaitable(send_complete_fn) {}

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) noexcept {
                continuation = h;
                uring::sqe_push_coro_send(
                    *io->ring, io->connection->client,
                    buffer_idx, offset, byte_count, this);
            }

        private:
            static void send_complete_fn(coro::IoAwaitable* base, int result) noexcept {
                auto* self = static_cast<SendAwaitable*>(base);
                if (self->buffer_idx != CORO_INVALID_BUFFER_IDX)
                    (*self->io->buffer_infos)[self->buffer_idx].client = os::zero_handle();
                coro::IoAwaitable::default_complete(base, result);
            }
        };

        // ----------------------------------------------------------------
        // Public data
        // ----------------------------------------------------------------
        Connection*               connection           = nullptr;
        uring::Ring*              ring                 = nullptr;
        DynamicArray<BufferInfo>* buffer_infos         = nullptr;
        U32*                      next_free_buffer_idx = nullptr;
        // Set while the coroutine is suspended in a recv() awaitable.
        RecvAwaitable*            pending_recv         = nullptr;

        // ----------------------------------------------------------------
        // API
        // ----------------------------------------------------------------

        // co_await this to receive the next chunk.
        RecvAwaitable recv() noexcept {
            RecvAwaitable aw;
            aw.io = this;
            return aw;
        }

        // Fire-and-forget send.  The buffer is released on write completion.
        void send(U32 buffer_idx, U32 offset, U32 byte_count) noexcept {
            uring::sqe_push_write(*ring, connection->client, buffer_idx, offset, byte_count);
        }

        // co_await this to send and be notified when the write is done.
        SendAwaitable send_async(U32 buffer_idx, U32 offset, U32 byte_count) noexcept {
            SendAwaitable aw;
            aw.io         = this;
            aw.buffer_idx = buffer_idx;
            aw.offset     = offset;
            aw.byte_count = byte_count;
            return aw;
        }

        // Release a recv buffer without sending.
        void release_recv(U32 buffer_idx) noexcept {
            if (buffer_idx != CORO_INVALID_BUFFER_IDX)
                (*buffer_infos)[buffer_idx].client = os::zero_handle();
        }
    };

    // ========================================================================
    // OnConnectionCoro concept
    // ========================================================================
    template<typename F>
    concept OnConnectionCoro = requires(F f, CoroConnectionIO& io) {
        { f(io) } -> SameAs<coro::Task>;
    };

    // ========================================================================
    // listen_coro
    //   Event loop that drives connection coroutines via io_uring.
    //   Replaces the on_chunk / on_open / on_close triple with a single
    //   coroutine per connection.
    //
    //   @note The existing io_uring ring setup and SQE/CQE machinery is
    //         preserved.  Only the dispatch layer changes: CoroEvent CQEs
    //         are forwarded directly to the waiting IoAwaitable.
    //
    //   @note Maximum in-flight IO is implicitly capped by
    //         MAX_CONCURRENT_CONNECTIONS (one recv per connection).
    // ========================================================================
    Stats listen_coro(
        uring::Ring& ring,
        const OnConnectionCoro auto& on_connection,
        const os::Notifier& interrupt,
        volatile bool& should_exit
    ) {
        Stats stats;

        // Per-connection state: Connection + CoroConnectionIO + coroutine Task.
        // @note MapFixedSentinel is fixed-size, so entries do not move —
        //       safe to hold pointers into them.
        struct Entry {
            Connection       connection;
            CoroConnectionIO coro_io;
            coro::Task       task;
        };

        MapFixedSentinel<os::Handle, Entry, 2_u64 * MAX_CONCURRENT_CONNECTIONS> entries;
        DynamicArray<BufferInfo> buffer_infos{ring.buffer_count};
        U32 next_free_buffer_idx = 0;

        // Handles of tasks that have finished and need to be closed.
        CappedArray<os::Handle, MAX_CONCURRENT_CONNECTIONS> finished;

        // ----------------------------------------------------------------
        // close_conn: wake any pending recv so the coroutine can exit, then
        //             release buffers and issue a close SQE.
        // ----------------------------------------------------------------
        auto close_conn = [&](os::Handle client, bool server_side = true) {
            auto it = find_it(entries, client);
            if (it == entries.end()) return;
            auto& entry = (*it).second;

            // Unblock a suspended recv coroutine (byte_count = 0 → disconnected).
            // @note Only wake the coroutine if it is still running.  The
            //       recv_complete_fn clears pending_recv before resuming, so a
            //       non-null pending_recv here means the coroutine is still
            //       suspended and waiting for IO.
            if (entry.coro_io.pending_recv && !entry.task.done()) {
                auto* aw = entry.coro_io.pending_recv;
                entry.coro_io.pending_recv = nullptr;
                aw->complete(0);
                // The coroutine now runs to completion or next yield.
            }

            // Release any buffers still owned by this connection.
            for (auto& info : buffer_infos) {
                if (info.client == client)
                    info.client = os::zero_handle();
            }

            remove_it(entries, it);
            stats.active_connections--;

            if (server_side)
                uring::sqe_push_close(ring, client);
        };

        // ----------------------------------------------------------------
        // handle_coro: resume the IoAwaitable that was awaiting this IO
        // ----------------------------------------------------------------
        auto handle_coro = [&](const uring::CoroEvent& ev) {
            // @note awaitable lives in the coroutine frame.  If the connection
            //       was closed before this CQE arrived the pointer is dangling.
            //       This is benign in the normal path because close_conn only
            //       removes the entry after either:
            //         a) the coroutine finishes naturally (task.done()), or
            //         b) the coroutine is woken via pending_recv->complete(0)
            //       In case (b) the pending SQE will still produce a CQE with
            //       a stale awaitable pointer.
            // @todo Mitigate by calling io_uring_prep_cancel on the pending
            //       SQE before destroying the Task, then draining the cancel
            //       completion before freeing the entry.
            static_cast<coro::IoAwaitable*>(ev.awaitable_ptr)->complete(ev.result);
        };

        // ----------------------------------------------------------------
        // handle_write: fire-and-forget write completion (releases buffer)
        // ----------------------------------------------------------------
        auto handle_write = [&](const uring::WriteEvent& write) {
            buffer_infos[write.buffer_idx].client = os::zero_handle();
            stats.total_bytes_written += static_cast<U64>(max(write.bytes_written, 0));
        };

        // ----------------------------------------------------------------
        // handle_accept: create the per-connection coroutine
        // ----------------------------------------------------------------
        auto handle_accept = [&](const auto& accept) {
            using T = RemoveCVRef<decltype(accept)>;
            static_assert(Either<T, uring::AcceptEvent, uring::MultishotAcceptEvent>);

            if constexpr (!SameAs<T, uring::MultishotAcceptEvent>)
                uring::sqe_push_accept(ring);

            socket_set_option(accept.client, os::SocketOption::NoDelay, true);

            Entry& entry = find_or_insert(entries, accept.client);
            entry.connection.client = accept.client;
            stats.total_connections++;
            stats.active_connections++;

            entry.coro_io.connection           = &entry.connection;
            entry.coro_io.ring                 = &ring;
            entry.coro_io.buffer_infos         = &buffer_infos;
            entry.coro_io.next_free_buffer_idx = &next_free_buffer_idx;
            entry.coro_io.pending_recv         = nullptr;

            // Start the coroutine; it runs until the first co_await recv(),
            // which submits the TYPE_CORO read SQE internally.
            entry.task = on_connection(entry.coro_io);
            entry.task.start();

            if (entry.task.done())
                push_back(finished, accept.client);
        };

        // ----------------------------------------------------------------
        // handle_close
        // ----------------------------------------------------------------
        auto handle_close = [&](const uring::CloseEvent& close) {
            close_conn(close.client, /*server_side=*/false);
        };

        // ----------------------------------------------------------------
        // Event loop
        // ----------------------------------------------------------------
        os::Poll poll{};
        poll_unblock_on(poll, interrupt);
        poll_unblock_on(poll, ring.server);

        uring::sqe_push_multishot_accept(ring);

        while (!should_exit) {
            uring::sqe_submit_non_blocking(ring);

            // Close connections whose tasks finished in the previous iteration.
            for (U64 i = 0; i < finished.cap; i++)
                close_conn(finished[i]);
            finished.cap = 0;

            U32 count     = uring::cqe_get_size(ring);
            U32 processed = 0;

            for (U32 i = 0; i < count; i++) {
                auto cqe = uring::cqe_top(ring);
                uring::cqe_pop(ring, 1);
                if (!static_cast<bool>(cqe)) continue;
                processed++;

                visit(cqe, [&](const auto& ev) {
                    using T = RemoveCVRef<decltype(ev)>;
                    if constexpr (SameAs<T, uring::ReadEvent>)
                        {} // unused in coro path (reads go via TYPE_CORO)
                    else if constexpr (SameAs<T, uring::WriteEvent>)
                        handle_write(ev);
                    else if constexpr (Either<T, uring::AcceptEvent, uring::MultishotAcceptEvent>)
                        handle_accept(ev);
                    else if constexpr (SameAs<T, uring::CloseEvent>)
                        handle_close(ev);
                    else if constexpr (SameAs<T, uring::CoroEvent>)
                        handle_coro(ev);
                    else
                        static_assert(!SameAs<T, T>);
                });
            }

            // Mark any tasks that completed during this CQE processing cycle.
            for (auto& kv : entries) {
                if (kv.second.task.done()) {
                    bool dup = false;
                    for (U64 i = 0; i < finished.cap; i++)
                        if (finished[i] == kv.first) { dup = true; break; }
                    if (!dup) push_back(finished, kv.first);
                }
            }

            if (processed == 0)
                block_until_poll_unblocks(poll);
        }

        // Close all remaining connections.
        for (auto& kv : entries)
            uring::sqe_push_close(ring, kv.second.connection.client);
        uring::sqe_submit_non_blocking(ring);

        return stats;
    }

} // export namespace objstore::tcp
