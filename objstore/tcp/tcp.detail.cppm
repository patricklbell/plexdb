module;
#include "macros.h"
#include <coroutine>

export module objstore.tcp.detail;

import plexdb.os;
import plexdb.os.uring;
import plexdb.base;
import plexdb.arena;
import plexdb.tagged_union;
import plexdb.coroutine;
import objstore.tcp.types;
import objstore.log;

using namespace plexdb;

namespace objstore::tcp {
    // @note fat struct, not always meaningfull depending on the handler
    struct BufferInfo {
        PLEXDB_DEBUG_X(U32 buffer_idx = -1;)
        os::Handle client = os::zero_handle();
    };

    constexpr U32 INVALID_BUFFER_IDX = MAX_U32;

    U32 get_next_free_buffer_idx(U32& next_free_buffer_idx, const DynamicArray<BufferInfo>& buffer_infos, U32 buffer_count) {
        U32 initial_free_buffer_idx = next_free_buffer_idx;
        while (!os::is_zero_handle(buffer_infos[next_free_buffer_idx].client)) {
            next_free_buffer_idx = (next_free_buffer_idx + 1) % buffer_count;
            
            if (next_free_buffer_idx == initial_free_buffer_idx) {
                return INVALID_BUFFER_IDX;
            }
        }
        return next_free_buffer_idx;
    };

    // ========================================================================
    // io_uring implementation
    // ========================================================================
    export void listen_uring(
        uring::Ring& ring, Stats& stats,
        const ConnectionHandler auto& connection_handler,
        const os::Notifier& interrupt, volatile bool& should_exit
    ) {
        assert_true(static_cast<bool>(ring), "cannot listen to invalid uring");
        objstore::log::db_connection_max(MAX_CONCURRENT_CONNECTIONS);

        // state
        MapFixedSentinel<os::Handle, Connection, 2_u64*MAX_CONCURRENT_CONNECTIONS> client_to_connection;
        DynamicArray<BufferInfo> buffer_infos{ring.buffer_count};
        U32 next_free_buffer_idx = 0;
        
        // helpers
        auto close_and_cleanup = [&](const os::Handle& client, bool is_in_close_handler) {
            auto it = find_it(client_to_connection, client);
            if (it != client_to_connection.end()) {
                auto& connection = it->second;
                connection.client = os::zero_handle(); // @note marks connection as closed

                stats.active_connections--;
                objstore::log::db_connection_count(stats.active_connections);

                if (!is_in_close_handler) {
                    uring::sqe_push_close(ring, client);
                } else if (connection.waiting_rwc) {
                    // @note coroutine should not r/w/c after this and run to completion
                    connection.waiting_rwc.resume();
                    assert_true(static_cast<bool>(connection.task) && connection.task->done(), "request handler task completed after resuming from close");
                }

                // @todo api, currently silently drops handler task for failed read/write
                connection.task.reset();
                remove_it(client_to_connection, it);
            }
        };

        // user api
        AcquireRWBufferFunctor uring_acquire_rwbuffer_functor{[&](Connection* connection) -> Optional<RWBuffer> {
            U32 free_buffer_idx = get_next_free_buffer_idx(next_free_buffer_idx, buffer_infos, ring.buffer_count);
            // @todo error handling
            if (free_buffer_idx == INVALID_BUFFER_IDX) {
                return {};
            }
            
            BufferInfo& info = buffer_infos[free_buffer_idx];
            info.client = connection->client;
            PLEXDB_DEBUG_X(info.buffer_idx = free_buffer_idx;)

            return RWBuffer{
                .view   = TArrayView<U8>(ring.buffers + free_buffer_idx * ring.buffer_size, ring.buffer_size),
                .length = ring.buffer_size,
                .idx    = free_buffer_idx,
            };
        }};

        ReleaseRWBufferFunctor uring_release_rwbuffer_functor{[&]([[maybe_unused]] Connection* connection, const RWBuffer* buffer) {
            BufferInfo& info = buffer_infos[buffer->idx];
            info.client = os::zero_handle();
        }};

        AsyncReadFunctor uring_async_read_functor{[&](Connection* connection, RWBuffer* buffer) -> coroutine::Task<bool> {
            assert_true(!os::is_zero_handle(connection->client), "invalid read on closed connection");
            assert_true(!connection->waiting_rwc, "connection already has coroutine pending, this should never happen!");
            assert_true(buffer_infos[buffer->idx].client == connection->client, "buffer has not been acquire or acquired for a different connection");

            U32 byte_offset = buffer->view.ptr - (ring.buffers + buffer->idx*ring.buffer_size);
            assert_true(buffer->length <= MAX_U32, "read buffer view length too large");
            U32 byte_count = buffer->length;
            
            co_return co_await coroutine::Awaitable{
                [&](std::coroutine_handle<> h) {
                    connection->waiting_rwc = h;
                    uring::sqe_push_read(ring, connection->client, buffer->idx, byte_offset, byte_count);
                },
                [&]() -> bool {
                    connection->waiting_rwc = std::coroutine_handle<>{};
                    buffer->length = max(connection->data_rwc, 0_s64);
                    return connection->data_rwc > 0; // true = bytes received; false = connection closed/error
                }
            };
        }};

        AsyncWriteFunctor uring_async_write_functor{[&](Connection* connection, const RWBuffer* buffer) -> coroutine::Task<bool> {
            assert_true(!os::is_zero_handle(connection->client), "invalid write on closed connection");
            assert_true(!connection->waiting_rwc, "connection already has coroutine pending, this should never happen!");

            U32 byte_offset = buffer->view.ptr - (ring.buffers + buffer->idx*ring.buffer_size);
            assert_true(buffer->length <= MAX_U32, "write buffer view length too large");
            U32 byte_count = buffer->length;

            co_return co_await coroutine::Awaitable{
                [&](std::coroutine_handle<> h) {
                    connection->waiting_rwc = h;
                    uring::sqe_push_write(ring, connection->client, buffer->idx, byte_offset, byte_count);
                },
                [&]() -> bool {
                    connection->waiting_rwc = std::coroutine_handle<>{};
                    return connection->data_rwc == byte_count;
                }
            };
        }};

        AsyncCloseFunctor uring_async_close_functor{[&](Connection* connection) -> coroutine::Task<> {
            assert_true(!os::is_zero_handle(connection->client), "invalid close on closed connection");
            assert_true(!connection->waiting_rwc, "connection already has coroutine pending, this should never happen!");

            co_return co_await coroutine::Awaitable{
                [&](std::coroutine_handle<> h) {
                    connection->waiting_rwc = h;
                    uring::sqe_push_close(ring, connection->client);
                },
                [&]() {
                    connection->waiting_rwc = std::coroutine_handle<>{};
                }
            };
        }};

        // handlers
        // @note handles client responding to the server's read submission
        auto handle_read_completion = [&](const uring::ReadEvent& read) {
            BufferInfo& info = buffer_infos[read.buffer_idx];
            PLEXDB_DEBUG_X(assert_true(info.buffer_idx == read.buffer_idx, "buffer info was not written before read completion"));

            Connection* connection = find(client_to_connection, info.client);
            assert_true(connection != nullptr, "connection dropped before read completion");
            assert_true(static_cast<bool>(connection->waiting_rwc), "read completion for connection without waiting coroutine, this should never happen!");

            if (read.bytes_read > 0)
                stats.total_bytes_read += read.bytes_read;

            connection->data_rwc = read.bytes_read;
            connection->waiting_rwc.resume();
        };

        // @note handles completion of write from server to client
        auto handle_write_completion = [&](const uring::WriteEvent& write) {
            BufferInfo& info = buffer_infos[write.buffer_idx];
            PLEXDB_DEBUG_X(assert_true(info.buffer_idx == write.buffer_idx, "buffer info was not written before handler needed it"));

            Connection* connection = find(client_to_connection, info.client);
            assert_true(connection != nullptr, "connection dropped before write completion");

            if (write.bytes_written > 0)
                stats.total_bytes_written += write.bytes_written;

            connection->data_rwc = write.bytes_written;
            connection->waiting_rwc.resume();
        };

        auto handle_accept_completion = [&](const auto& accept) {
            using T = RemoveCVRef<decltype(accept)>;
            static_assert(Either<T, uring::AcceptEvent, uring::MultishotAcceptEvent>);

            if constexpr (!SameAs<T, uring::MultishotAcceptEvent>) {
                // re-arm to accept future connections ASAP
                uring::sqe_push_accept(ring);
            }

            socket_set_option(accept.client, os::SocketOption::NoDelay, true);

            // store new connection
            Connection& connection = find_or_insert(client_to_connection, accept.client);
            connection.client = accept.client;
            stats.total_connections++;
            stats.active_connections++;
            objstore::log::db_connection_count(stats.active_connections);

            connection.req = Request{
                .connection = &connection,
                .acquire = &uring_acquire_rwbuffer_functor,
                .release = &uring_release_rwbuffer_functor,
                .read = &uring_async_read_functor,
                .write = &uring_async_write_functor,
                .close = &uring_async_close_functor,
            };

            // eagerly begin task to handle request
            connection.task = connection_handler(connection.req);
        };

        auto handle_close_completion = [&](const uring::CloseEvent& close) {
            close_and_cleanup(close.client, /*is_in_close_handler*/ true);
        };

        // event loop
        os::Poll poll_until_server_or_interrupt{};
        poll_unblock_on(poll_until_server_or_interrupt, interrupt);
        poll_unblock_on(poll_until_server_or_interrupt, ring.server);

        uring::sqe_push_multishot_accept(ring);
        
        while (!should_exit) {
            uring::sqe_submit_non_blocking(ring);

            U32 cqe_entry_count = cqe_get_size(ring);
            U32 cqe_entry_processed_count = 0;
            for (U32 cqe_entry_idx = 0; cqe_entry_idx < cqe_entry_count; cqe_entry_idx++) {
                auto cqe = cqe_top(ring);
                cqe_pop(ring, 1);
                
                if (!static_cast<bool>(cqe)) {
                    continue;
                }

                cqe_entry_processed_count++;
                visit(cqe, [&](const auto& cqe) {
                    using T = RemoveCVRef<decltype(cqe)>;
                    if constexpr (SameAs<T, uring::ReadEvent>) {
                        handle_read_completion(cqe);
                    } else if constexpr (SameAs<T, uring::WriteEvent>) {
                        handle_write_completion(cqe);
                    } else if constexpr (Either<T, uring::AcceptEvent, uring::MultishotAcceptEvent>) {
                        handle_accept_completion(cqe);
                    } else if constexpr (SameAs<T, uring::CloseEvent>) {
                        handle_close_completion(cqe);
                    } else {
                        static_assert(!SameAs<T,T>);
                    }
                });
            }

            if (cqe_entry_processed_count == 0) {
                // @blocking wait until either the callers interrupt or a write to the server socket unblocks us
                block_until_poll_unblocks(poll_until_server_or_interrupt);
            }
        }

        // cleanup
        for (auto& it : client_to_connection) {
            uring::sqe_push_close(ring, it.second.client);
        }
        // @note silently drops pending request tasks @todo api
    }

    // @note non-io_uring async fallback using os::Poll + non-blocking sockets.
    export void listen_socket(
        os::Handle socket, Stats& stats,
        const ConnectionHandler auto& connection_handler,
        const os::Notifier& interrupt, volatile bool& should_exit
    ) {
        objstore::log::db_connection_max(MAX_CONCURRENT_CONNECTIONS);

        constexpr U32 BUFFER_SIZE  = 64_kb;
        constexpr U32 BUFFER_COUNT = MAX_CONCURRENT_CONNECTIONS;
        constexpr U8 WAIT_READ  = 1;
        constexpr U8 WAIT_WRITE = 2;
        constexpr os::PollEventMask SOCKET_DEFAULT_POLL_EVENTS =
            os::PollEventMask::Read | os::PollEventMask::Error | os::PollEventMask::HangUp;
        constexpr os::PollEventMask SOCKET_WRITE_POLL_EVENTS =
            SOCKET_DEFAULT_POLL_EVENTS | os::PollEventMask::Write;

        MapFixedSentinel<os::Handle, Connection, 2_u64*MAX_CONCURRENT_CONNECTIONS> client_to_connection;
        MapFixedSentinel<os::Handle, U8, 2_u64*MAX_CONCURRENT_CONNECTIONS> waiting_op;
        DynamicArray<BufferInfo> buffer_infos{BUFFER_COUNT};
        U32 next_free_buffer_idx = 0;
        U8* buffer_pool = os::allocate(U64(BUFFER_SIZE) * BUFFER_COUNT);

        // non-blocking accept + non-blocking client I/O, coroutine handlers suspend on socket readiness.
        os::socket_set_option(socket, os::SocketOption::NonBlocking, true);

        os::Poll poll{};
        poll_unblock_on(poll, interrupt);
        poll_unblock_on(poll, socket);

        auto close_and_cleanup = [&](const os::Handle& client) {
            auto it = find_it(client_to_connection, client);
            if (it == client_to_connection.end()) {
                return;
            }

            Connection& connection = it->second;
            if (!os::is_zero_handle(connection.client)) {
                os::socket_close(connection.client);
            }
            connection.client = os::zero_handle();

            if (stats.active_connections > 0) {
                stats.active_connections--;
                objstore::log::db_connection_count(stats.active_connections);
            }

            auto w_it = find_it(waiting_op, client);
            if (w_it != waiting_op.end()) {
                remove_it(waiting_op, w_it);
            }

            connection.task.reset();
            remove_it(client_to_connection, it);
        };

        AcquireRWBufferFunctor socket_acquire_functor{[&](Connection* connection) -> Optional<RWBuffer> {
            U32 free = get_next_free_buffer_idx(next_free_buffer_idx, buffer_infos, BUFFER_COUNT);
            if (free == INVALID_BUFFER_IDX) return {};

            buffer_infos[free].client = connection->client;
            PLEXDB_DEBUG_X(buffer_infos[free].buffer_idx = free;)

            return RWBuffer{
                .view   = TArrayView<U8>(buffer_pool + free * BUFFER_SIZE, BUFFER_SIZE),
                .length = BUFFER_SIZE,
                .idx    = free,
            };
        }};

        ReleaseRWBufferFunctor socket_release_functor{[&](Connection*, const RWBuffer* buffer) {
            buffer_infos[buffer->idx].client = os::zero_handle();
        }};

        AsyncReadFunctor socket_read_functor{[&](Connection* connection, RWBuffer* buffer) -> coroutine::Task<bool> {
            assert_true(!os::is_zero_handle(connection->client), "invalid read on closed connection");
            assert_true(!connection->waiting_rwc, "connection already has coroutine pending, this should never happen!");
            while (true) {
                auto result = os::socket_receive(connection->client, buffer->view.ptr, buffer->length);
                if (result.byte_count > 0) {
                    buffer->length = U64(result.byte_count);
                    stats.total_bytes_read += U64(result.byte_count);
                    co_return true;
                }

                buffer->length = 0;
                if (result.error == os::SocketError::WouldBlock) {
                    bool ready = co_await coroutine::Awaitable{
                        [&](std::coroutine_handle<> h) {
                            connection->waiting_rwc = h;
                            find_or_insert(waiting_op, connection->client) = WAIT_READ;
                        },
                        [&]() -> bool {
                            return !os::is_zero_handle(connection->client);
                        }
                    };
                    if (!ready) {
                        co_return false;
                    }
                    continue;
                }

                co_return false;
            }
        }};

        AsyncWriteFunctor socket_write_functor{[&](Connection* connection, const RWBuffer* buffer) -> coroutine::Task<bool> {
            assert_true(!os::is_zero_handle(connection->client), "invalid write on closed connection");
            assert_true(!connection->waiting_rwc, "connection already has coroutine pending, this should never happen!");

            U64 sent = 0;
            while (sent < buffer->length) {
                auto result = os::socket_send(connection->client, buffer->view.ptr + sent, buffer->length - sent);
                if (result.byte_count > 0) {
                    sent += U64(result.byte_count);
                    stats.total_bytes_written += U64(result.byte_count);
                    continue;
                }

                if (result.error == os::SocketError::WouldBlock) {
                    bool ready = co_await coroutine::Awaitable{
                        [&](std::coroutine_handle<> h) {
                            connection->waiting_rwc = h;
                            find_or_insert(waiting_op, connection->client) = WAIT_WRITE;
                            os::poll_update(poll, connection->client, SOCKET_WRITE_POLL_EVENTS);
                        },
                        [&]() -> bool {
                            if (!os::is_zero_handle(connection->client)) {
                                os::poll_update(poll, connection->client, SOCKET_DEFAULT_POLL_EVENTS);
                            }
                            return !os::is_zero_handle(connection->client);
                        }
                    };
                    if (!ready) {
                        co_return false;
                    }
                    continue;
                }

                co_return false;
            }

            co_return true;
        }};

        AsyncCloseFunctor socket_close_functor{[&](Connection* connection) -> coroutine::Task<> {
            if (!os::is_zero_handle(connection->client)) {
                os::socket_close(connection->client);
            }
            auto w_it = find_it(waiting_op, connection->client);
            if (w_it != waiting_op.end()) {
                remove_it(waiting_op, w_it);
            }
            connection->client = os::zero_handle();
            co_return;
        }};

        Array<os::PollEvent, max(128_u32, MAX_CONCURRENT_CONNECTIONS + 2)> events_storage{};
        while (!should_exit) {
            CappedTArrayView<os::PollEvent> events(events_storage.values, events_storage.length, 0);
            poll_wait(poll, &events);

            for (const auto& event : events) {
                bool has_read  = (U32(event.events) & U32(os::PollEventMask::Read)) != 0;
                bool has_write = (U32(event.events) & U32(os::PollEventMask::Write)) != 0;
                bool has_err   = (U32(event.events) & U32(os::PollEventMask::Error)) != 0;
                bool has_hup   = (U32(event.events) & U32(os::PollEventMask::HangUp)) != 0;

                if (event.handle == interrupt.read) {
                    continue;
                }

                if (event.handle == socket) {
                    while (true) {
                        os::Handle client = os::socket_accept(socket);
                        if (os::is_zero_handle(client)) {
                            break;
                        }

                        if (stats.active_connections >= MAX_CONCURRENT_CONNECTIONS) {
                            stats.dropped_connections++;
                            os::socket_close(client);
                            continue;
                        }

                        os::socket_set_option(client, os::SocketOption::NonBlocking, true);
                        os::socket_set_option(client, os::SocketOption::NoDelay, true);
                        poll_unblock_on(
                            poll,
                            client,
                            SOCKET_DEFAULT_POLL_EVENTS
                        );

                        Connection& connection = find_or_insert(client_to_connection, client);
                        connection.client = client;
                        stats.total_connections++;
                        stats.active_connections++;
                        objstore::log::db_connection_count(stats.active_connections);

                        connection.req = Request{
                            .connection = &connection,
                            .acquire    = &socket_acquire_functor,
                            .release    = &socket_release_functor,
                            .read       = &socket_read_functor,
                            .write      = &socket_write_functor,
                            .close      = &socket_close_functor,
                        };
                        // eagerly begin task to handle connection
                        connection.task = connection_handler(connection.req);

                        if (static_cast<bool>(connection.task) && connection.task->done()) {
                            close_and_cleanup(client);
                        }
                    }
                    continue;
                }

                Connection* connection = find(client_to_connection, event.handle);
                if (connection == nullptr) {
                    continue;
                }

                if (has_err || has_hup) {
                    close_and_cleanup(event.handle);
                    continue;
                }

                auto waiting_it = find_it(waiting_op, event.handle);
                if (waiting_it != waiting_op.end() && static_cast<bool>(connection->waiting_rwc)) {
                    bool ready_for_waiting =
                        (waiting_it->second == WAIT_READ  && has_read) ||
                        (waiting_it->second == WAIT_WRITE && has_write);
                    if (ready_for_waiting) {
                        std::coroutine_handle<> h = connection->waiting_rwc;
                        connection->waiting_rwc = std::coroutine_handle<>{};
                        remove_it(waiting_op, waiting_it);
                        h.resume();
                        if (static_cast<bool>(connection->task) && connection->task->done()) {
                            close_and_cleanup(event.handle);
                        }
                    }
                }
            }
        }

        for (auto& it : client_to_connection) {
            if (!os::is_zero_handle(it.second.client)) {
                os::socket_close(it.second.client);
            }
        }
        os::deallocate(buffer_pool);
    }
}