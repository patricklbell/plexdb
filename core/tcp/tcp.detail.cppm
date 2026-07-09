module;
#include <plexdb/macros/macros.h>
#include <coroutine>

export module plexdb.tcp.detail;

import plexdb.os;
import plexdb.os.uring;
import plexdb.aio;
import plexdb.base;
import plexdb.arena;
import plexdb.coroutine;
import plexdb.tcp.types;

using namespace plexdb;

namespace plexdb::tcp {
    // @note fat struct, not always meaningful depending on the handler
    struct BufferInfo {
        PLEXDB_DEBUG_X(U32 buffer_idx = -1;)
        os::Handle client = os::zero_handle();
    };
}

export namespace plexdb::tcp {
    struct UringListenerState {
        uring::Ring                                                                                     ring;
        Stats                                                                                           stats;
        MapFixedSentinel<os::Handle, Connection, 2_u64 * MAX_CONCURRENT_CONNECTIONS, os::zero_handle()> client_to_connection;
        DynamicArray<BufferInfo>                                                                        buffer_infos;
        uring::BufferPool<MAX_CONCURRENT_CONNECTIONS>                                                   buffer_pool;
        AcquireBufferFunctor                                                                            acquire_fn;
        ReleaseBufferFunctor                                                                            release_fn;
        ReadToBufferFunctor                                                                             read_fn;
        WriteFromBufferFunctor                                                                          write_fn;

        UringListenerState(uring::Ring r)
            : ring(move(r))
            , buffer_infos(ring.buffer_count)
            , buffer_pool(ring.buffer_count) {
        }

        UringListenerState(UringListenerState&&) = default;

        ~UringListenerState() {
            if (ring) {
                for (auto& it : client_to_connection) {
                    Connection& conn = it.second;
                    if (conn.waiting_rwc) {
                        conn.count_rwc            = 0;
                        conn.error_rwc            = Error::ConnectionClosed;
                        std::coroutine_handle<> h = conn.waiting_rwc;
                        conn.waiting_rwc          = std::coroutine_handle<>{};
                        h.resume();
                    }
                    conn.task.reset();
                    uring::sqe_push_close(ring, it.second.client);
                }
                uring::sqe_submit_non_blocking(ring);
            }
        }
    };

    struct SocketListenerState {
        static constexpr U32               BUFFER_SIZE  = 64_kb;
        static constexpr U32               BUFFER_COUNT = MAX_CONCURRENT_CONNECTIONS;
        static constexpr os::PollEventMask DEFAULT_EVENTS =
            os::PollEventMask::Read | os::PollEventMask::Error | os::PollEventMask::HangUp;
        static constexpr os::PollEventMask WRITE_EVENTS =
            DEFAULT_EVENTS | os::PollEventMask::Write;

        enum Op : U8 {
            None      = 0,
            WaitWrite = 1,
            WaitRead  = 2,
        };

        os::Handle                                                                   socket;
        Stats                                                                        stats;
        os::Poll*                                                                    poll;
        MapFixedSentinel<os::Handle, Connection, 2_u64 * MAX_CONCURRENT_CONNECTIONS> client_to_connection;
        MapFixedSentinel<os::Handle, Op, 2_u64 * MAX_CONCURRENT_CONNECTIONS>         waiting_op;
        DynamicArray<BufferInfo>                                                     buffer_infos;
        uring::BufferPool<MAX_CONCURRENT_CONNECTIONS>                                buffer_pool;
        UniquePtr<U8>                                                                socket_buffers{};
        AcquireBufferFunctor                                                         acquire_fn;
        ReleaseBufferFunctor                                                         release_fn;
        ReadToBufferFunctor                                                          read_fn;
        WriteFromBufferFunctor                                                       write_fn;

        SocketListenerState(os::Handle s, os::Poll* p)
            : socket(s)
            , poll(p)
            , buffer_infos{BUFFER_COUNT}
            , buffer_pool{BUFFER_COUNT}
            , socket_buffers{os::allocate(U64(BUFFER_SIZE) * BUFFER_COUNT)} {
        }

        SocketListenerState(SocketListenerState&& o) noexcept
            : socket(o.socket)
            , stats(move(o.stats))
            , poll(o.poll)
            , client_to_connection(move(o.client_to_connection))
            , waiting_op(move(o.waiting_op))
            , buffer_infos(move(o.buffer_infos))
            , buffer_pool(move(o.buffer_pool))
            , socket_buffers(plexdb::move(o.socket_buffers))
            , acquire_fn(move(o.acquire_fn))
            , release_fn(move(o.release_fn))
            , read_fn(move(o.read_fn))
            , write_fn(move(o.write_fn)) {
        }

        ~SocketListenerState() {
            for (auto& it : client_to_connection) {
                Connection& conn = it.second;
                if (!os::is_zero_handle(conn.client)) {
                    os::poll_dont_unblock_on(*poll, conn.client);
                    os::socket_close(conn.client);
                    conn.client = os::zero_handle();
                }
                if (conn.waiting_rwc) {
                    conn.error_rwc            = Error::ConnectionClosed;
                    std::coroutine_handle<> h = conn.waiting_rwc;
                    conn.waiting_rwc          = std::coroutine_handle<>{};
                    h.resume();
                }
                conn.task.reset();
            }
        }
    };

    aio::EventConsumer listen_uring_start(
        UringListenerState*           in_s,
        const ConnectionHandler auto* in_handler,
        os::Poll&                     poll
    ) {
        assert_true(static_cast<bool>(in_s->ring), "cannot listen to invalid uring");
        assert_true(!os::is_zero_handle(in_s->ring.event_fd), "uring ring missing event_fd");

        in_s->acquire_fn = AcquireBufferFunctor{[in_s](Connection* connection) -> coroutine::Task<RWBuffer> {
            U32 idx                        = co_await in_s->buffer_pool.acquire();
            in_s->buffer_infos[idx].client = connection->client;
            PLEXDB_DEBUG_X(in_s->buffer_infos[idx].buffer_idx = idx;)
            co_return RWBuffer{
                .view   = TArrayView<U8, U32>(in_s->ring.buffers + U64(idx) * in_s->ring.buffer_size, 0),
                .length = in_s->ring.buffer_size,
                .idx    = idx,
            };
        }};

        in_s->release_fn = ReleaseBufferFunctor{[in_s]([[maybe_unused]] Connection*, const RWBuffer* buffer) {
            in_s->buffer_infos[buffer->idx].client = os::zero_handle();
            in_s->buffer_pool.release(bounds_checked_cast<U32>(buffer->idx));
        }};

        in_s->read_fn = ReadToBufferFunctor{[in_s](Connection* connection, RWBuffer* buffer) -> coroutine::Task<Error> {
            assert_true(!os::is_zero_handle(connection->client), "invalid read on closed connection");
            assert_true(!connection->waiting_rwc, "connection already has coroutine pending");
            assert_true(in_s->buffer_infos[buffer->idx].client == connection->client, "buffer not acquired for this connection");

            U32 byte_offset    = bounds_checked_cast<U32>(buffer->view.ptr - (in_s->ring.buffers + buffer->idx * in_s->ring.buffer_size));
            U32 max_byte_count = bounds_checked_cast<U32>(buffer->length);

            co_return co_await coroutine::Awaitable{
                [in_s, connection, buffer, byte_offset, max_byte_count](std::coroutine_handle<> h) {
                    connection->waiting_rwc = h;
                    bool pushed             = uring::sqe_push_read(in_s->ring, connection->client, bounds_checked_cast<U32>(buffer->idx), byte_offset, max_byte_count);
                    assert_true_always(pushed, "failed to submit io_uring read SQE");
                },
                [connection, buffer]() -> Error {
                    buffer->view.length     = connection->count_rwc;
                    connection->waiting_rwc = std::coroutine_handle<>{};
                    return connection->error_rwc;
                }
            };
        }};

        in_s->write_fn = WriteFromBufferFunctor{[in_s](Connection* connection, const RWBuffer* buffer) -> coroutine::Task<Error> {
            assert_true(!os::is_zero_handle(connection->client), "invalid write on closed connection");
            assert_true(!connection->waiting_rwc, "connection already has coroutine pending");

            U32 byte_offset       = bounds_checked_cast<U32>(buffer->view.ptr - (in_s->ring.buffers + buffer->idx * in_s->ring.buffer_size));
            U32 target_byte_count = bounds_checked_cast<U32>(buffer->view.length);

            co_return co_await coroutine::Awaitable{
                [in_s, connection, buffer, byte_offset, target_byte_count](std::coroutine_handle<> h) {
                    connection->waiting_rwc = h;
                    bool pushed             = uring::sqe_push_write(in_s->ring, connection->client, bounds_checked_cast<U32>(buffer->idx), byte_offset, target_byte_count);
                    assert_true_always(pushed, "failed to submit io_uring write SQE");
                },
                [connection, target_byte_count]() -> Error {
                    connection->waiting_rwc = std::coroutine_handle<>{};
                    assert_true(connection->error_rwc != Error::None || connection->count_rwc == target_byte_count, "unexpected partial write resumption");
                    return connection->error_rwc;
                }
            };
        }};

        assert_true_always(uring::sqe_push_multishot_accept(in_s->ring), "failed to submit io_uring multishot accept SQE");
        uring::sqe_submit_non_blocking(in_s->ring);
        os::poll_unblock_on(poll, in_s->ring.event_fd);

        return aio::EventConsumer{
            .max_events = U64(in_s->ring.buffer_count) + 2,
            .on_unblock = aio::OnUnblockFunctor{
                [in_s, in_handler]([[maybe_unused]] const TArrayView<os::PollEvent>& events) mutable -> bool {
                    auto close_and_cleanup = [in_s](os::Handle client, bool is_in_close_handler) {
                        auto it = find_it(in_s->client_to_connection, client);
                        if (it == in_s->client_to_connection.end()) {
                            return false;
                        }

                        Connection& connection = it->second;
                        connection.client      = os::zero_handle();
                        in_s->stats.active_connections--;

                        if (!is_in_close_handler) {
                            assert_true_always(uring::sqe_push_close(in_s->ring, client), "failed to submit io_uring close SQE");
                        }

                        connection.task.reset();
                        remove_it(in_s->client_to_connection, it);
                        return true;
                    };

                    // @note a coroutine resumed via another ring's completion can push an SQE
                    // here without this ring's eventfd firing; submit unconditionally, before
                    // checking for new events, or that push never reaches the kernel.
                    bool has_new_events = uring::ring_drain_event_fd(in_s->ring);
                    uring::sqe_submit_non_blocking(in_s->ring);
                    if (!has_new_events) {
                        return true;
                    }

                    while (uring::cqe_get_size(in_s->ring) > 0) {
                        auto cqe = uring::cqe_top(in_s->ring);
                        uring::cqe_pop(in_s->ring, 1);

                        visit(cqe, [&](const auto& ev) {
                            using T = RemoveCVRef<decltype(ev)>;
                            if constexpr (SameAs<T, uring::ReadEvent>) {
                                Connection* conn;
                                {
                                    // @note info is only valid up to this lookup: resuming the
                                    // connection's coroutine below may release the buffer, which
                                    // clears BufferInfo::client.
                                    BufferInfo& info = in_s->buffer_infos[ev.buffer_idx];
                                    conn             = find(in_s->client_to_connection, info.client);
                                }
                                assert_true(conn != nullptr, "connection dropped before read completion");
                                in_s->stats.total_bytes_read += ev.bytes_read;
                                conn->error_rwc = ev.error != uring::Error::None ? Error::Other
                                                : ev.bytes_read == 0             ? Error::ConnectionClosed
                                                                                 : Error::None;
                                conn->count_rwc = ev.bytes_read;
                                conn->waiting_rwc.resume();
                                if (static_cast<bool>(conn->task) && conn->task->done()) {
                                    close_and_cleanup(conn->client, /*is_in_close_handler=*/false);
                                }
                            } else if constexpr (SameAs<T, uring::WriteEvent>) {
                                Connection* conn;
                                {
                                    // @note scope, see above
                                    BufferInfo& info = in_s->buffer_infos[ev.buffer_idx];
                                    conn             = find(in_s->client_to_connection, info.client);
                                }
                                assert_true(conn != nullptr, "connection dropped before write completion");
                                in_s->stats.total_bytes_written += ev.bytes_written;
                                conn->error_rwc = ev.error != uring::Error::None ? Error::Other : Error::None;
                                conn->count_rwc = ev.bytes_written;
                                conn->waiting_rwc.resume();
                                if (static_cast<bool>(conn->task) && conn->task->done()) {
                                    close_and_cleanup(conn->client, /*is_in_close_handler=*/false);
                                }
                            } else if constexpr (Either<T, uring::AcceptEvent, uring::MultishotAcceptEvent>) {
                                if constexpr (!SameAs<T, uring::MultishotAcceptEvent>) {
                                    assert_true_always(uring::sqe_push_accept(in_s->ring), "failed to submit io_uring accept SQE");
                                }
                                // @note a failed accept only drops this one connection attempt;
                                // the listener keeps accepting others.
                                if (ev.error != uring::Error::None) {
                                    return;
                                }
                                os::socket_set_option(ev.client, os::SocketOption::NoDelay, true);
                                Connection& conn = find_or_insert(in_s->client_to_connection, ev.client);
                                conn.client      = ev.client;
                                in_s->stats.total_connections++;
                                in_s->stats.active_connections++;
                                conn.req = Request{
                                    .connection = &conn,
                                    .acquire    = &in_s->acquire_fn,
                                    .release    = &in_s->release_fn,
                                    .read       = &in_s->read_fn,
                                    .write      = &in_s->write_fn,
                                };
                                conn.task = (*in_handler)(conn.req);
                            } else if constexpr (SameAs<T, uring::CloseEvent>) {
                                // @note intentionally ignores a close error, the connection is gone anyways
                                close_and_cleanup(ev.client, /*is_in_close_handler*/ true);
                            }
                        });
                    }

                    uring::sqe_submit_non_blocking(in_s->ring);
                    return true;
                }
            }
        };
    }

    aio::EventConsumer listen_socket_start(
        SocketListenerState*          in_s,
        const ConnectionHandler auto* in_handler,
        os::Poll&                     poll
    ) {
        os::socket_set_option(in_s->socket, os::SocketOption::NonBlocking, true);

        constexpr U32 BUFFER_SIZE = SocketListenerState::BUFFER_SIZE;

        in_s->acquire_fn = AcquireBufferFunctor{[in_s](Connection* conn) -> coroutine::Task<RWBuffer> {
            U32 idx                        = co_await in_s->buffer_pool.acquire();
            in_s->buffer_infos[idx].client = conn->client;
            PLEXDB_DEBUG_X(in_s->buffer_infos[idx].buffer_idx = idx;)
            co_return RWBuffer{
                .view   = TArrayView<U8, U32>(in_s->socket_buffers.ptr + U64(idx) * BUFFER_SIZE, 0),
                .length = BUFFER_SIZE,
                .idx    = idx,
            };
        }};

        in_s->release_fn = ReleaseBufferFunctor{[in_s]([[maybe_unused]] Connection*, const RWBuffer* buffer) {
            in_s->buffer_infos[buffer->idx].client = os::zero_handle();
            in_s->buffer_pool.release(bounds_checked_cast<U32>(buffer->idx));
        }};

        in_s->read_fn = ReadToBufferFunctor{[in_s](Connection* conn, RWBuffer* buffer) -> coroutine::Task<Error> {
            assert_true(!os::is_zero_handle(conn->client), "invalid read on closed connection");
            assert_true(!conn->waiting_rwc, "connection already has coroutine pending");

            const U32 max_byte_count = bounds_checked_cast<U32>(buffer->length);
            while (true) {
                auto result = os::socket_receive(conn->client, buffer->view.ptr, max_byte_count);
                if (result.byte_count > 0 && result.error == os::SocketError::None) {
                    buffer->view.length = result.byte_count;
                    in_s->stats.total_bytes_read += result.byte_count;
                    co_return Error::None;
                }

                buffer->length = 0;
                switch (result.error) {
                    case os::SocketError::None:
                        co_return Error::ConnectionClosed;
                    case os::SocketError::WouldBlock: {
                        bool ready = co_await coroutine::Awaitable{
                            [in_s, conn](std::coroutine_handle<> h) {
                                conn->waiting_rwc                              = h;
                                find_or_insert(in_s->waiting_op, conn->client) = SocketListenerState::WaitRead;
                            },
                            [conn]() -> bool {
                                return !os::is_zero_handle(conn->client);
                            }
                        };
                        if (!ready) {
                            co_return Error::ConnectionClosed;
                        }
                        continue;
                    }
                    case os::SocketError::Timeout:
                    case os::SocketError::ConnectionClosed:
                    case os::SocketError::ConnectionRefused:
                    case os::SocketError::ConnectionReset:
                        co_return Error::ConnectionClosed;
                    case os::SocketError::Other:
                        co_return Error::Other;
                }
                assert_true(false, "unhandled socket error");
                co_return Error::Other;
            }
        }};

        in_s->write_fn = WriteFromBufferFunctor{[in_s](Connection* conn, const RWBuffer* buffer) -> coroutine::Task<Error> {
            assert_true(!os::is_zero_handle(conn->client), "invalid write on closed connection");
            assert_true(!conn->waiting_rwc, "connection already has coroutine pending");

            U64 current_byte_count = 0;
            U64 target_byte_count  = buffer->view.length;
            while (current_byte_count < target_byte_count) {
                auto result = os::socket_send(conn->client, buffer->view.ptr + current_byte_count, target_byte_count - current_byte_count);
                if (result.byte_count > 0 && result.error == os::SocketError::None) {
                    current_byte_count += result.byte_count;
                    in_s->stats.total_bytes_written += result.byte_count;
                    continue;
                }

                switch (result.error) {
                    case os::SocketError::None:
                        assert_true(false, "zero-length write with no error");
                        break;
                    case os::SocketError::WouldBlock: {
                        bool ready = co_await coroutine::Awaitable{
                            [in_s, conn](std::coroutine_handle<> h) {
                                conn->waiting_rwc                              = h;
                                find_or_insert(in_s->waiting_op, conn->client) = SocketListenerState::Op::WaitWrite;
                                os::poll_update_mask(*in_s->poll, conn->client, SocketListenerState::WRITE_EVENTS);
                            },
                            [in_s, conn]() -> bool {
                                if (os::is_zero_handle(conn->client)) {
                                    return false;
                                }
                                os::poll_update_mask(*in_s->poll, conn->client, SocketListenerState::DEFAULT_EVENTS);
                                return true;
                            }
                        };
                        if (!ready) {
                            co_return Error::ConnectionClosed;
                        }
                        continue;
                    }
                    case os::SocketError::Timeout:
                    case os::SocketError::ConnectionClosed:
                    case os::SocketError::ConnectionRefused:
                    case os::SocketError::ConnectionReset:
                        co_return Error::ConnectionClosed;
                    case os::SocketError::Other:
                        co_return Error::Other;
                }
                assert_true(false, "unhandled socket error");
                co_return Error::Other;
            }

            assert_true(current_byte_count == target_byte_count, "sent more bytes over socket than expected");
            co_return Error::None;
        }};

        os::poll_unblock_on(poll, in_s->socket, SocketListenerState::DEFAULT_EVENTS);

        return aio::EventConsumer{
            .max_events = U64(MAX_CONCURRENT_CONNECTIONS) + 2,
            .on_unblock = aio::OnUnblockFunctor{
                [in_s, in_handler](const TArrayView<os::PollEvent>& events) mutable -> bool {
                    auto close_and_cleanup = [in_s](const os::Handle& client) {
                        auto it = find_it(in_s->client_to_connection, client);
                        if (it == in_s->client_to_connection.end()) {
                            return;
                        }

                        Connection& conn = it->second;
                        if (!os::is_zero_handle(conn.client)) {
                            os::poll_dont_unblock_on(*in_s->poll, conn.client);
                            os::socket_close(conn.client);
                            conn.client = os::zero_handle();
                        }

                        if (in_s->stats.active_connections > 0) {
                            in_s->stats.active_connections--;
                        }

                        auto w_it = find_it(in_s->waiting_op, client);
                        if (w_it != in_s->waiting_op.end()) {
                            remove_it(in_s->waiting_op, w_it);
                        }

                        if (conn.waiting_rwc) {
                            std::coroutine_handle<> h = conn.waiting_rwc;
                            conn.waiting_rwc          = std::coroutine_handle<>{};
                            h.resume();
                        }

                        conn.task.reset();
                        remove_it(in_s->client_to_connection, it);
                    };

                    for (const auto& event : events) {
                        bool has_read  = ((event.events) & os::PollEventMask::Read) != os::PollEventMask::None;
                        bool has_write = ((event.events) & os::PollEventMask::Write) != os::PollEventMask::None;
                        bool has_err   = ((event.events) & os::PollEventMask::Error) != os::PollEventMask::None;
                        bool has_hup   = ((event.events) & os::PollEventMask::HangUp) != os::PollEventMask::None;

                        if (event.handle == in_s->socket) {
                            while (true) {
                                os::Handle client = os::socket_accept(in_s->socket);
                                if (os::is_zero_handle(client)) {
                                    break;
                                }

                                if (in_s->stats.active_connections >= MAX_CONCURRENT_CONNECTIONS) {
                                    in_s->stats.dropped_connections++;
                                    os::socket_close(client);
                                    continue;
                                }

                                os::socket_set_option(client, os::SocketOption::NonBlocking, true);
                                os::socket_set_option(client, os::SocketOption::NoDelay, true);
                                os::poll_unblock_on(*in_s->poll, client, SocketListenerState::DEFAULT_EVENTS);

                                Connection& conn = find_or_insert(in_s->client_to_connection, client);
                                conn.client      = client;
                                in_s->stats.total_connections++;
                                in_s->stats.active_connections++;

                                conn.req = Request{
                                    .connection = &conn,
                                    .acquire    = &in_s->acquire_fn,
                                    .release    = &in_s->release_fn,
                                    .read       = &in_s->read_fn,
                                    .write      = &in_s->write_fn,
                                };
                                conn.task = (*in_handler)(conn.req);

                                if (static_cast<bool>(conn.task) && conn.task->done()) {
                                    close_and_cleanup(client);
                                }
                            }
                            continue;
                        }

                        Connection* conn = find(in_s->client_to_connection, event.handle);
                        if (conn == nullptr) {
                            continue;
                        }

                        if (has_err || has_hup) {
                            close_and_cleanup(event.handle);
                            continue;
                        }

                        if (!static_cast<bool>(conn->waiting_rwc)) {
                            continue;
                        }

                        auto waiting_it = find_it(in_s->waiting_op, event.handle);
                        if (waiting_it != in_s->waiting_op.end()) {
                            bool ready =
                                (waiting_it->second == SocketListenerState::Op::WaitRead && has_read) || (waiting_it->second == SocketListenerState::Op::WaitWrite && has_write);
                            if (ready) {
                                std::coroutine_handle<> h = conn->waiting_rwc;
                                conn->waiting_rwc         = std::coroutine_handle<>{};
                                remove_it(in_s->waiting_op, waiting_it);
                                h.resume();
                                if (static_cast<bool>(conn->task) && conn->task->done()) {
                                    close_and_cleanup(event.handle);
                                }
                            }
                        }
                    }
                    return true;
                }
            }
        };
    }
}
