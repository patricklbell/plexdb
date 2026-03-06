#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <atomic>
#include <semaphore>
#include <string.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;

import objstore.engine;
import objstore.server;
import objstore.parser;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore;

namespace {
    constexpr int E2E_TEST_PORT_BASE = 22000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return E2E_TEST_PORT_BASE + port_counter.fetch_add(1);
    }

    bool contains(const char* haystack, const char* needle) {
        return strstr(haystack, needle) != nullptr;
    }

    struct OwnedResponse {
        U16 status_code;
        AutoString8 body;
    };

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

    OwnedResponse send_cql(Socket& client, const String8& cql) {
        AutoString8 request = make_http_request(cql);
        socket_send_all(client, request.c_str, request.length);

        parser::http::ResponseParser parser;
        char buffer[4096];
        
        while (!is_complete(parser) && !has_error(parser)) {
            auto res = socket_receive(client, buffer, sizeof(buffer));
            if (res.byte_count <= 0) break;
            execute(parser, buffer, res.byte_count);
        }
        
        const auto& resp = get_response(parser);
        return OwnedResponse{
            .status_code = resp.status_code,
            .body = AutoString8{resp.body}
        };
    }
}

TEST_CASE("Server end-to-end CQL operations with persistence", "[objstore.server]") {
    int port = get_unique_port();

    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    // First server instance: create schema and insert data
    {
        os::Notifier signal_pipe;
        std::binary_semaphore server_ready{0};
        volatile bool exit_signal = false;

        std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
            U64 page_size = 4_kb;
            pager::create(db_file, page_size);
            Pager pager{db_file};

            engine::create_database(pager);
            engine::Engine engine{&pager};

            server::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
                server_ready.release();
            });
        });

        server_ready.acquire();

        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

        auto response = send_cql(client, "CREATE KEYSPACE test_ks WITH replication = 'SimpleStrategy';");
        REQUIRE(response.status_code == 200);

        response = send_cql(client, "CREATE TABLE test_ks.users (id int PRIMARY KEY, name text, age int);");
        REQUIRE(response.status_code == 200);

        response = send_cql(client, "INSERT INTO test_ks.users VALUES (1, 'Alice', 30);");
        REQUIRE(response.status_code == 200);

        response = send_cql(client, "INSERT INTO test_ks.users VALUES (2, 'Bob', 25);");
        REQUIRE(response.status_code == 200);

        exit_signal = true;
        os::signal_notify_safe(signal_pipe);
        server_thread.join();
    }

    // Second server instance: verify data persisted
    {
        int port2 = get_unique_port();
        os::Notifier signal_pipe;
        std::binary_semaphore server_ready{0};
        volatile bool exit_signal = false;

        std::thread server_thread([port2, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
            Pager pager{db_file};
            engine::Engine engine{&pager};

            server::run(port2, signal_pipe, exit_signal, engine, [&server_ready]() {
                server_ready.release();
            });
        });

        server_ready.acquire();

        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port2));

        auto response = send_cql(client, "SELECT * FROM test_ks.users;");
        REQUIRE(response.status_code == 200);

        REQUIRE(contains(response.body.c_str, "Alice"));
        REQUIRE(contains(response.body.c_str, "Bob"));
        REQUIRE(contains(response.body.c_str, "30"));
        REQUIRE(contains(response.body.c_str, "25"));

        exit_signal = true;
        os::signal_notify_safe(signal_pipe);
        server_thread.join();
    }
}
