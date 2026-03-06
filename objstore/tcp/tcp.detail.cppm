module;
#include "macros.h"

export module objstore.tcp.detail;

import plexdb.os;
import plexdb.os.uring;
import plexdb.base;
import plexdb.arena;
import objstore.tcp.types;

using namespace plexdb;

namespace objstore::tcp {
    [[nodiscard("handle hold/release for appended buffer")]]
    bool append_chunk_chain(ChunkChain& chain, U32 buffer_idx, U8* buffer_data, int byte_count);
    void release_chunk_chain(DynamicArray<BufferInfo>& buffer_infos, ChunkChain& chain);

    export Stats listen_uring(
        uring::Ring& ring,
        const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, const OnOpen auto& on_open_callback,
        const os::Notifier& interrupt, volatile bool& should_exit
    ) {
        assert_true(static_cast<bool>(ring), "cannot listen to invalid uring");

        Stats stats;

        MapFixedSentinel<os::Handle, Connection, 2_u64*MAX_CONCURRENT_CONNECTIONS> client_to_connection;
        DynamicArray<BufferInfo> buffer_infos{ring.buffer_count};
        
        constexpr U32 INVALID_BUFFER_IDX = MAX_U32;
        U32 next_free_buffer_idx = 0;
        auto get_next_free_buffer_idx = [&]() -> U32 {
            U32 initial_free_buffer_idx = next_free_buffer_idx;
            while (!os::is_zero_handle(buffer_infos[next_free_buffer_idx].client)) {
                next_free_buffer_idx += (next_free_buffer_idx + 1) % ring.buffer_count;
                
                if (next_free_buffer_idx == initial_free_buffer_idx) {
                    return INVALID_BUFFER_IDX;
                }
            }
            return next_free_buffer_idx;
        };

        auto release_buffer = [](BufferInfo& info) {
            info.client = os::zero_handle();
        };

        auto acquire_buffer = [](BufferInfo& info, os::Handle client) {
            info.client = client;
        };

        auto close_and_cleanup = [&](const os::Handle& client, bool is_server_side_initiating=true) {
            auto it = find_it(client_to_connection, client);
            if (it != client_to_connection.end()) {
                on_close_callback(&(*it).second);

                remove_it(client_to_connection, it);
                stats.active_connections--;
                if (is_server_side_initiating) {
                    uring::sqe_push_close(ring, client);
                }
            }
        };

        AsyncWriteFunctor uring_async_buffer_write_functor{[&](Connection* connection, U64 buffer_idx, U32 offset, U32 byte_count) {
            uring::sqe_push_write(ring, connection->client, buffer_idx, offset, byte_count);
        }};

        // @note handles client resporead.client = infonding to the server's read submission
        auto handle_read_completion = [&](const uring::ReadEvent& read) {
            BufferInfo& info = buffer_infos[read.buffer_idx];
            PLEXDB_DEBUG_X(assert_true(info.buffer_idx == read.buffer_idx, "buffer info was not written before read completion"));

            Connection* connection = find(client_to_connection, info.client);
            if (connection == nullptr) {
                // @note connection was closed before read completed,
                // mark the buffer as free of a connection
                release_buffer(info);
                return;
            }

            // handle bad read or graceful disconnect by closing connection
            if (read.bytes_read <= 0) {
                close_and_cleanup(connection->client);
                release_buffer(info);
                return;
            }
            stats.total_bytes_read += read.bytes_read;            
            
            U8* buffer = get_buffer_ptr(ring, read.buffer_idx);
            // @note buffer is needed for write and is released in the write completion handler
            bool active = append_chunk_chain(connection->chunk_chain, read.buffer_idx, buffer, read.bytes_read);
            
            Request req{connection, &uring_async_buffer_write_functor};
            RequestStatus status = on_chunk_callback(req);

            switch (status) {
                case RequestStatus::Handled:
                case RequestStatus::Pending:{
                    U32 next_buffer_idx = get_next_free_buffer_idx();
                    if (next_buffer_idx == INVALID_BUFFER_IDX) {
                        release_chunk_chain(buffer_infos, connection->chunk_chain);
                        // @todo log/avoid starvation
                        close_and_cleanup(connection->client);
                        return;
                    }
                    if (status == RequestStatus::Handled) {
                        release_chunk_chain(buffer_infos, connection->chunk_chain);
                        acquire_buffer(info, connection->client);
                    }

                    // acquire buffer for client to read to
                    BufferInfo& info = buffer_infos[next_buffer_idx];
                    acquire_buffer(info, connection->client);
                    PLEXDB_DEBUG_X(info.buffer_idx = next_buffer_idx;)

                    uring::sqe_push_read(ring, connection->client, next_buffer_idx);
                    return;
                }break;
                case RequestStatus::Close:{
                    release_chunk_chain(buffer_infos, connection->chunk_chain);
                    close_and_cleanup(connection->client);
                    release_buffer(info);
                    return;
                }
            }
        };

        // @note handles completion of write from server to client
        auto handle_write_completion = [&](const uring::WriteEvent& write) {
            BufferInfo& info = buffer_infos[write.buffer_idx];
            PLEXDB_DEBUG_X(assert_true(info.buffer_idx == write.buffer_idx, "buffer info was not written before handler needed it"));

            release_buffer(info);

            if (write.bytes_written < 0) {
                close_and_cleanup(info.client);
                return;
            }

            stats.total_bytes_written += write.bytes_written;
        };

        auto handle_accept_completion = [&](const auto& accept) {
            using T = RemoveCVRef<decltype(accept)>;
            static_assert(Either<T, uring::AcceptEvent, uring::MultishotAcceptEvent>);

            if constexpr (!SameAs<T, uring::MultishotAcceptEvent>) {
                // re-arm to accept future connections ASAP
                uring::sqe_push_accept(ring);
            }

            socket_set_option(accept.client, os::SocketOption::NoDelay, true);
            
            U32 buffer_idx = get_next_free_buffer_idx();
            if (buffer_idx == INVALID_BUFFER_IDX) {
                // @todo log starvation
                stats.dropped_connections += 1;
                uring::sqe_push_close(ring, accept.client);
                return;
            }

            // store new connection
            Connection& connection = find_or_insert(client_to_connection, accept.client);
            release_chunk_chain(buffer_infos, connection.chunk_chain);
            connection.client = accept.client;
            connection.chunk_chain.chunk_size = ring.buffer_size;
            stats.total_connections++;

            // notify
            on_open_callback(&connection);

            // acquire buffer for client to read to
            BufferInfo& info = buffer_infos[buffer_idx];
            acquire_buffer(info, accept.client);
            PLEXDB_DEBUG_X(info.buffer_idx = buffer_idx;)

            // read first packet from the client
            uring::sqe_push_read(ring, accept.client, buffer_idx);
        };

        auto handle_close_completion = [&](const uring::CloseEvent& close) {
            close_and_cleanup(close.client, /*is_server_side_initiating*/ false);
        };

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
                // @blocking wait until either the callers interrupt or a writeo to the server socket unblocks us
                block_until_poll_unblocks(poll_until_server_or_interrupt);
            }
        }

        return stats;
    }

    // @note simple socket-based fallback for systems without io_uring
    export Stats listen_socket(
        os::Handle socket,
        const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, const OnOpen auto& on_open_callback,
        const os::Notifier& interrupt, volatile bool& should_exit
    ) {
        Stats stats;

        // @note server socket must be non-blocking for poll-based accept
        os::socket_set_option(socket, os::SocketOption::NonBlocking, true);

        constexpr U32 BUFFER_SIZE = 4_kb;
        constexpr U32 BUFFER_COUNT = 64;

        MapFixedSentinel<os::Handle, Connection, 2_u64*MAX_CONCURRENT_CONNECTIONS> client_to_connection;
        DynamicArray<BufferInfo> buffer_infos{BUFFER_COUNT};

        // initialize all buffer slots as free
        for (U32 i = 0; i < BUFFER_COUNT; i++) {
            buffer_infos[i].client = os::zero_handle();
        }

        U8* buffer_pool = os::allocate(BUFFER_SIZE * BUFFER_COUNT);
        auto get_buffer_ptr = [&](U32 idx) -> U8* { return buffer_pool + idx * BUFFER_SIZE; };

        constexpr U32 INVALID_BUFFER_IDX = MAX_U32;
        U32 next_free_buffer_idx = 0;
        auto get_next_free_buffer_idx = [&]() -> U32 {
            U32 initial_free_buffer_idx = next_free_buffer_idx;
            while (!os::is_zero_handle(buffer_infos[next_free_buffer_idx].client)) {
                next_free_buffer_idx = (next_free_buffer_idx + 1) % BUFFER_COUNT;
                
                if (next_free_buffer_idx == initial_free_buffer_idx) {
                    return INVALID_BUFFER_IDX;
                }
            }
            return next_free_buffer_idx;
        };

        auto release_buffer = [](BufferInfo& info) {
            info.client = os::zero_handle();
        };

        auto acquire_buffer = [](BufferInfo& info, os::Handle client) {
            info.client = client;
        };

        os::Poll poll{};
        poll_unblock_on(poll, interrupt);
        poll_unblock_on(poll, socket);

        CappedArray<os::Handle, MAX_CONCURRENT_CONNECTIONS> active_clients;

        AsyncWriteFunctor socket_async_write_functor{[&](Connection* connection, U64 buffer_idx, U32 offset, U32 byte_count) {
            U8* buffer = get_buffer_ptr(static_cast<U32>(buffer_idx)) + offset;
            auto res = os::socket_send_all(connection->client, buffer, byte_count);
            assert_true(res.error == os::SocketError::None, "error sending on buffer");
            stats.total_bytes_written += byte_count;
        }};

        while (!should_exit) {
            block_until_poll_unblocks(poll);
            
            if (should_exit) break;

            // accept new connections
            while (true) {
                os::Handle client = os::socket_accept(socket);
                if (os::is_zero_handle(client)) break;

                os::socket_set_option(client, os::SocketOption::NonBlocking, true);
                os::socket_set_option(client, os::SocketOption::NoDelay, true);

                U32 buffer_idx = get_next_free_buffer_idx();
                if (buffer_idx == INVALID_BUFFER_IDX) {
                    stats.dropped_connections++;
                    os::socket_close(client);
                    continue;
                }

                Connection& connection = find_or_insert(client_to_connection, client);
                release_chunk_chain(buffer_infos, connection.chunk_chain);
                connection.client = client;
                connection.chunk_chain.chunk_size = BUFFER_SIZE;
                stats.total_connections++;

                on_open_callback(&connection);
                
                push_back(active_clients, client);
                poll_unblock_on(poll, client);
                
                acquire_buffer(buffer_infos[buffer_idx], client);
                stats.active_connections++;
            }

            // read from active clients
            for (U64 i = 0; i < active_clients.cap; ) {
                os::Handle client = active_clients[i];
                Connection* connection = find(client_to_connection, client);
                
                if (connection == nullptr) {
                    swap_remove(active_clients, i);
                    continue;
                }

                U32 buffer_idx = get_next_free_buffer_idx();
                if (buffer_idx == INVALID_BUFFER_IDX) {
                    i++;
                    continue;
                }

                U8* buffer = get_buffer_ptr(buffer_idx);
                auto result = os::socket_receive(client, buffer, BUFFER_SIZE);

                if (result.error == os::SocketError::WouldBlock) {
                    i++;
                    continue;
                }

                if (result.byte_count <= 0 || result.error != os::SocketError::None) {
                    on_close_callback(connection);
                    release_chunk_chain(buffer_infos, connection->chunk_chain);
                    try_remove(client_to_connection, client);
                    os::socket_close(client);
                    swap_remove(active_clients, i);
                    stats.active_connections--;
                    continue;
                }

                stats.total_bytes_read += result.byte_count;

                bool active = append_chunk_chain(connection->chunk_chain, buffer_idx, buffer, static_cast<int>(result.byte_count));
                if (!active) {
                    release_buffer(buffer_infos[buffer_idx]);
                }

                Request req{connection, &socket_async_write_functor};
                RequestStatus status = on_chunk_callback(req);

                switch (status) {
                    case RequestStatus::Handled:{
                        release_chunk_chain(buffer_infos, connection->chunk_chain);
                    }break;
                    case RequestStatus::Pending:{
                    }break;
                    case RequestStatus::Close:{
                        release_chunk_chain(buffer_infos, connection->chunk_chain);
                        on_close_callback(connection);
                        try_remove(client_to_connection, client);
                        os::socket_close(client);
                        swap_remove(active_clients, i);
                        stats.active_connections--;
                        continue;
                    }break;
                }

                i++;
            }
        }

        // cleanup
        for (os::Handle client : active_clients) {
            Connection* connection = find(client_to_connection, client);
            if (connection) {
                on_close_callback(connection);
                release_chunk_chain(buffer_infos, connection->chunk_chain);
            }
            os::socket_close(client);
        }

        os::deallocate(buffer_pool);
        return stats;
    }
}