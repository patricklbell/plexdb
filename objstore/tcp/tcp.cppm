module;
#include "macros.h"

#include <netinet/in.h>

export module objstore.tcp;

import plexdb.base;
import plexdb.arena;
import plexdb.os;

using namespace plexdb;

namespace objstore::tcp {
    // @todo user/auto config
    constexpr int QUEUE_DEPTH = 4096;
    constexpr int BUFFER_SIZE = 4096;
    // constexpr int NUM_BUFFERS = 8192;
    constexpr int NUM_BUFFERS = 1000; // @todo
    constexpr int MAX_CQE_BATCH = 256;
    export constexpr int MAX_CONNECTIONS = 10000;
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
        os::EventLoop event_loop;
        
        char* buffer_pool;
        bool buffer_pool_has_huge_pages;
        bool buffer_in_use[NUM_BUFFERS];
        size_t free_buffer_idx;
        
        os::IoBuffer io_buffers[NUM_BUFFERS];
        PoolRequest request_pool[NUM_BUFFERS];
        
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
        Handled,
        Pending,
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
    void init_buffers(Pool& pool);

    // ========================================================================
    // buffer chain management
    // ========================================================================
    bool append_to_buffer_chain(BufferChain& chain, int buffer_idx, char* buffer_data, int bytes_read);
    void release_chain(Pool& pool, BufferChain& chain);

    // ========================================================================
    // connection management
    // ========================================================================
    void submit_accept(Pool& pool);
    void submit_multishot_accept(Pool& pool);
    void handle_accept(Pool& pool, Stats& stats, int result, U64 user_data);
    void handle_multishot_accept(Pool& pool, Stats& stats, int result, U64 user_data);
    void submit_close(Pool& pool, int fd);

    void handle_close(const OnClose auto& on_close_callback, Pool& pool, Stats& stats, int result, int fd) {
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
    void handle_write(Pool& pool, Stats& stats, int result, int buffer_idx);

    void handle_read(const OnChunk auto& on_chunk_callback, Pool& pool, Stats& stats, int result, int buffer_idx) {
        PoolRequest& request = pool.request_pool[buffer_idx];
        int fd = request.fd;
        
        if (result <= 0) {
            pool.buffer_in_use[buffer_idx] = false;
            if (try_remove(pool.connections, fd))
                stats.active_connections--;
            submit_close(pool, fd);
            return;
        }
        
        int bytes_read = result;
        stats.total_bytes_read += bytes_read;
        request.len += bytes_read;

        PoolConnection* conn = find(pool.connections, fd);
        assert_true(conn != nullptr, "connection not found for read, this should never happen");
        BufferChain& chain = conn->rx_chain;

        {
            assert_true(buffer_idx >= 0 && buffer_idx < NUM_BUFFERS, "read buffer idx out of range, this should never happen");
            assert_true(bytes_read >= 0 && bytes_read <= BUFFER_SIZE, "buffer overflow in cqe, this should never happen");
            
            char* buffer_data = &pool.buffer_pool[BUFFER_SIZE * buffer_idx];
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
    void handle_event(const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, Pool& pool, Stats& stats, os::CompletionEvent& event) {
        U64 user_data = event.user_data;
        EventType type = (EventType)(user_data & TYPE_MASK);
        int data = (int)(user_data & DATA_MASK);
        
        switch (type) {
            case TYPE_ACCEPT:{
                handle_accept(pool, stats, event.result, user_data);
            }break;
            case TYPE_READ:{
                handle_read(on_chunk_callback, pool, stats, event.result, data);
            }break;
            case TYPE_WRITE:{
                handle_write(pool, stats, event.result, data);
            }break;
            case TYPE_CLOSE:{
                handle_close(on_close_callback, pool, stats, event.result, data);
            }break;
            case TYPE_MULTISHOT_ACCEPT:{
                handle_multishot_accept(pool, stats, event.result, user_data);
            }break;

            case TYPE_MASK:{}break;
            case DATA_MASK:{}break;
        }
    }

    void drain_or_block(const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, Pool& pool, Stats& stats, volatile bool& exit_signal) {
        os::CompletionEvent events[MAX_CQE_BATCH];
        int count = os::event_loop_drain(pool.event_loop, events, MAX_CQE_BATCH);
        
        if (count > 0) {
            for (int i = 0; i < count; i++) {
                handle_event(on_chunk_callback, on_close_callback, pool, stats, events[i]);
            }
        } else {
            os::event_loop_wait(pool.event_loop);
        }
    }

    export void listen(const OnChunk auto& on_chunk_callback, const OnClose auto& on_close_callback, Pool& pool, Stats& stats, volatile bool& exit_signal) {
        submit_multishot_accept(pool);
        
        while (!exit_signal) {
            os::event_loop_flush(pool.event_loop);
            drain_or_block(on_chunk_callback, on_close_callback, pool, stats, exit_signal);
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
