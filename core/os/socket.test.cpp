#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>

import plexdb.base;
import plexdb.os;
import plexdb.threads;

using namespace plexdb;
using namespace plexdb::os;

namespace {
    constexpr int    TEST_PORT_BASE = 19000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return TEST_PORT_BASE + port_counter.fetch_add(1);
    }
}

TEST_CASE("Socket simple client-server exchange", "[plexdb.os.socket]") {
    int port = get_unique_port();

    threads::Semaphore server_ready{0};
    AutoString8        received_data;

    Socket listen_sock{socket_open()};
    socket_set_option(listen_sock, SocketOption::Reuse, true);
    socket_bind(listen_sock, (U16)port);
    socket_listen(listen_sock, 1);
    socket_set_timeout(listen_sock, 2000);

    threads::Thread client_thread = threads::launch("test-client", [&]() {
        server_ready.wait();

        Socket client{socket_open()};
        socket_set_timeout(client, 1000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        String8 message  = "PING";
        auto    send_res = socket_send_all(client, message.data, message.length);
        CHECK(send_res.error == SocketError::None);

        char buffer[256];
        auto recv_res = socket_receive(client, buffer, sizeof(buffer) - 1);
        CHECK(recv_res.byte_count > 0);
        if (recv_res.byte_count > 0) {
            buffer[recv_res.byte_count] = '\0';
            CHECK(String8{buffer, (U64)recv_res.byte_count} == "PONG");
        }
    });

    server_ready.signal();

    Socket accepted{socket_accept(listen_sock)};
    if (accepted) {
        socket_set_timeout(accepted, 1000);

        char buffer[256];
        auto res = socket_receive(accepted, buffer, sizeof(buffer) - 1);
        if (res.byte_count > 0) {
            buffer[res.byte_count] = '\0';
            received_data          = AutoString8{buffer};

            String8 response = "PONG";
            socket_send(accepted, response.data, response.length);
        }
    }

    // Explicit join before checking results
    client_thread = {};
    REQUIRE(received_data == "PING");
}

TEST_CASE("Socket recv timeout", "[plexdb.os.socket]") {
    int port = get_unique_port();

    threads::Semaphore server_ready{0};

    Socket listen_sock{socket_open()};
    socket_set_option(listen_sock, SocketOption::Reuse, true);
    socket_bind(listen_sock, (U16)port);
    socket_listen(listen_sock, 1);
    socket_set_timeout(listen_sock, 3000);

    threads::Thread client_thread = threads::launch("test-client", [&]() {
        server_ready.wait();

        Socket client{socket_open()};
        socket_set_timeout(client, 500);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        char buffer[256];
        auto recv_res = socket_receive(client, buffer, sizeof(buffer));
        CHECK(recv_res.byte_count <= 0);
    });

    server_ready.signal();

    Socket accepted{socket_accept(listen_sock)};
    if (accepted) {
        os::sleep_ms(1500);
    }
}

TEST_CASE("Socket multiple rapid connections", "[plexdb.os.socket]") {
    int port = get_unique_port();

    std::atomic<bool>  server_running{true};
    std::atomic<int>   connections_handled{0};
    threads::Semaphore server_ready{0};

    Socket listen_sock{socket_open()};
    socket_set_option(listen_sock, SocketOption::Reuse, true);
    socket_bind(listen_sock, (U16)port);
    socket_listen(listen_sock, 32);
    socket_set_option(listen_sock, SocketOption::NonBlocking, true);

    threads::Thread client_thread = threads::launch("test-client", [&]() {
        server_ready.wait();

        constexpr int NUM_CONNECTIONS = 10;
        for (int i = 0; i < NUM_CONNECTIONS; i++) {
            Socket client{socket_open()};
            socket_set_timeout(client, 500);
            socket_connect(client, "127.0.0.1", (U16)port);
        }

        os::sleep_ms(200);
        server_running = false;
    });

    server_ready.signal();

    while (server_running) {
        Handle client_fd = socket_accept(listen_sock);
        if (!is_zero_handle(client_fd)) {
            connections_handled++;
            socket_close(client_fd);
        }
        os::sleep_ms(1);
    }

    client_thread = {};
    REQUIRE(connections_handled >= 5);
}

TEST_CASE("Socket send_all sends complete data", "[plexdb.os.socket]") {
    int port = get_unique_port();

    threads::Semaphore server_ready{0};
    U64                total_received = 0;
    constexpr U64      DATA_SIZE      = 4096;

    Socket listen_sock{socket_open()};
    socket_set_option(listen_sock, SocketOption::Reuse, true);
    socket_bind(listen_sock, (U16)port);
    socket_listen(listen_sock, 1);
    socket_set_timeout(listen_sock, 2000);

    threads::Thread client_thread = threads::launch("test-client", [&]() {
        server_ready.wait();

        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        char data[DATA_SIZE];
        for (U64 i = 0; i < DATA_SIZE; i++) {
            data[i] = (char)('A' + (i % 26));
        }

        auto send_res = socket_send_all(client, data, DATA_SIZE);
        CHECK(send_res.error == SocketError::None);
        CHECK(send_res.byte_count == (S64)DATA_SIZE);
    });

    server_ready.signal();

    Socket accepted{socket_accept(listen_sock)};
    if (accepted) {
        socket_set_timeout(accepted, 1000);

        char buffer[8192];
        while (true) {
            auto res = socket_receive(accepted, buffer, sizeof(buffer));
            if (res.byte_count > 0) {
                total_received += res.byte_count;
            } else {
                break;
            }
        }
    }

    client_thread = {};
    REQUIRE(total_received == DATA_SIZE);
}

TEST_CASE("Socket handles empty connection (connect and close)", "[plexdb.os.socket]") {
    int port = get_unique_port();

    threads::Semaphore server_ready{0};
    std::atomic<bool>  client_closed{false};

    Socket listen_sock{socket_open()};
    socket_set_option(listen_sock, SocketOption::Reuse, true);
    socket_bind(listen_sock, (U16)port);
    socket_listen(listen_sock, 1);
    socket_set_timeout(listen_sock, 2000);

    threads::Thread client_thread = threads::launch("test-client", [&]() {
        server_ready.wait();
        {
            Socket client{socket_open()};
            socket_set_timeout(client, 500);
            CHECK(socket_connect(client, "127.0.0.1", (U16)port));
        }
    });

    server_ready.signal();

    Socket accepted{socket_accept(listen_sock)};
    if (accepted) {
        socket_set_timeout(accepted, 1000);

        char buffer[256];
        auto res = socket_receive(accepted, buffer, sizeof(buffer));
        if (res.error == SocketError::ConnectionClosed || res.byte_count == 0) {
            client_closed = true;
        }
    }

    client_thread = {};
    REQUIRE(client_closed);
}

TEST_CASE("Socket handles partial data", "[plexdb.os.socket]") {
    int port = get_unique_port();

    threads::Semaphore server_ready{0};
    U64                total_received = 0;

    Socket listen_sock{socket_open()};
    socket_set_option(listen_sock, SocketOption::Reuse, true);
    socket_bind(listen_sock, (U16)port);
    socket_listen(listen_sock, 1);
    socket_set_timeout(listen_sock, 2000);

    threads::Thread client_thread = threads::launch("test-client", [&]() {
        server_ready.wait();

        Socket client{socket_open()};
        socket_set_timeout(client, 1000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        socket_send(client, "Hello", 5);
        os::sleep_ms(50);
        socket_send(client, "World", 5);
    });

    server_ready.signal();

    Socket accepted{socket_accept(listen_sock)};
    if (accepted) {
        socket_set_timeout(accepted, 1000);

        char buffer[1024];
        while (true) {
            auto res = socket_receive(accepted, buffer, sizeof(buffer));
            if (res.byte_count > 0) {
                total_received += res.byte_count;
            } else {
                break;
            }
        }
    }

    client_thread = {};
    REQUIRE(total_received == 10);
}

TEST_CASE("Socket handles binary data with null bytes", "[plexdb.os.socket]") {
    int port = get_unique_port();

    threads::Semaphore server_ready{0};
    char               received_data[256] = {0};
    U64                received_len       = 0;

    Socket listen_sock{socket_open()};
    socket_set_option(listen_sock, SocketOption::Reuse, true);
    socket_bind(listen_sock, (U16)port);
    socket_listen(listen_sock, 1);
    socket_set_timeout(listen_sock, 2000);

    threads::Thread client_thread = threads::launch("test-client", [&]() {
        server_ready.wait();

        Socket client{socket_open()};
        socket_set_timeout(client, 1000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        char binary_data[10] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00};
        socket_send_all(client, binary_data, 10);
    });

    server_ready.signal();

    Socket accepted{socket_accept(listen_sock)};
    if (accepted) {
        socket_set_timeout(accepted, 1000);

        auto res = socket_receive(accepted, received_data, sizeof(received_data));
        if (res.byte_count > 0) {
            received_len = res.byte_count;
        }
    }

    client_thread = {};
    REQUIRE(received_len == 10);
    REQUIRE(received_data[0] == 0x01);
    REQUIRE(received_data[1] == 0x00);
    REQUIRE(received_data[2] == 0x02);
}
