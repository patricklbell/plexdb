#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <string.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;

import objstore.engine;
import objstore.server;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore;

namespace {
    constexpr int E2E_TEST_PORT_BASE = 22000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return E2E_TEST_PORT_BASE + port_counter.fetch_add(1);
    }

    struct SignalPipe {
        int read_fd = -1;
        int write_fd = -1;

        SignalPipe() {
            int fds[2];
            if (pipe(fds) == 0) {
                read_fd = fds[0];
                write_fd = fds[1];
            }
        }

        ~SignalPipe() {
            if (read_fd >= 0) close(read_fd);
            if (write_fd >= 0) close(write_fd);
        }

        void signal() {
            char c = 1;
            [[maybe_unused]] auto res = write(write_fd, &c, 1);
        }
    };

    bool contains(const char* haystack, const char* needle) {
        return strstr(haystack, needle) != nullptr;
    }

    AutoString8 make_http_request(const String8& cql) {
        return fmt(
            "POST / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            static_cast<int>(cql.length),
            cql.c_str()
        );
    }

    AutoString8 send_cql(Socket& client, const String8& cql) {
        AutoString8 request = make_http_request(cql);
        socket_send_all(client, request.c_str, request.length);

        char buffer[4096];
        auto res = socket_receive(client, buffer, sizeof(buffer) - 1);
        if (res.bytes <= 0) return AutoString8{""};
        buffer[res.bytes] = '\0';
        return AutoString8{buffer};
    }

    const char* extract_http_body(const char* response) {
        const char* body_start = strstr(response, "\r\n\r\n");
        if (body_start == nullptr) return "";
        return body_start + 4;
    }
}

TEST_CASE("Server end-to-end CQL operations with persistence", "[objstore.server]") {
    int port = get_unique_port();

    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    // First server instance: create schema and insert data
    {
        SignalPipe signal_pipe;
        std::atomic<bool> server_ready{false};
        volatile bool exit_signal = false;

        std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
            U64 page_size = 4_kb;
            pager::create(db_file, page_size);
            Pager pager{db_file};

            engine::create_database(pager);
            engine::Engine engine{&pager};

            server::run(port, signal_pipe.read_fd, exit_signal, engine, [&server_ready]() {
                server_ready = true;
            });
        });

        while (!server_ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        auto response = send_cql(client, "CREATE KEYSPACE test_ks WITH replication = 'SimpleStrategy';");
        REQUIRE(contains(response.c_str, "200 OK"));

        response = send_cql(client, "CREATE TABLE test_ks.users (id int PRIMARY KEY, name text, age int);");
        REQUIRE(contains(response.c_str, "200 OK"));

        response = send_cql(client, "INSERT INTO test_ks.users VALUES (1, 'Alice', 30);");
        REQUIRE(contains(response.c_str, "200 OK"));

        response = send_cql(client, "INSERT INTO test_ks.users VALUES (2, 'Bob', 25);");
        REQUIRE(contains(response.c_str, "200 OK"));

        exit_signal = true;
        signal_pipe.signal();
        server_thread.join();
    }

    // Second server instance: verify data persisted
    {
        int port2 = get_unique_port();
        SignalPipe signal_pipe;
        std::atomic<bool> server_ready{false};
        volatile bool exit_signal = false;

        std::thread server_thread([port2, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
            Pager pager{db_file};
            engine::Engine engine{&pager};

            server::run(port2, signal_pipe.read_fd, exit_signal, engine, [&server_ready]() {
                server_ready = true;
            });
        });

        while (!server_ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port2));

        auto response = send_cql(client, "SELECT * FROM test_ks.users;");
        REQUIRE(contains(response.c_str, "200 OK"));

        const char* body = extract_http_body(response.c_str);
        REQUIRE(contains(body, "Alice"));
        REQUIRE(contains(body, "Bob"));
        REQUIRE(contains(body, "30"));
        REQUIRE(contains(body, "25"));

        exit_signal = true;
        signal_pipe.signal();
        server_thread.join();
    }
}
