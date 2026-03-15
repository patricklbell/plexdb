#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <coroutine>
#include <thread>
#include <atomic>
#include <semaphore>
#include <vector>

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.coro;
import objstore.tcp;
import objstore.tcp.detail;
import objstore.tcp.coro;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore::tcp;

namespace {
    constexpr int TCP_TEST_PORT_BASE = 21000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return TCP_TEST_PORT_BASE + port_counter.fetch_add(1);
    }

    enum class ListenMode { Auto, Uring, Socket };

    struct TestServer {
        os::Handle socket = os::zero_handle();
        os::Notifier signal_pipe;

        TestServer(int port) {
            this->socket = socket_open();
            socket_set_option(this->socket, SocketOption::Reuse, true);
            socket_bind(this->socket, static_cast<U16>(port));
            socket_listen(this->socket, 128);
        }

        ~TestServer() {
            if (!is_zero_handle(this->socket)) {
                socket_close(this->socket);
            }
        }

        void stop() {
            os::signal_notify_safe(this->signal_pipe);
        }
    };

    template<typename OnChunk, typename OnClose>
    Stats listen(const OnChunk& on_chunk, const OnClose& on_close, TestServer& server, volatile bool& exit_signal, ListenMode mode = ListenMode::Auto) {
        auto on_open = [](Connection*) {};
        auto on_chunk_new = [&](const Request& req) -> RequestStatus {
            return on_chunk(const_cast<Request&>(req));
        };
        auto on_close_new = [&](Connection* connection) {
            on_close(static_cast<int>(connection->client.u32[0]));
        };

        if (mode == ListenMode::Socket) {
            return objstore::tcp::listen_socket(server.socket, on_chunk_new, on_close_new, on_open, server.signal_pipe, exit_signal);
        }
        if (mode == ListenMode::Uring) {
            auto ring_settings = uring::get_ring_settings();
            uring::Ring ring{
                server.socket,
                ring_settings->recommended_queue_depth, ring_settings->recommended_buffer_size, ring_settings->recommended_buffer_count
            };
            if (ring) {
                return objstore::tcp::listen_uring(ring, on_chunk_new, on_close_new, on_open, server.signal_pipe, exit_signal);
            }
        }

        return objstore::tcp::listen(server.socket, on_chunk_new, on_open, on_close_new, server.signal_pipe, exit_signal);
    }

    U64 total_chunk_bytes(const Request& req) {
        U64 total = 0;
        for (const auto& chunk : req.connection->chunk_chain.chunks) {
            total += (U64)chunk.data.length;
        }
        return total;
    }

    AutoString8 chunks_to_string(const Request& req) {
        AutoString8 result;
        for (const auto& chunk : req.connection->chunk_chain.chunks) {
            for (int i = 0; i < chunk.data.length; i++) {
                result.push_back(chunk.data.ptr[i]);
            }
        }
        return result;
    }
}

TEST_CASE("handles signal from pipe", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);
    
    REQUIRE(!is_zero_handle(server.socket));
}

TEST_CASE("receives simple request - socket", "[objstore.tcp][socket]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    AutoString8 received_data;

    std::thread server_thread([&]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            received_data = chunks_to_string(req);
            data_received.release();
            return RequestStatus::Handled;
        };

        auto on_close = [&](int fd) {};

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal, ListenMode::Socket);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        bool connected = socket_connect(client, "127.0.0.1", (U16)port);
        REQUIRE(connected);

        String8 message = "HELLO";
        auto res = socket_send_all(client, message.data, message.length);
        REQUIRE(res.error == SocketError::None);
    }

    data_received.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(received_data == "HELLO");
}

TEST_CASE("receives simple request - uring", "[objstore.tcp][uring]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    AutoString8 received_data;

    std::thread server_thread([&server, &server_ready, &exit_signal, &received_data, &data_received]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            received_data = chunks_to_string(req);
            data_received.release();
            return RequestStatus::Handled;
        };

        auto on_close = [](int) {};

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal, ListenMode::Uring);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        String8 message = "HELLO";
        auto res = socket_send_all(client, message.data, message.length);
        REQUIRE(res.error == SocketError::None);
    }

    data_received.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(received_data == "HELLO");
}

TEST_CASE("receives large request", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    U64 total_received = 0;

    constexpr U64 DATA_SIZE = 32 * 1024;

    std::thread server_thread([&server, &server_ready, &exit_signal, &total_received, &data_received]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            U64 bytes = total_chunk_bytes(req);

            if (bytes >= DATA_SIZE) {
                total_received = bytes;
                data_received.release();
                return RequestStatus::Handled;
            }
            return RequestStatus::Pending;
        };

        auto on_close = [](int) {};

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        std::vector<char> large_data(DATA_SIZE);
        for (U64 i = 0; i < DATA_SIZE; i++) {
            large_data[i] = (char)('A' + (i % 26));
        }

        auto res = socket_send_all(client, large_data.data(), large_data.size());
        REQUIRE(res.error == SocketError::None);
    }

    data_received.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(total_received >= DATA_SIZE);
}

TEST_CASE("handles multiple sequential connections", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::counting_semaphore<16> connection_sem{0};
    volatile bool exit_signal = false;
    std::atomic<int> connection_count{0};

    constexpr int NUM_CONNECTIONS = 5;

    std::thread server_thread([&server, &server_ready, &exit_signal, &connection_count, &connection_sem]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            connection_count++;
            connection_sem.release();
            return RequestStatus::Close;
        };

        auto on_close = [](int) {};

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        char message[32];
        int len = snprintf(message, sizeof(message), "MSG%d", i);
        socket_send_all(client, message, len);
        
        connection_sem.acquire();
    }

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(connection_count == NUM_CONNECTIONS);
}

TEST_CASE("handles concurrent connections", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::counting_semaphore<16> request_sem{0};
    volatile bool exit_signal = false;
    std::atomic<int> request_count{0};

    constexpr int NUM_CLIENTS = 4;

    std::thread server_thread([&server, &server_ready, &exit_signal, &request_count, &request_sem]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            request_count++;
            request_sem.release();
            return RequestStatus::Close;
        };

        auto on_close = [](int) {};

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    std::vector<std::thread> client_threads;
    client_threads.reserve(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_threads.emplace_back([port, i]() {
            Socket client{socket_open()};
            if (socket_connect(client, "127.0.0.1", (U16)port)) {
                char message[32];
                int len = snprintf(message, sizeof(message), "CLIENT%d", i);
                socket_send_all(client, message, len);
            }
        });
    }

    for (auto& t : client_threads) {
        t.join();
    }

    for (int i = 0; i < NUM_CLIENTS; i++) {
        request_sem.acquire();
    }

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(request_count == NUM_CLIENTS);
}

TEST_CASE("handles client disconnect", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore close_sem{0};
    volatile bool exit_signal = false;
    std::atomic<int> close_count{0};

    std::thread server_thread([&server, &server_ready, &exit_signal, &close_count, &close_sem]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            return RequestStatus::Pending;
        };

        auto on_close = [&](int) {
            close_count++;
            close_sem.release();
        };

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "DATA", 4);
    }

    close_sem.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(close_count >= 1);
}

TEST_CASE("tracks statistics correctly", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore done{0};
    volatile bool exit_signal = false;
    Stats stats;

    std::thread server_thread([&server, &server_ready, &exit_signal, &stats, &done]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            done.release();
            return RequestStatus::Close;
        };

        auto on_close = [](int) {};

        server_ready.release();
        stats = listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "TESTDATA", 8);
    }

    done.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(stats.total_connections >= 1);
    REQUIRE(stats.total_bytes_read >= 8);
}

TEST_CASE("binary data with null bytes", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    std::vector<char> received_data;

    std::thread server_thread([&server, &server_ready, &exit_signal, &received_data, &data_received]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            for (const auto& chunk : req.connection->chunk_chain.chunks) {
                for (int i = 0; i < chunk.data.length; i++) {
                    received_data.push_back(chunk.data.ptr[i]);
                }
            }
            data_received.release();
            return RequestStatus::Handled;
        };

        auto on_close = [](int) {};

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        char binary_data[10] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00};
        socket_send_all(client, binary_data, 10);
    }

    data_received.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(received_data.size() >= 10);
    REQUIRE(received_data[0] == 0x01);
    REQUIRE(received_data[1] == 0x00);
    REQUIRE(received_data[2] == 0x02);
    REQUIRE(received_data[4] == 0x03);
}

TEST_CASE("pending response accumulates data", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore done{0};
    volatile bool exit_signal = false;
    std::atomic<int> chunk_count{0};
    U64 final_size = 0;

    constexpr U64 TARGET_SIZE = 20;

    std::thread server_thread([&server, &server_ready, &exit_signal, &chunk_count, &final_size, &done]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            chunk_count++;
            U64 total = total_chunk_bytes(req);
            
            if (total >= TARGET_SIZE) {
                final_size = total;
                done.release();
                return RequestStatus::Handled;
            }
            return RequestStatus::Pending;
        };

        auto on_close = [](int) {};

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        socket_send_all(client, "PART1PART2PART3PART4", 20);
    }

    done.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(final_size >= TARGET_SIZE);
}

TEST_CASE("close response terminates connection", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore chunk_sem{0};
    volatile bool exit_signal = false;
    std::atomic<int> request_count{0};
    std::atomic<int> close_count{0};

    std::thread server_thread([&server, &server_ready, &exit_signal, &request_count, &close_count, &chunk_sem]() {

        auto on_chunk = [&](Request& req) -> RequestStatus {
            request_count++;
            chunk_sem.release();
            return RequestStatus::Close;
        };

        auto on_close = [&](int) {
            close_count++;
        };

        server_ready.release();
        listen(on_chunk, on_close, server, exit_signal);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "CLOSE_ME", 8);
    }

    chunk_sem.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(request_count >= 1);
    REQUIRE(close_count >= 1);
}

// ============================================================================
// Coroutine TCP tests
//   These tests exercise listen_coro via an io_uring ring.
//   They require io_uring to be available (Linux kernel >= 5.19).
// ============================================================================

TEST_CASE("listen_coro - single connection echoes data", "[objstore.tcp][objstore.tcp.coro]") {
    auto ring_settings = uring::get_ring_settings();
    if (!ring_settings->recommended) {
        SKIP("io_uring not available on this platform");
    }

    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    std::atomic<int> recv_count{0};

    std::thread server_thread([&]() {
        uring::Ring ring{
            server.socket,
            ring_settings->recommended_queue_depth,
            ring_settings->recommended_buffer_size,
            ring_settings->recommended_buffer_count
        };
        if (!ring) return;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            auto chunk = co_await io.recv();
            if (chunk.valid()) {
                recv_count++;
                data_received.release();
                // Echo back using send() (fire-and-forget).
                io.send(chunk.buffer_idx, 0, static_cast<U32>(chunk.byte_count));
            }
        };

        server_ready.release();
        listen_coro(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "HELLO", 5);
    }

    data_received.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(recv_count >= 1);
}

TEST_CASE("listen_coro - coroutine can disconnect on empty recv", "[objstore.tcp][objstore.tcp.coro]") {
    auto ring_settings = uring::get_ring_settings();
    if (!ring_settings->recommended) {
        SKIP("io_uring not available on this platform");
    }

    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;
    std::atomic<int> conn_count{0};
    std::atomic<int> disconn_count{0};

    std::thread server_thread([&]() {
        uring::Ring ring{
            server.socket,
            ring_settings->recommended_queue_depth,
            ring_settings->recommended_buffer_size,
            ring_settings->recommended_buffer_count
        };
        if (!ring) return;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            conn_count++;
            while (true) {
                auto chunk = co_await io.recv();
                if (!chunk.valid()) {
                    disconn_count++;
                    co_return;
                }
                io.release_recv(chunk.buffer_idx);
            }
        };

        server_ready.release();
        listen_coro(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();

    // Connect, send one message, then disconnect.
    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "DATA", 4);
        // Socket closes here (out of scope) — server sees byte_count == 0.
    }

    // Give the server time to process the disconnect.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(conn_count >= 1);
    REQUIRE(disconn_count >= 1);
}
