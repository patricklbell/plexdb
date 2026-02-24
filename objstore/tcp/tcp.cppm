module;
#include "macros.h"

#include <liburing.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

export module objstore.tcp;

import plexdb.base;
import plexdb.arena;

using namespace plexdb;

namespace objstore::tcp {
    export constexpr int MAX_CONNECTIONS = 10000;
    
    // the event type is encoded in the upper bits of user_data 
    enum EventType : U64 {
        TYPE_ACCEPT             = 0x1_u64 << 60,
        TYPE_READ               = 0x2_u64 << 60,
        TYPE_WRITE              = 0x3_u64 << 60,
        TYPE_CLOSE              = 0x4_u64 << 60,
        TYPE_MULTISHOT_ACCEPT   = 0x5_u64 << 60,

        TYPE_MASK  = 0xF_u64 << 60,
        DATA_MASK = ~TYPE_MASK
    };

    struct PoolRequest {
        int buffer_idx = 0;
        int fd         = 0;
        size_t len     = 0;
    };

    // ========================================================================
    // Buffer chain for pending requests
    // When a request spans multiple reads, we chain buffers together.
    // If the chain grows, fall back to dynamic allocation
    // ========================================================================
    constexpr int MAX_SMALL_CHAIN_BUFFERS_COUNT = 4;
    constexpr int LARGE_BUFFER_SIZE = 64 * 1_kb;

    export struct Chunk {
        int buffer_idx = -1;
        TArrayView<char,int> data{};
    };

    using ChunkNode = Deque<Chunk>::Node;

    struct BufferChain {
        arena::ArenaPage* arena;
        CappedArray<ChunkNode, MAX_SMALL_CHAIN_BUFFERS_COUNT> small_chunks;
        Deque<Chunk> chunks;
    };

    struct PoolConnection {
        int fd           = 0;
        sockaddr_in addr = {0};
        bool active      = false;
        BufferChain rx_chain;
    };

    export struct Pool {
        int listen_fd;
        int epoll_fd;
        int signal_fd;

        // Runtime configuration, determined from sysinfo at construction
        int buffer_size;
        int num_buffers;
        int queue_depth;
        int max_cqe_batch;
        bool use_io_uring;

        char* buffer_pool;
        bool buffer_pool_has_huge_pages;
        bool* buffer_in_use;
        size_t free_buffer_idx;
        
        io_uring ring;
        iovec* iovecs;
        PoolRequest* request_pool;
        
        MapFixedSentinel<int, PoolConnection, 2*MAX_CONNECTIONS> connections;

        Pool(int port, int signal_fd);
        ~Pool();
    };

    export struct Stats {
        U64 total_connections   = 0;
        U64 active_connections  = 0;
        U64 total_bytes_read    = 0;
        U64 total_bytes_written = 0;
    };

    export enum class RequestStatus {
        // The request has been processed completely
        Handled,
        // The request is pending further data from the client, some data may have been consumed
        Pending,
        // The connection has been processed and should be closed
        Close,
    };

    export struct Request {
        void* context;
        int id;
        Deque<Chunk>* chunks;
    };

    template<typename F>
    concept OnChunk = requires(F f, Request& req) {
        { f(req) } -> SameAs<RequestStatus>;
    };

    template<typename F>
    concept OnClose = requires(F f, int id) {
        { f(id) };
    };

    // ========================================================================
    // init helpers
    // ========================================================================
    void init_socket(Pool& pool, int port);
    void init_uring(Pool& pool);
    void init_buffers(Pool& pool);
    void init_socket_buffers(Pool& pool);
    void init_epoll(Pool& pool);
    void init_socket_epoll(Pool& pool);

    // ========================================================================
    // buffer chain management
    // ========================================================================
    // @note returns true if the buffer_idx is in use
    bool append_to_buffer_chain(BufferChain& chain, int buffer_idx, char* buffer_data, int bytes_read);
    void release_chain(Pool& pool, BufferChain& chain);
    int  alloc_buffer(Pool& pool);

    // ========================================================================
    // connection management
    // ========================================================================
    void submit_accept(Pool& pool);
    void submit_multishot_accept(Pool& pool);
    void handle_accept(Pool& pool, Stats& stats, io_uring_cqe* cqe);
    void handle_multishot_accept(Pool& pool, Stats& stats, io_uring_cqe* cqe);
    void submit_close(Pool& pool, int fd);

    void handle_close(const OnClose auto& on_close_callback, Pool& pool, Stats& stats, io_uring_cqe* cqe, int fd) {
        // close completed (or failed, but fd is closed anyway)
        if (try_remove(pool.connections, fd)) {
            stats.active_connections--;
        }
        on_close_callback(fd);
    }

    // ========================================================================
    // io
    // ========================================================================
    void submit_write(Pool& pool, int fd, int buffer_idx, size_t len);
    void submit_read(Pool& pool, int fd);
    void handle_write(Pool& pool, Stats& stats, io_uring_cqe* cqe, int buffer_idx);

    void handle_read(const OnChunk auto& on_chunk_callback, Pool& pool, Stats& stats, io_uring_cqe* cqe, int buffer_idx) {
        PoolRequest& request = pool.request_pool[buffer_idx];
        int fd = request.fd;
        
        // Connection closed or error
        if (cqe->res <= 0) {
            pool.buffer_in_use[buffer_idx] = false;
            if (try_remove(pool.connections, fd))
                stats.active_connections--;
            submit_close(pool, fd);
            return;
        }
        
        int bytes_read = cqe->res;
        stats.total_bytes_read += bytes_read;
        request.len += bytes_read;

        // get the connection to access the buffer chain
        PoolConnection* conn = find(pool.connections, fd);
        assert_true(conn != nullptr, "connection not found for read, this should never happen");
        BufferChain& chain = conn->rx_chain;

        {
            assert_true(buffer_idx >= 0 && buffer_idx < pool.num_buffers, "read buffer idx out of range, this should never happen");
            assert_true(bytes_read >= 0 && bytes_read <= pool.buffer_size, "buffer overflow in cqe, this should never happen");
            
            char* buffer_data = &pool.buffer_pool[(size_t)pool.buffer_size * (size_t)buffer_idx];
            pool.buffer_in_use[buffer_idx] = append_to_buffer_chain(chain, buffer_idx, buffer_data, bytes_read);
            
            Request req{
                .context = &pool,
                .id = fd,
                .chunks = &chain.chunks
            };
            
            RequestStatus status = on_chunk_callback(req);

            switch (status) {
                case RequestStatus::Handled:{
                    release_chain(pool, chain);
                    submit_read(pool, fd);
                }break;
                case RequestStatus::Pending:{
                    submit_read(pool, fd);
                }break;
                case RequestStatus::Close:{
                    release_chain(pool, chain);
                    submit_close(pool, fd);
                }break;
            }   
        }
    }

    // ========================================================================
    // listen
    // ========================================================================
    void handle_cqe(const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, Pool& pool, Stats& stats, io_uring_cqe* cqe) {
        U64 user_data = (U64)io_uring_cqe_get_data(cqe);
        EventType type = (EventType)(user_data & TYPE_MASK);
        int data = (int)(user_data & DATA_MASK);
        
        switch (type) {
            case TYPE_ACCEPT:{
                handle_accept(pool, stats, cqe);
            }break;
            case TYPE_READ:{
                handle_read(on_chunk_callback, pool, stats, cqe, data);
            }break;
            case TYPE_WRITE:{
                handle_write(pool, stats, cqe, data);
            }break;
            case TYPE_CLOSE:{
                handle_close(on_close_callback, pool, stats, cqe, data);
            }break;
            case TYPE_MULTISHOT_ACCEPT:{
                handle_multishot_accept(pool, stats, cqe);
            }break;

            case TYPE_MASK:{}break;
            case DATA_MASK:{}break;
        }
    }

    void drain_or_block_cqes(const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, Pool& pool, Stats& stats, volatile bool& exit_signal) {
        io_uring_cqe* cqe;
        unsigned head;
        unsigned count = 0;
        
        io_uring_for_each_cqe(&pool.ring, head, cqe) {
            handle_cqe(on_chunk_callback, on_close_callback, pool, stats, cqe);
            count++;
            
            // @note avoids starving the kernel of cqes if processing is slow 
            if ((int)count >= pool.max_cqe_batch) {
                break;
            }
        }
        
        if (count > 0) {
            io_uring_cq_advance(&pool.ring, count);
        } else {
            struct epoll_event event;
            epoll_wait(pool.epoll_fd, &event, 1, -1);
        }
    }

    // ========================================================================
    // socket fallback listen loop (used when io_uring is unavailable)
    // ========================================================================
    void listen_sockets(const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, Pool& pool, Stats& stats, volatile bool& exit_signal) {
        constexpr int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];

        while (!exit_signal) {
            int n = epoll_wait(pool.epoll_fd, events, MAX_EVENTS, -1);
            if (n < 0) continue;

            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;

                if (fd == pool.signal_fd) {
                    exit_signal = true;
                    break;
                }

                if (fd == pool.listen_fd) {
                    // Accept new connections
                    while (true) {
                        struct sockaddr_in addr{};
                        socklen_t addr_len = sizeof(addr);
                        int client_fd = accept(pool.listen_fd, (sockaddr*)&addr, &addr_len);
                        if (client_fd < 0) break;

                        int flags = fcntl(client_fd, F_GETFL, 0);
                        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                        PoolConnection& conn = insert(pool.connections, client_fd);
                        conn.fd = client_fd;
                        conn.active = true;
                        stats.total_connections++;
                        stats.active_connections++;

                        struct epoll_event ce{};
                        ce.events = EPOLLIN;
                        ce.data.fd = client_fd;
                        epoll_ctl(pool.epoll_fd, EPOLL_CTL_ADD, client_fd, &ce);
                    }
                    continue;
                }

                // Client data available
                int client_fd = fd;
                int buffer_idx = alloc_buffer(pool);
                if (buffer_idx < 0) {
                    // @todo log dropped connection due to buffer exhaustion
                    epoll_ctl(pool.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    ::close(client_fd);
                    if (try_remove(pool.connections, client_fd))
                        stats.active_connections--;
                    on_close_callback(client_fd);
                    continue;
                }

                char* buf = &pool.buffer_pool[(size_t)pool.buffer_size * (size_t)buffer_idx];
                ssize_t bytes = recv(client_fd, buf, (size_t)pool.buffer_size, 0);

                if (bytes <= 0) {
                    pool.buffer_in_use[buffer_idx] = false;
                    if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        PoolConnection* conn2 = find(pool.connections, client_fd);
                        if (conn2 != nullptr) release_chain(pool, conn2->rx_chain);
                        epoll_ctl(pool.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        ::close(client_fd);
                        if (try_remove(pool.connections, client_fd))
                            stats.active_connections--;
                        on_close_callback(client_fd);
                    }
                    continue;
                }

                stats.total_bytes_read += (U64)bytes;

                PoolConnection* conn = find(pool.connections, client_fd);
                if (conn == nullptr) {
                    pool.buffer_in_use[buffer_idx] = false;
                    continue;
                }

                BufferChain& chain = conn->rx_chain;
                pool.buffer_in_use[buffer_idx] = append_to_buffer_chain(chain, buffer_idx, buf, (int)bytes);

                Request req{ .context = &pool, .id = client_fd, .chunks = &chain.chunks };
                RequestStatus status = on_chunk_callback(req);

                switch (status) {
                    case RequestStatus::Handled: {
                        release_chain(pool, chain);
                    } break;
                    case RequestStatus::Pending: {
                        // keep reading
                    } break;
                    case RequestStatus::Close: {
                        release_chain(pool, chain);
                        epoll_ctl(pool.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        ::close(client_fd);
                        if (try_remove(pool.connections, client_fd))
                            stats.active_connections--;
                        on_close_callback(client_fd);
                    } break;
                }
            }
        }
    }

    export void listen(const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, Pool& pool, Stats& stats, volatile bool& exit_signal) {
        if (pool.use_io_uring) {
            submit_multishot_accept(pool);
            while (!exit_signal) {
                io_uring_submit(&pool.ring);
                drain_or_block_cqes(on_chunk_callback, on_close_callback, pool, stats, exit_signal);
            }
        } else {
            listen_sockets(on_chunk_callback, on_close_callback, pool, stats, exit_signal);
        }
    }

    // ========================================================================
    // http
    // @todo remove
    // ========================================================================
    export void return_http_success(Request& req, const String8& body);
    export void return_http_fail(Request& req, int status, const String8& body, bool close=false);

    // ========================================================================
    // printing
    // ========================================================================
    export AutoString8 to_str(const objstore::tcp::Stats& s);
}