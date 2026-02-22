module;
#include "macros.h"

#include <stdlib.h>

#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/epoll.h>

module objstore.tcp;

import plexdb.os;
import plexdb.arena;

namespace objstore::tcp {
    Pool::Pool(int port, int signal_fd): free_buffer_idx(0), signal_fd(signal_fd)  {
        init_socket(*this, port);
        init_uring(*this);
        init_buffers(*this);
        init_epoll(*this);
    }
    
    Pool::~Pool() {
        // Release all buffer chains for active connections
        for (auto& kv : this->connections) {
            release_chain(*this, kv.second.rx_chain);
        }
        
        int err = io_uring_unregister_buffers(&this->ring);
        assert_true_always(err == 0, "io_uring_unregister_buffers failed");
        io_uring_queue_exit(&this->ring);
        // err = munmap(this->buffer_pool, BUFFER_SIZE * NUM_BUFFERS);
        // assert_true_always(err == 0, "munmap failed");
        free(this->buffer_pool);
        err = close(this->listen_fd);
        assert_true_always(err == 0, "close failed");
    }

    // ========================================================================
    // buffer chain management
    // ========================================================================
    bool append_to_buffer_chain(BufferChain& chain, int buffer_idx, char* buffer_data, int bytes_read) {
        // small chunk
        if (!full(chain.small_chunks)) {
            ChunkNode& node = emplace_back(chain.small_chunks);
            node.value.buffer_idx = buffer_idx;
            node.value.data = { buffer_data, bytes_read };
            
            push_back(chain.chunks, &node);

            // @note buffer_idx is in use
            return true; 
        }
        
        // large chunk
        if (chain.arena == nullptr) {
            chain.arena = arena::allocate(LARGE_BUFFER_SIZE, nullptr);
        }
        
        char* large_buffer = arena::push_array_no_zero<char>(&chain.arena, bytes_read);
        os::memory_copy(large_buffer, buffer_data, bytes_read);

        ChunkNode* node = arena::push_array_no_zero<ChunkNode>(&chain.arena, 1);
        node->value.buffer_idx = -1;
        node->value.data = { large_buffer, bytes_read };
        
        push_back(chain.chunks, node);

        // @note buffer_idx is not longer in use
        return false;
    }

    void release_chain(Pool& pool, BufferChain& chain) {
        for (Chunk& chunk : chain.chunks) {
            if (chunk.buffer_idx >= 0) {
                pool.buffer_in_use[chunk.buffer_idx] = false;
            }
        }
        if (chain.arena != nullptr) {
            arena::deallocate(chain.arena);
        }

        clear(chain.chunks);
        chain.small_chunks.cap = 0;
        chain.arena = nullptr;
    }

    // ========================================================================
    // static helpers
    // ========================================================================
    // finds the next free buffer in the ring
    // @todo virtual address mapping for ring buffer
    static int alloc_buffer(Pool& pool) {
        for (int i = 0; i < NUM_BUFFERS; i++) {
            int idx = (pool.free_buffer_idx + i) % NUM_BUFFERS;
            if (!pool.buffer_in_use[idx]) {
                pool.buffer_in_use[idx] = true;
                pool.free_buffer_idx = (idx + 1) % NUM_BUFFERS;
                return idx;
            }
        }
        return -1;
    }

    static void set_tcp_socket_options(int fd) {
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }

    // ========================================================================
    // init helpers
    //  - initialise tcp socket, configure uring and acquire resources
    // ========================================================================
    void init_socket(Pool& pool, int port) {
        pool.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        assert_true_always(pool.listen_fd >= 0, "failed to open socket for pool");
        
        // Socket options for performance
        int opt = 1;
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        
        // TCP optimizations
        set_tcp_socket_options(pool.listen_fd);
        
        // Increase buffer sizes
        int bufsize = 16 * 1024 * 1024;
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        
        // Set non-blocking
        int flags = fcntl(pool.listen_fd, F_GETFL, 0);
        fcntl(pool.listen_fd, F_SETFL, flags | O_NONBLOCK);
        
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        int res = bind(pool.listen_fd, (sockaddr*)&addr, sizeof(addr));
        assert_true_always(res == 0, "failed to bind socket to address");
        
        res = ::listen(pool.listen_fd, port);
        assert_true_always(res == 0, "failed to listen on address");
    }
    
    void init_uring(Pool& pool) {
        io_uring_params params{};
        
        // TigerBeetle-style flags
        // @todo detect CAP
        // params.flags = IORING_SETUP_COOP_TASKRUN |     // Cooperative scheduling
        //                IORING_SETUP_DEFER_TASKRUN;     // Defer completion processing
        // params.flags |= IORING_SETUP_SQPOLL;
        
        params.sq_thread_idle = 2000;
        params.cq_entries = QUEUE_DEPTH * 2;
        static_assert(has_single_bit(QUEUE_DEPTH * 2), "cq_entries must be a power of 2");
        
        int res = io_uring_queue_init_params(QUEUE_DEPTH, &pool.ring, &params);
        assert_true_always(res == 0, "io_uring_queue_init_params failed");
    }
    
    void init_buffers(Pool& pool) {
        size_t total_size = BUFFER_SIZE * NUM_BUFFERS;
        
        // @todo CAP_IPC_LOCK
        // pool.buffer_pool = (char*)mmap(
        //     NULL, total_size,
        //     PROT_READ | PROT_WRITE,
        //     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
        //     -1, 0
        // );
        
        // if (pool.buffer_pool == MAP_FAILED) {
        //     pool.buffer_pool = (char*)mmap(
        //         NULL, total_size,
        //         PROT_READ | PROT_WRITE,
        //         MAP_PRIVATE | MAP_ANONYMOUS,
        //         -1, 0
        //     );
        //     assert_true_always(pool.buffer_pool != MAP_FAILED, "mmap failed");
        //     pool.buffer_pool_has_huge_pages = false;
        // } else {
        //     pool.buffer_pool_has_huge_pages = true;
        // }
        // madvise(pool.buffer_pool, total_size, MADV_HUGEPAGE);
        pool.buffer_pool_has_huge_pages = false;
        
        int err = posix_memalign((void**)&pool.buffer_pool, 4096, total_size);
        assert_true_always(err == 0 && pool.buffer_pool, "posix_memalign failed");
        
        madvise(pool.buffer_pool, total_size, MADV_WILLNEED);
        
        for (int i = 0; i < NUM_BUFFERS; i++) {
            pool.iovecs[i].iov_base = pool.buffer_pool + (i * BUFFER_SIZE);
            pool.iovecs[i].iov_len = BUFFER_SIZE;
        }

        err = io_uring_register_buffers(&pool.ring, pool.iovecs, NUM_BUFFERS);
        assert_true_always(err == 0, "io_uring_register_buffers failed");
    }

    void init_epoll(Pool& pool) {
        pool.epoll_fd = epoll_create1(0);

        assert_true(pool.epoll_fd != -1, "failed to create epoll");

        // add the signal pipe to epoll to monitor for signal events
        // @note relies on signal handler submitting write to signal pipe
        {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = pool.signal_fd;
            int res = epoll_ctl(pool.epoll_fd, EPOLL_CTL_ADD, pool.signal_fd, &ev);
            assert_true(res != -1, "epoll_ctl failed to add signal fd");
        }

        // add the listen_fd to epoll to monitor for incoming connections
        {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = pool.ring.ring_fd;
            int res = epoll_ctl(pool.epoll_fd, EPOLL_CTL_ADD, pool.ring.ring_fd, &ev);
            assert_true(res != -1, "epoll_ctl failed to add ring fd");
        }
    }

    // ========================================================================
    // connection management
    //  - submit our accept and close requests
    //  - open new connection for accept events and read
    //  - record closed connections  
    // ========================================================================
    void submit_accept(Pool& pool) {
        io_uring_sqe* sqe = io_uring_get_sqe(&pool.ring);
        if (sqe == nullptr) {
            return;
        }
        
        io_uring_prep_accept(sqe, pool.listen_fd, NULL, NULL, 0);
        
        {
            U64 user_data = TYPE_ACCEPT;
            io_uring_sqe_set_data(sqe, (void*)user_data);
        }
    }

    void submit_multishot_accept(Pool& pool) {
        io_uring_sqe* sqe = io_uring_get_sqe(&pool.ring);
        if (sqe == nullptr) {
            return;
        }
        
        io_uring_prep_multishot_accept(sqe, pool.listen_fd, NULL, NULL, 0);

        {
            U64 user_data = TYPE_MULTISHOT_ACCEPT;
            io_uring_sqe_set_data(sqe, (void*)user_data);
        }
    }

    template<bool multishot>
    static inline void handle_accept_or_multishot(Pool& pool, Stats& stats, io_uring_cqe* cqe) {
        int client_fd = cqe->res;
        
        // @note no client socket was created so connection does not need to be closed
        if (unlikely(client_fd < 0)) {
            int errno_ = -client_fd;

            // retry
            if (errno_ == EAGAIN || errno_ == EINTR) {
                if constexpr (!multishot) {
                    submit_accept(pool);
                }
                return;
            }

            // @todo
            assert_not_implemented("non retry errors are not implemented");
            return;
        }

        if constexpr (!multishot) {
            // re-arm to accept future connections ASAP
            submit_accept(pool);
        }
        
        set_tcp_socket_options(client_fd);
        
        // track connection
        {
            PoolConnection& connection = insert(pool.connections, client_fd);
            connection.fd = client_fd;
            connection.active = true;
    
            stats.total_connections++;
            stats.active_connections++;
        }
        
        // read first packet from client
        submit_read(pool, client_fd);
    }

    void handle_accept(Pool& pool, Stats& stats, io_uring_cqe* cqe) {
        handle_accept_or_multishot<false>(pool, stats, cqe);
    }

    void handle_multishot_accept(Pool& pool, Stats& stats, io_uring_cqe* cqe) {
        handle_accept_or_multishot<true>(pool, stats, cqe);
    }
    
    void submit_close(Pool& pool, int fd) {
        io_uring_sqe* sqe = io_uring_get_sqe(&pool.ring);
        if (sqe == nullptr) {
            close(fd);
            return;
        }
        
        io_uring_prep_close(sqe, fd);
        
        {
            U64 user_data = TYPE_CLOSE | (U64)fd;
            io_uring_sqe_set_data(sqe, (void*)user_data);
        }
    }
    
    // ========================================================================
    // io
    // ========================================================================
    void submit_read(Pool& pool, int fd) {
        int buffer_idx = alloc_buffer(pool);

        // could not find a free buffer, close the connection
        // @todo log dropped connection
        if (buffer_idx < 0) {
            submit_close(pool, fd);
            return;
        }
        
        io_uring_sqe* sqe = io_uring_get_sqe(&pool.ring);

        // could not get new sqe, release the acquired buffer
        if (sqe == nullptr) {
            pool.buffer_in_use[buffer_idx] = false;
            submit_close(pool, fd);
            return;
        }
        
        // track the request
        {
            PoolRequest& request = pool.request_pool[buffer_idx];
            request.buffer_idx = buffer_idx;
            request.fd = fd;
            request.len = 0;
        }
        
        // set sqe to read into pre-registered buffer
        io_uring_prep_read_fixed(
            sqe, fd,
            pool.iovecs[buffer_idx].iov_base, BUFFER_SIZE, 0, buffer_idx
        );

        {
            U64 user_data = TYPE_READ | (U64)buffer_idx;
            io_uring_sqe_set_data(sqe, (void*)user_data);
        }
    }
    
    void submit_write(Pool& pool, int fd, int buffer_idx, size_t len) {
        io_uring_sqe* sqe = io_uring_get_sqe(&pool.ring);
        if (sqe == nullptr) {
            pool.buffer_in_use[buffer_idx] = false;
            return;
        }
        
        PoolRequest& request = pool.request_pool[buffer_idx];
        request.buffer_idx = buffer_idx;
        request.fd = fd;
        request.len = len;
        
        io_uring_prep_write_fixed(
            sqe, fd, pool.iovecs[buffer_idx].iov_base,
            len, 0, buffer_idx
        );
        
        U64 user_data = TYPE_WRITE | (U64)buffer_idx;
        io_uring_sqe_set_data(sqe, (void*)user_data);
    }

    void handle_write(Pool& pool, Stats& stats, io_uring_cqe* cqe, int buffer_idx) {
        PoolRequest& request = pool.request_pool[buffer_idx];
        int fd = request.fd;
        
        pool.buffer_in_use[buffer_idx] = false;
        
        if (cqe->res < 0) {
            submit_close(pool, fd);
            return;
        }
        
        int bytes_written = cqe->res;
        stats.total_bytes_written += bytes_written;
    }

    // ========================================================================
    // http
    // ========================================================================
    static void return_raw_text(Request& req, String8 msg) {
        Pool& pool = *reinterpret_cast<Pool*>(req.context);
        int fd = req.id;

        for (int offset = 0; offset < msg.length; offset += BUFFER_SIZE) {
            int length = min((int)msg.length - offset, BUFFER_SIZE);
            
            int buffer_idx = alloc_buffer(pool);
            if (buffer_idx < 0) {
                // @todo buffer starvation
                submit_close(pool, fd);
                return;
            }
            
            os::memory_copy(pool.iovecs[buffer_idx].iov_base, &msg.data[offset], length);
            submit_write(pool, fd, buffer_idx, length);
        }
    }

    void return_http_success(Request& req, const String8& body) {
        return_raw_text(req, fmt(
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: %d\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "%s",
            static_cast<int>(body.length),
            body.c_str()
        ));
    }

    void return_http_fail(Request& req, int status, const String8& body, bool close) {
        auto get_reason_for_status = [](int code) -> const char* {
            switch (code) {
                case 200: return "OK";
                case 400: return "Bad Request";
                case 401: return "Unauthorized";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 500: return "Internal Server Error";
                case 501: return "Not Implemented";
                case 502: return "Bad Gateway";
                case 503: return "Service Unavailable";
                default:  return "Unknown";
            }
        };

        const char* connection_open = "";
        const char* connection_close = "Connection: close\r\n";

        return_raw_text(req, fmt(
            "HTTP/1.1 %d %s\r\n"
            "Content-Length: %d\r\n"
            "Content-Type: text/plain\r\n"
            "%s"
            "\r\n"
            "%s",
            status,
            get_reason_for_status(status),
            static_cast<int>(body.length),
            (close ? connection_close : connection_open),
            body.c_str()
        ));
    }

    // ========================================================================
    // printing
    // ========================================================================
    AutoString8 to_str(const Stats& s) {
        AutoString8 res = "=== Stats ===\n"_as;
        res += "Total connections: " + plexdb::to_str(s.total_connections) + "\n";
        res += "Active connections: " + plexdb::to_str(s.active_connections) + "\n";
        res += "Total bytes read: " + plexdb::to_str(s.total_bytes_read) + "(" + plexdb::to_str((F64)s.total_bytes_read / 1_mb) + "MB)\n";
        res += "Total bytes written: " + plexdb::to_str(s.total_bytes_written) + "(" + plexdb::to_str((F64)s.total_bytes_written / 1_mb) + "MB)\n";
        res += "=============";
        return res;
    }
}