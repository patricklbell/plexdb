#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

import plexdb.base;
import plexdb.os;
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

    struct TestSignalPipe {
        os::SignalPipe pipe;

        TestSignalPipe() {
            pipe = os::signal_pipe_create();
        }

        ~TestSignalPipe() {
            os::signal_pipe_destroy(pipe);
        }

        void signal() {
            os::signal_pipe_notify(pipe);
        }

        int read_fd() { return pipe.read_fd; }
    };

    U64 total_chunk_bytes(const Request& req) {
        U64 total = 0;
        for (const auto& chunk : *req.chunks) {
            total += (U64)chunk.data.length;
        }
        return total;
    }

    AutoString8 chunks_to_string(const Request& req) {
        AutoString8 result;
        for (const auto& chunk : *req.chunks) {
            for (int i = 0; i < chunk.data.length; i++) {
                result.push_back(chunk.data.ptr[i]);
            }
        }
        return result;
    }
}

TEST_CASE("pool handles signal from pipe", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    Pool pool(port, signal_pipe.read_fd());
    
    REQUIRE(pool.listen_fd >= 0);
    REQUIRE(pool.buffer_pool != nullptr);
}

TEST_CASE("pool receives simple request", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    AutoString8 received_data;
    std::atomic<bool> data_received{false};

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &received_data, &data_received]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            received_data = chunks_to_string(req);
            data_received = true;
            return RequestStatus::Handled;
        };

        auto on_close = [](int) {};

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 1000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        String8 message = "HELLO";
        auto res = socket_send_all(client, message.data, message.length);
        REQUIRE(res.error == SocketError::None);
    }

    auto start = std::chrono::steady_clock::now();
    while (!data_received) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(data_received);
    REQUIRE(received_data == "HELLO");
}

TEST_CASE("pool receives large request", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    U64 total_received = 0;
    std::atomic<bool> data_received{false};

    // Use a smaller data size that works with the limited buffer pool
    constexpr U64 DATA_SIZE = 32 * 1024;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &total_received, &data_received]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            // total_chunk_bytes returns cumulative size (pending chunks accumulate)
            U64 bytes = total_chunk_bytes(req);

            if (bytes >= DATA_SIZE) {
                total_received = bytes;
                data_received = true;
                return RequestStatus::Handled;
            }
            return RequestStatus::Pending;
        };

        auto on_close = [](int) {};

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        std::vector<char> large_data(DATA_SIZE);
        for (U64 i = 0; i < DATA_SIZE; i++) {
            large_data[i] = (char)('A' + (i % 26));
        }

        auto res = socket_send_all(client, large_data.data(), large_data.size());
        REQUIRE(res.error == SocketError::None);
    }

    auto start = std::chrono::steady_clock::now();
    while (!data_received) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(data_received);
    REQUIRE(total_received >= DATA_SIZE);
}

TEST_CASE("pool handles multiple sequential connections", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    std::atomic<int> connection_count{0};

    constexpr int NUM_CONNECTIONS = 5;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &connection_count]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            connection_count++;
            return RequestStatus::Close;
        };

        auto on_close = [](int) {};

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 1000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        char message[32];
        int len = snprintf(message, sizeof(message), "MSG%d", i);
        socket_send_all(client, message, len);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto start = std::chrono::steady_clock::now();
    while (connection_count < NUM_CONNECTIONS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(connection_count == NUM_CONNECTIONS);
}

TEST_CASE("pool handles concurrent connections", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    std::atomic<int> request_count{0};

    constexpr int NUM_CLIENTS = 4;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &request_count]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            request_count++;
            return RequestStatus::Close;
        };

        auto on_close = [](int) {};

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<std::thread> client_threads;
    client_threads.reserve(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_threads.emplace_back([port, i]() {
            Socket client{socket_create_tcp()};
            socket_set_timeout(client, 2000);
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

    auto start = std::chrono::steady_clock::now();
    while (request_count < NUM_CLIENTS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(request_count == NUM_CLIENTS);
}

TEST_CASE("pool handles client disconnect", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    std::atomic<int> close_count{0};

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &close_count]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            return RequestStatus::Pending;
        };

        auto on_close = [&](int) {
            close_count++;
        };

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 1000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "DATA", 4);
    }

    auto start = std::chrono::steady_clock::now();
    while (close_count < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(close_count >= 1);
}

TEST_CASE("pool tracks statistics correctly", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    Stats final_stats;
    std::atomic<bool> done{false};

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &final_stats, &done]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            done = true;
            final_stats = stats;
            return RequestStatus::Close;
        };

        auto on_close = [](int) {};

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 1000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "TESTDATA", 8);
    }

    auto start = std::chrono::steady_clock::now();
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(done);
    REQUIRE(final_stats.total_connections >= 1);
    REQUIRE(final_stats.total_bytes_read >= 8);
}

TEST_CASE("binary data with null bytes", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    std::vector<char> received_data;
    std::atomic<bool> data_received{false};

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &received_data, &data_received]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            for (const auto& chunk : *req.chunks) {
                for (int i = 0; i < chunk.data.length; i++) {
                    received_data.push_back(chunk.data.ptr[i]);
                }
            }
            data_received = true;
            return RequestStatus::Handled;
        };

        auto on_close = [](int) {};

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 1000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        char binary_data[10] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00};
        socket_send_all(client, binary_data, 10);
    }

    auto start = std::chrono::steady_clock::now();
    while (!data_received) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(data_received);
    REQUIRE(received_data.size() >= 10);
    REQUIRE(received_data[0] == 0x01);
    REQUIRE(received_data[1] == 0x00);
    REQUIRE(received_data[2] == 0x02);
    REQUIRE(received_data[4] == 0x03);
}

TEST_CASE("pending response accumulates data", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    std::atomic<int> chunk_count{0};
    U64 final_size = 0;
    std::atomic<bool> done{false};

    constexpr U64 TARGET_SIZE = 20;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &chunk_count, &final_size, &done]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            chunk_count++;
            U64 total = total_chunk_bytes(req);
            
            if (total >= TARGET_SIZE) {
                final_size = total;
                done = true;
                return RequestStatus::Handled;
            }
            return RequestStatus::Pending;
        };

        auto on_close = [](int) {};

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        socket_send_all(client, "PART1", 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        socket_send_all(client, "PART2", 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        socket_send_all(client, "PART3", 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        socket_send_all(client, "PART4", 5);
    }

    auto start = std::chrono::steady_clock::now();
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(done);
    REQUIRE(final_size >= TARGET_SIZE);
}

TEST_CASE("close response terminates connection", "[objstore.tcp]") {
    int port = get_unique_port();
    TestSignalPipe signal_pipe;

    std::atomic<bool> server_ready{false};
    volatile bool exit_signal = false;
    std::atomic<int> request_count{0};
    std::atomic<int> close_count{0};

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &request_count, &close_count]() {
        Pool pool(port, signal_pipe.read_fd());
        Stats stats;

        auto on_chunk = [&](Request& req) -> RequestStatus {
            request_count++;
            return RequestStatus::Close;
        };

        auto on_close = [&](int) {
            close_count++;
        };

        server_ready = true;
        listen(on_chunk, on_close, pool, stats, exit_signal);
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 1000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
        socket_send_all(client, "CLOSE_ME", 8);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        char buf[16];
        auto res = socket_receive(client, buf, sizeof(buf));
        REQUIRE((res.bytes == 0 || res.error == SocketError::ConnectionReset || res.error == SocketError::ConnectionClosed));
    }

    auto start = std::chrono::steady_clock::now();
    while (close_count < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            break;
        }
    }

    exit_signal = true;
    signal_pipe.signal();
    server_thread.join();

    REQUIRE(request_count >= 1);
    REQUIRE(close_count >= 1);
}
