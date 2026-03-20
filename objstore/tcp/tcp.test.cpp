#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "plexdb_coro.h"
#include <thread>
#include <atomic>
#include <semaphore>
#include <vector>
#include <chrono>

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.coro;
import objstore.tcp;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore::tcp;

namespace {
    constexpr int TCP_TEST_PORT_BASE = 21000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return TCP_TEST_PORT_BASE + port_counter.fetch_add(1);
    }

    struct TestServer {
        os::Handle socket = os::zero_handle();
        os::Notifier signal_pipe;

        TestServer(int port) {
            socket = socket_open();
            socket_set_option(socket, SocketOption::Reuse, true);
            socket_bind(socket, static_cast<U16>(port));
            socket_listen(socket, 128);
        }

        ~TestServer() {
            if (!is_zero_handle(socket))
                socket_close(socket);
        }

        void stop() { os::signal_notify_safe(signal_pipe); }
    };
}

// ============================================================================
// Basic infrastructure test
// ============================================================================
TEST_CASE("handles signal from pipe", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);
    REQUIRE(!is_zero_handle(server.socket));
}

// ============================================================================
// Coroutine-based tests
// All tests require io_uring.  A shared atomic bool is used to signal whether
// the ring was successfully created, avoiding reliance on get_ring_settings()
// (which changes after Ring creation depletes available buffer memory).
// ============================================================================

TEST_CASE("receives simple request", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    AutoString8 received_data;

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            auto chunk = co_await io.recv();
            if (chunk.valid()) {
                for (int i = 0; i < chunk.byte_count; i++)
                    received_data.push_back(static_cast<char>(chunk.data[i]));
                data_received.release();
            }
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return; // io_uring not available
    }

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "HELLO", 5);
    }

    data_received.acquire();
    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(received_data == "HELLO");
}

TEST_CASE("receives large request", "[objstore.tcp]") {
    constexpr U64 DATA_SIZE = 32 * 1024;

    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::atomic<U64> total_received{0};

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            U64 accumulated = 0;
            while (accumulated < DATA_SIZE) {
                auto chunk = co_await io.recv();
                if (!chunk.valid()) co_return;
                accumulated += chunk.byte_count;
                io.release_recv(chunk.buffer_idx);
            }
            total_received = accumulated;
            data_received.release();
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        std::vector<char> large_data(DATA_SIZE);
        for (U64 i = 0; i < DATA_SIZE; i++) large_data[i] = (char)('A' + (i % 26));
        REQUIRE(socket_send_all(client, large_data.data(), large_data.size()).error == SocketError::None);
    }

    data_received.acquire();
    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(total_received >= DATA_SIZE);
}

TEST_CASE("handles multiple sequential connections", "[objstore.tcp]") {
    constexpr int NUM_CONNECTIONS = 5;

    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::counting_semaphore<16> conn_sem{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::atomic<int> conn_count{0};

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            auto chunk = co_await io.recv();
            if (chunk.valid()) {
                conn_count++;
                conn_sem.release();
                io.release_recv(chunk.buffer_idx);
            }
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        char msg[32];
        int len = snprintf(msg, sizeof(msg), "MSG%d", i);
        socket_send_all(client, msg, len);
        conn_sem.acquire();
    }

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(conn_count == NUM_CONNECTIONS);
}

TEST_CASE("handles concurrent connections", "[objstore.tcp]") {
    constexpr int NUM_CLIENTS = 4;

    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::counting_semaphore<16> req_sem{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::atomic<int> req_count{0};

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            auto chunk = co_await io.recv();
            if (chunk.valid()) {
                req_count++;
                req_sem.release();
                io.release_recv(chunk.buffer_idx);
            }
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

    std::vector<std::thread> client_threads;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_threads.emplace_back([port, i]() {
            Socket client{socket_open()};
            if (socket_connect(client, "127.0.0.1", (U16)port)) {
                char msg[32];
                int len = snprintf(msg, sizeof(msg), "CLIENT%d", i);
                socket_send_all(client, msg, len);
            }
        });
    }
    for (auto& t : client_threads) t.join();
    for (int i = 0; i < NUM_CLIENTS; i++) req_sem.acquire();

    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(req_count == NUM_CLIENTS);
}

TEST_CASE("handles client disconnect", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore close_sem{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::atomic<int> disconn_count{0};

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            while (true) {
                auto chunk = co_await io.recv();
                if (!chunk.valid()) {
                    disconn_count++;
                    close_sem.release();
                    co_return;
                }
                io.release_recv(chunk.buffer_idx);
            }
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "DATA", 4);
    }

    close_sem.acquire();
    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(disconn_count >= 1);
}

TEST_CASE("tracks statistics correctly", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore done{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    Stats stats;

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            auto chunk = co_await io.recv();
            if (chunk.valid()) {
                done.release();
                io.release_recv(chunk.buffer_idx);
            }
        };

        server_ready.release();
        stats = listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

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
}

TEST_CASE("binary data passes through", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::vector<U8> received_data;

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            auto chunk = co_await io.recv();
            if (chunk.valid()) {
                for (int i = 0; i < chunk.byte_count; i++)
                    received_data.push_back(chunk.data[i]);
                data_received.release();
                io.release_recv(chunk.buffer_idx);
            }
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

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
}

TEST_CASE("coroutine accumulates data across recvs", "[objstore.tcp]") {
    constexpr U64 TARGET_SIZE = 20;

    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore done{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::atomic<U64> final_size{0};

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            U64 total = 0;
            while (total < TARGET_SIZE) {
                auto chunk = co_await io.recv();
                if (!chunk.valid()) co_return;
                total += chunk.byte_count;
                io.release_recv(chunk.buffer_idx);
            }
            final_size = total;
            done.release();
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

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

TEST_CASE("single connection echoes data", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore data_received{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::atomic<int> recv_count{0};

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            auto chunk = co_await io.recv();
            if (chunk.valid()) {
                recv_count++;
                data_received.release();
                io.send(chunk.buffer_idx, 0, static_cast<U32>(chunk.byte_count));
            }
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

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

TEST_CASE("coroutine detects client disconnect via empty recv", "[objstore.tcp]") {
    int port = get_unique_port();
    TestServer server(port);

    std::binary_semaphore server_ready{0};
    std::binary_semaphore disconn_sem{0};
    volatile bool exit_signal = false;
    std::atomic<bool> ring_ok{false};
    std::atomic<int> conn_count{0};
    std::atomic<int> disconn_count{0};

    std::thread server_thread([&]() {
        auto* s = uring::get_ring_settings();
        if (!s->recommended) { server_ready.release(); return; }
        uring::Ring ring{server.socket,
            s->recommended_queue_depth,
            s->recommended_buffer_size,
            s->recommended_buffer_count};
        if (!ring) { server_ready.release(); return; }
        ring_ok = true;

        auto handler = [&](CoroConnectionIO& io) -> coro::Task {
            conn_count++;
            while (true) {
                auto chunk = co_await io.recv();
                if (!chunk.valid()) {
                    disconn_count++;
                    disconn_sem.release();
                    co_return;
                }
                io.release_recv(chunk.buffer_idx);
            }
        };

        server_ready.release();
        listen(ring, handler, server.signal_pipe, exit_signal);
    });

    server_ready.acquire();
    if (!ring_ok) {
        server_thread.join();
        return;
    }

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "DATA", 4);
    }

    disconn_sem.acquire();
    exit_signal = true;
    server.stop();
    server_thread.join();

    REQUIRE(conn_count >= 1);
    REQUIRE(disconn_count >= 1);
}
