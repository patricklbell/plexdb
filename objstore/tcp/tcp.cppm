module;
#include "macros.h"

#include <liburing.h>
#include <netinet/in.h>

export module objstore.tcp;

import plexdb.base;

using namespace plexdb;

namespace objstore::tcp {
    // @todo user/auto config
    constexpr int QUEUE_DEPTH = 4096;
    constexpr int BUFFER_SIZE = 4096;
    // constexpr int NUM_BUFFERS = 8192;
    constexpr int NUM_BUFFERS = 2; // @todo
    constexpr int MAX_CQE_BATCH = 256;
    constexpr int MAX_CONNECTIONS = 10000;
    constexpr int INITAL_ACCEPTS = min(32, MAX_CONNECTIONS);
    
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

        #if PLEXDB_DEBUG
        bool has_responded = false;
        #endif
    };

    struct PoolConnection {
        int fd           = 0;
        sockaddr_in addr = {0};
        bool active      = 0;
    };

    export struct Pool {
        int listen_fd;
        
        char* buffer_pool;
        bool buffer_pool_has_huge_pages;
        bool buffer_in_use[NUM_BUFFERS];
        size_t free_buffer_idx;
        
        io_uring ring;
        iovec iovecs[NUM_BUFFERS];
        PoolRequest request_pool[NUM_BUFFERS];
        
        MapFixedSentinel<int, PoolConnection, 2*MAX_CONNECTIONS> connections;

        Pool(int port);
        ~Pool();
    };

    export struct Stats {
        U64 total_connections   = 0;
        U64 active_connections  = 0;
        U64 total_bytes_read    = 0;
        U64 total_bytes_written = 0;
    };

    export struct Request {
        void* context;
        int id;

        TArrayViewPrefix<char,int> inout_data;
    };

    template<typename F>
    concept IsCallback = requires(F f, Request& req) {
        f(req);
    };

    export void submit_write(const Request& req);

    // ========================================================================
    // init helpers
    // ========================================================================
    void init_socket(Pool& pool, int port);
    void init_uring(Pool& pool);
    void init_buffers(Pool& pool);

    // ========================================================================
    // connection management
    // ========================================================================
    void submit_accept(Pool& pool);
    void submit_multishot_accept(Pool& pool);
    void handle_accept(Pool& pool, Stats& stats, io_uring_cqe* cqe);
    void handle_multishot_accept(Pool& pool, Stats& stats, io_uring_cqe* cqe);
    void submit_close(Pool& pool, int fd);
    void handle_close(Pool& pool, Stats& stats, io_uring_cqe* cqe, int fd);

    // ========================================================================
    // io
    // ========================================================================
    void submit_write(Pool& pool, int fd, int buffer_idx, size_t len);
    void submit_read(Pool& pool, int fd);
    void handle_write(Pool& pool, Stats& stats, io_uring_cqe* cqe, int buffer_idx);
    
    template<typename F>
        requires IsCallback<F>
    void handle_read(F& callback, Pool& pool, Stats& stats, io_uring_cqe* cqe, int buffer_idx) {
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

        {
            assert_true(buffer_idx >= 0 && buffer_idx < NUM_BUFFERS, "read buffer idx out of range");
            assert_true(bytes_read >= 0 && bytes_read <= BUFFER_SIZE, "buffer overflow");
            
            Request req{
                .context = &pool,
                .id = buffer_idx,
                .inout_data = {
                    &pool.buffer_pool[BUFFER_SIZE*buffer_idx],
                    BUFFER_SIZE,
                    bytes_read
                },
            };
            callback(req);
        }

        #if PLEXDB_DEBUG
        assert_true(request.has_responded, "callback failed to respond to request");
        #endif
    }

    // ========================================================================
    // listen
    // ========================================================================
    template<typename F>
        requires IsCallback<F>
    void handle_cqe(F& callback, Pool& pool, Stats& stats, io_uring_cqe* cqe) {
        U64 user_data = (U64)io_uring_cqe_get_data(cqe);
        EventType type = (EventType)(user_data & TYPE_MASK);
        int data = (int)(user_data & DATA_MASK);
        
        switch (type) {
            case TYPE_ACCEPT:{
                handle_accept(pool, stats, cqe);
            }break;
            case TYPE_READ:{
                handle_read(callback, pool, stats, cqe, data);
            }break;
            case TYPE_WRITE:{
                handle_write(pool, stats, cqe, data);
            }break;
            case TYPE_CLOSE:{
                handle_close(pool, stats, cqe, data);
            }break;
            case TYPE_MULTISHOT_ACCEPT:{
                handle_multishot_accept(pool, stats, cqe);
            }break;

            case TYPE_MASK:{}break;
            case DATA_MASK:{}break;
        }
    }

    template<typename F>
        requires IsCallback<F>
    void drain_or_block_cqes(F& callback, Pool& pool, Stats& stats) {
        io_uring_cqe* cqe;
        unsigned head;
        unsigned count = 0;
        
        io_uring_for_each_cqe(&pool.ring, head, cqe) {
            handle_cqe(callback, pool, stats, cqe);
            count++;
            
            if (count >= MAX_CQE_BATCH) {
                break;
            }
        }
        
        if (count > 0) {
            io_uring_cq_advance(&pool.ring, count);
        } else {
            int ret = io_uring_wait_cqe(&pool.ring, &cqe);
            if (ret == 0) {
                handle_cqe(callback, pool, stats, cqe);
                io_uring_cqe_seen(&pool.ring, cqe);
            }
        }
    }

    export template<typename F>
        requires IsCallback<F>
    void listen(F& callback, Pool& pool, Stats& stats) {
        submit_multishot_accept(pool);
        
        while (true) {
            io_uring_submit(&pool.ring);
            drain_or_block_cqes(callback, pool, stats);
        }
    }

    // ========================================================================
    // printing
    // ========================================================================
    export AutoString8 to_str(const objstore::tcp::Stats& s);
}