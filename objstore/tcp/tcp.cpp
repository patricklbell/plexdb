module;
#include "macros.h"

#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdlib.h>

module objstore.tcp;

namespace objstore::tcp {
    Pool::Pool(int port): free_buffer_idx(0) {
        init_socket(*this, port);
        init_uring(*this);
        init_buffers(*this);
    }
    
    Pool::~Pool() {
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
        
        // @note no client socket was created so conenction does not need to be closed
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
        handle_accept_or_multishot<false>(pool, stats, cqe);
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

    void handle_close(Pool& pool, Stats& stats, io_uring_cqe* cqe, int fd) {
        // close completed (or failed, but fd is closed anyway)
        if (try_remove(pool.connections, fd)) {
            stats.active_connections--;
        }
    }
    
    // ========================================================================
    // io
    // ========================================================================
    void submit_write(const Request& req) {
        Pool& pool = *reinterpret_cast<Pool*>(req.context);
        PoolRequest& request = pool.request_pool[req.id];
        
        submit_write(pool, request.fd, request.buffer_idx, req.inout_data.prefix);
    }

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
        #if PLEXDB_DEBUG
        request.has_responded = true;
        #endif
        
        // Use fixed buffer
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
        
        // go back to reading
        // @todo support large writes
        submit_read(pool, fd);
    }

    // ========================================================================
    // printing
    // ========================================================================
    AutoString8 to_str(const objstore::tcp::Stats& s) {
        AutoString8 res = "=== Stats ===\n"_as;
        res += "Total connections: " + plexdb::to_str(s.total_connections) + "\n";
        res += "Active connections: " + plexdb::to_str(s.active_connections) + "\n";
        res += "Total bytes read: " + plexdb::to_str(s.total_bytes_read) + "(" + plexdb::to_str((F64)s.total_bytes_read / 1_mb) + "MB)\n";
        res += "Total bytes written: " + plexdb::to_str(s.total_bytes_written) + "(" + plexdb::to_str((F64)s.total_bytes_written / 1_mb) + "MB)\n";
        res += "=============";
        return res;
    }
}