#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <coroutine>
#include <thread>
#include <atomic>
#include <semaphore>
#include <vector>

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;
import plexdb.os.uring;
import objstore.tcp;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore::tcp;
namespace tcp = objstore::tcp;

namespace {
    constexpr int TCP_TEST_PORT_BASE = 21000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return TCP_TEST_PORT_BASE + port_counter.fetch_add(1);
    }

    struct TestServer {
        os::Handle socket = os::zero_handle();
        os::Notifier interrupt;
        U16 port;

        TestServer(int p) : port(static_cast<U16>(p)) {
            socket = socket_open();
            socket_set_option(socket, SocketOption::Reuse, true);
            socket_bind(socket, port);
            socket_listen(socket, 128);
        }

        ~TestServer() {
            if (!is_zero_handle(socket)) socket_close(socket);
        }

        void stop() { os::signal_notify_safe(interrupt); }
    };
}

TEST_CASE("handles signal from pipe", "[objstore.tcp]") {
    TestServer server(get_unique_port());
    REQUIRE(!is_zero_handle(server.socket));
}

TEST_CASE("receives request", "[objstore.tcp]") {
    bool try_uring = GENERATE(false, true);
    TestServer server(get_unique_port());

    std::binary_semaphore done{0};
    volatile bool should_exit = false;
    AutoString8 received;

    std::thread server_thread([&]() {
        Listener listener{server.socket, try_uring};
        listen(listener, [&](Request req) -> coroutine::Task<void, coroutine::Start::Eager> {
            auto buf = tcp::acquire(req);
            CHECK(static_cast<bool>(buf));
            CHECK(co_await tcp::read(req, &*buf));
            received = AutoString8{buf->view.ptr, buf->length};
            tcp::release(req, &*buf);

            done.release();
            co_await tcp::close(req);
        }, server.interrupt, should_exit);
    });

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", server.port));
        socket_send_all(client, "HELLO", 5);
    }

    done.acquire();
    should_exit = true;
    server.stop();
    server_thread.join();

    REQUIRE(received == "HELLO");
}

TEST_CASE("receives large request", "[objstore.tcp]") {
    TestServer server(get_unique_port());

    std::binary_semaphore done{0};
    volatile bool should_exit = false;
    U64 total_received = 0;
    constexpr U64 DATA_SIZE = 32 * 1024;

    std::thread server_thread([&]() {
        Listener listener{server.socket};
        listen(listener, [&](Request req) -> coroutine::Task<void, coroutine::Start::Eager> {
            U64 total = 0;
            while (total < DATA_SIZE) {
                auto buf = tcp::acquire(req);
                CHECK(static_cast<bool>(buf));
                CHECK(co_await tcp::read(req, &*buf));
                tcp::release(req, &*buf);
                total += buf->length;
            }
            total_received = total;
            done.release();
            co_await tcp::close(req);
        }, server.interrupt, should_exit);
    });

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", server.port));
        std::vector<char> data(DATA_SIZE);
        for (U64 i = 0; i < DATA_SIZE; i++) data[i] = (char)('A' + (i % 26));
        socket_send_all(client, data.data(), data.size());
    }

    done.acquire();
    should_exit = true;
    server.stop();
    server_thread.join();

    REQUIRE(total_received >= DATA_SIZE);
}

TEST_CASE("handles multiple sequential connections", "[objstore.tcp]") {
    TestServer server(get_unique_port());

    std::counting_semaphore<16> connection_sem{0};
    volatile bool should_exit = false;
    std::atomic<int> connection_count{0};
    constexpr int NUM_CONNECTIONS = 5;

    std::thread server_thread([&]() {
        Listener listener{server.socket};
        listen(listener, [&](Request req) -> coroutine::Task<void, coroutine::Start::Eager> {
            auto buf = tcp::acquire(req);
            CHECK(static_cast<bool>(buf));
            CHECK(co_await tcp::read(req, &*buf));
            tcp::release(req, &*buf);

            connection_count++;
            connection_sem.release();
            co_await tcp::close(req);
        }, server.interrupt, should_exit);
    });

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", server.port));
        char msg[16]; snprintf(msg, sizeof(msg), "MSG%d", i);
        socket_send_all(client, msg, (int)strlen(msg));
        connection_sem.acquire();
    }

    should_exit = true;
    server.stop();
    server_thread.join();

    REQUIRE(connection_count == NUM_CONNECTIONS);
}

TEST_CASE("handles concurrent connections", "[objstore.tcp]") {
    TestServer server(get_unique_port());

    std::counting_semaphore<16> request_sem{0};
    volatile bool should_exit = false;
    std::atomic<int> request_count{0};
    constexpr int NUM_CLIENTS = 4;

    std::thread server_thread([&]() {
        Listener listener{server.socket};
        listen(listener, [&](Request req) -> coroutine::Task<void, coroutine::Start::Eager> {
            auto buf = tcp::acquire(req);
            CHECK(static_cast<bool>(buf));
            CHECK(co_await tcp::read(req, &*buf));
            tcp::release(req, &*buf);
            
            request_count++;
            request_sem.release();
            co_await tcp::close(req);
        }, server.interrupt, should_exit);
    });

    std::vector<std::thread> clients;
    clients.reserve(NUM_CLIENTS);
    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients.emplace_back([&server, i]() {
            Socket client{socket_open()};
            if (socket_connect(client, "127.0.0.1", server.port)) {
                char msg[16]; snprintf(msg, sizeof(msg), "CLIENT%d", i);
                socket_send_all(client, msg, (int)strlen(msg));
            }
        });
    }
    for (auto& t : clients) t.join();
    for (int i = 0; i < NUM_CLIENTS; i++) request_sem.acquire();

    should_exit = true;
    server.stop();
    server_thread.join();

    REQUIRE(request_count == NUM_CLIENTS);
}

TEST_CASE("handles client disconnect", "[objstore.tcp]") {
    TestServer server(get_unique_port());

    std::binary_semaphore done{0};
    volatile bool should_exit = false;

    std::thread server_thread([&]() {
        Listener listener{server.socket};
        listen(listener, [&](Request req) -> coroutine::Task<void, coroutine::Start::Eager> {
            auto buf = tcp::acquire(req);
            CHECK(static_cast<bool>(buf));            
            CHECK(!co_await tcp::read(req, &*buf));
            tcp::release(req, &*buf);

            done.release();
            co_await tcp::close(req);
        }, server.interrupt, should_exit);
    });

    { Socket client{socket_open()}; REQUIRE(socket_connect(client, "127.0.0.1", server.port)); }

    done.acquire();
    should_exit = true;
    server.stop();
    server_thread.join();
}

TEST_CASE("tracks statistics correctly", "[objstore.tcp]") {
    TestServer server(get_unique_port());

    std::binary_semaphore done{0};
    volatile bool should_exit = false;

    std::thread server_thread([&]() {
        Listener listener{server.socket};
        listen(listener, [&](Request req) -> coroutine::Task<void, coroutine::Start::Eager> {
            auto buf = tcp::acquire(req);
            CHECK(static_cast<bool>(buf));
            CHECK(co_await tcp::read(req, &*buf));
            tcp::release(req, &*buf);

            done.release();
            co_await tcp::close(req);
        }, server.interrupt, should_exit);

        REQUIRE(listener.stats.total_connections >= 1);
        REQUIRE(listener.stats.total_bytes_read >= 8);
    });

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", server.port));
        socket_send_all(client, "TESTDATA", 8);
    }

    done.acquire();
    should_exit = true;
    server.stop();
    server_thread.join();
}

TEST_CASE("binary data with null bytes", "[objstore.tcp]") {
    TestServer server(get_unique_port());

    std::binary_semaphore done{0};
    volatile bool should_exit = false;
    std::vector<U8> received;

    std::thread server_thread([&]() {
        Listener listener{server.socket};
        listen(listener, [&](Request req) -> coroutine::Task<void, coroutine::Start::Eager> {
            auto buf = tcp::acquire(req);
            CHECK(static_cast<bool>(buf));
            CHECK(co_await tcp::read(req, &*buf));
            received.assign(buf->view.ptr, buf->view.ptr + buf->length);
            tcp::release(req, &*buf);

            done.release();
            co_await tcp::close(req);
        }, server.interrupt, should_exit);
    });

    {
        Socket client{socket_open()};
        REQUIRE(socket_connect(client, "127.0.0.1", server.port));
        char binary_data[10] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00};
        socket_send_all(client, binary_data, 10);
    }

    done.acquire();
    should_exit = true;
    server.stop();
    server_thread.join();

    REQUIRE(received.size() >= 10);
    REQUIRE(received[0] == 0x01);
    REQUIRE(received[1] == 0x00);
    REQUIRE(received[2] == 0x02);
    REQUIRE(received[4] == 0x03);
}
