module;
#include "macros.h"

#include <stdlib.h>
#include <errno.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

module objstore.tcp;

import plexdb.os;
import plexdb.arena;

namespace objstore::tcp {
    Pool::Pool(int port, int signal_fd): free_buffer_idx(0)  {
        init_socket(*this, port);
        this->event_loop = os::event_loop_create(QUEUE_DEPTH, signal_fd);
        init_buffers(*this);
    }
    
    Pool::~Pool() {
        for (auto& kv : this->connections) {
            release_chain(*this, kv.second.rx_chain);
        }
        
        os::event_loop_unregister_buffers(this->event_loop);
        os::event_loop_destroy(this->event_loop);
        free(this->buffer_pool);
        int err = close(this->listen_fd);
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
    // ========================================================================
    void init_socket(Pool& pool, int port) {
        pool.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        assert_true_always(pool.listen_fd >= 0, "failed to open socket for pool");
        
        int opt = 1;
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        
        set_tcp_socket_options(pool.listen_fd);
        
        int bufsize = 16 * 1024 * 1024;
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        setsockopt(pool.listen_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        
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
    
    void init_buffers(Pool& pool) {
        size_t total_size = BUFFER_SIZE * NUM_BUFFERS;
        
        pool.buffer_pool_has_huge_pages = false;
        
        int err = posix_memalign((void**)&pool.buffer_pool, 4096, total_size);
        assert_true_always(err == 0 && pool.buffer_pool, "posix_memalign failed");
        
        madvise(pool.buffer_pool, total_size, MADV_WILLNEED);
        
        for (int i = 0; i < NUM_BUFFERS; i++) {
            pool.io_buffers[i].base = pool.buffer_pool + (i * BUFFER_SIZE);
            pool.io_buffers[i].length = BUFFER_SIZE;
        }

        bool ok = os::event_loop_register_buffers(pool.event_loop, pool.io_buffers, NUM_BUFFERS);
        assert_true_always(ok, "event_loop_register_buffers failed");
    }

    // ========================================================================
    // connection management
    // ========================================================================
    void submit_accept(Pool& pool) {
        U64 user_data = TYPE_ACCEPT;
        os::event_loop_submit_accept(pool.event_loop, pool.listen_fd, user_data);
    }

    void submit_multishot_accept(Pool& pool) {
        U64 user_data = TYPE_MULTISHOT_ACCEPT;
        os::event_loop_submit_multishot_accept(pool.event_loop, pool.listen_fd, user_data);
    }

    template<bool multishot>
    static inline void handle_accept_or_multishot(Pool& pool, Stats& stats, int result, U64 user_data) {
        int client_fd = result;
        
        // @note no client socket was created so connection does not need to be closed
        if (unlikely(client_fd < 0)) {
            int errno_ = -client_fd;

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
            submit_accept(pool);
        }
        
        set_tcp_socket_options(client_fd);
        
        {
            PoolConnection& connection = insert(pool.connections, client_fd);
            connection.fd = client_fd;
            connection.active = true;
    
            stats.total_connections++;
            stats.active_connections++;
        }
        
        submit_read(pool, client_fd);
    }

    void handle_accept(Pool& pool, Stats& stats, int result, U64 user_data) {
        handle_accept_or_multishot<false>(pool, stats, result, user_data);
    }

    void handle_multishot_accept(Pool& pool, Stats& stats, int result, U64 user_data) {
        handle_accept_or_multishot<true>(pool, stats, result, user_data);
    }
    
    void submit_close(Pool& pool, int fd) {
        U64 user_data = TYPE_CLOSE | (U64)fd;
        if (!os::event_loop_submit_close(pool.event_loop, fd, user_data)) {
            close(fd);
        }
    }
    
    // ========================================================================
    // io
    // ========================================================================
    void submit_read(Pool& pool, int fd) {
        int buffer_idx = alloc_buffer(pool);

        if (buffer_idx < 0) {
            submit_close(pool, fd);
            return;
        }

        {
            PoolRequest& request = pool.request_pool[buffer_idx];
            request.buffer_idx = buffer_idx;
            request.fd = fd;
            request.len = 0;
        }
        
        U64 user_data = TYPE_READ | (U64)buffer_idx;
        if (!os::event_loop_submit_read(pool.event_loop, fd, pool.io_buffers[buffer_idx].base, BUFFER_SIZE, buffer_idx, user_data)) {
            pool.buffer_in_use[buffer_idx] = false;
            submit_close(pool, fd);
        }
    }
    
    void submit_write(Pool& pool, int fd, int buffer_idx, size_t len) {
        PoolRequest& request = pool.request_pool[buffer_idx];
        request.buffer_idx = buffer_idx;
        request.fd = fd;
        request.len = len;
        
        U64 user_data = TYPE_WRITE | (U64)buffer_idx;
        if (!os::event_loop_submit_write(pool.event_loop, fd, pool.io_buffers[buffer_idx].base, len, buffer_idx, user_data)) {
            pool.buffer_in_use[buffer_idx] = false;
        }
    }

    void handle_write(Pool& pool, Stats& stats, int result, int buffer_idx) {
        PoolRequest& request = pool.request_pool[buffer_idx];
        int fd = request.fd;
        
        pool.buffer_in_use[buffer_idx] = false;
        
        if (result < 0) {
            submit_close(pool, fd);
            return;
        }
        
        int bytes_written = result;
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
                submit_close(pool, fd);
                return;
            }
            
            os::memory_copy(pool.io_buffers[buffer_idx].base, &msg.data[offset], length);
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
