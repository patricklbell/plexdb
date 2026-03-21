#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <semaphore>
#include <string.h>
#include <vector>
#include <stdint.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;

import objstore.engine;
import objstore.native;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore;

namespace {
    constexpr int NATIVE_TEST_PORT_BASE = 23000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return NATIVE_TEST_PORT_BASE + port_counter.fetch_add(1);
    }

    // ========================================================================
    // Binary frame builder (client side)
    // ========================================================================
    struct Bytes {
        std::vector<uint8_t> data;

        void u8(uint8_t v)  { data.push_back(v); }
        void u16(uint16_t v){ u8(v >> 8); u8(v); }
        void s32(int32_t v) { u8(uint32_t(v) >> 24); u8(uint32_t(v) >> 16); u8(uint32_t(v) >> 8); u8(uint32_t(v)); }

        // [string]: [short] n + bytes
        void cql_string(const char* s) {
            uint16_t len = uint16_t(strlen(s));
            u16(len);
            for (uint16_t i = 0; i < len; i++) u8(uint8_t(s[i]));
        }

        // [long string]: [int] n + bytes
        void cql_long_string(const char* s) {
            int32_t len = int32_t(strlen(s));
            s32(len);
            for (int32_t i = 0; i < len; i++) u8(uint8_t(s[i]));
        }

        // Prepend 9-byte frame header
        void prepend_header(uint8_t op, int16_t stream) {
            int32_t body_len = int32_t(data.size());
            std::vector<uint8_t> hdr(9);
            hdr[0] = 0x04; // request version
            hdr[1] = 0x00; // flags
            hdr[2] = uint8_t(uint16_t(stream) >> 8);
            hdr[3] = uint8_t(stream);
            hdr[4] = op;
            hdr[5] = uint8_t(uint32_t(body_len) >> 24);
            hdr[6] = uint8_t(uint32_t(body_len) >> 16);
            hdr[7] = uint8_t(uint32_t(body_len) >> 8);
            hdr[8] = uint8_t(body_len);
            data.insert(data.begin(), hdr.begin(), hdr.end());
        }
    };

    Bytes make_startup(int16_t stream = 0) {
        Bytes b;
        b.u16(1);
        b.cql_string("CQL_VERSION");
        b.cql_string("3.0.0");
        b.prepend_header(0x01, stream);
        return b;
    }

    Bytes make_options(int16_t stream = 0) {
        Bytes b;
        b.prepend_header(0x05, stream);
        return b;
    }

    Bytes make_query(const char* cql, int16_t stream = 0) {
        Bytes b;
        b.cql_long_string(cql);
        b.u16(0x0001); // consistency = ONE
        b.u8(0x00);    // flags
        b.prepend_header(0x07, stream);
        return b;
    }

    void send_frame(Socket& client, const Bytes& b) {
        socket_send_all(client, reinterpret_cast<const char*>(b.data.data()), b.data.size());
    }

    // ========================================================================
    // Response reader
    // ========================================================================
    struct NativeResponse {
        uint8_t  version  = 0;
        uint8_t  flags    = 0;
        int16_t  stream   = 0;
        uint8_t  opcode   = 0;
        int32_t  body_len = 0;
        std::vector<uint8_t> body;
    };

    NativeResponse recv_frame(Socket& client) {
        NativeResponse resp;
        uint8_t hdr[9] = {};
        size_t got = 0;
        while (got < 9) {
            auto res = socket_receive(client, reinterpret_cast<char*>(hdr + got), 9 - got);
            if (res.byte_count <= 0) break;
            got += size_t(res.byte_count);
        }
        resp.version  = hdr[0];
        resp.flags    = hdr[1];
        resp.stream   = int16_t((uint16_t(hdr[2]) << 8) | hdr[3]);
        resp.opcode   = hdr[4];
        resp.body_len = int32_t((uint32_t(hdr[5]) << 24) | (uint32_t(hdr[6]) << 16) | (uint32_t(hdr[7]) << 8) | hdr[8]);

        if (resp.body_len > 0) {
            resp.body.resize(size_t(resp.body_len));
            size_t received = 0;
            while (received < size_t(resp.body_len)) {
                auto res = socket_receive(client, reinterpret_cast<char*>(resp.body.data() + received), size_t(resp.body_len) - received);
                if (res.byte_count <= 0) break;
                received += size_t(res.byte_count);
            }
        }
        return resp;
    }

    // Check if the raw body bytes contain a substring
    bool body_contains(const NativeResponse& resp, const char* needle) {
        if (resp.body.empty()) return false;
        // Search as a byte sequence
        const char* body_str = reinterpret_cast<const char*>(resp.body.data());
        size_t body_size     = resp.body.size();
        size_t needle_len    = strlen(needle);
        if (needle_len == 0) return true;
        for (size_t i = 0; i + needle_len <= body_size; i++) {
            if (memcmp(body_str + i, needle, needle_len) == 0) return true;
        }
        return false;
    }

    // Read the RESULT kind (first 4 bytes of body)
    int32_t result_kind(const NativeResponse& resp) {
        if (resp.body.size() < 4) return -1;
        return int32_t(
            (uint32_t(resp.body[0]) << 24) |
            (uint32_t(resp.body[1]) << 16) |
            (uint32_t(resp.body[2]) << 8)  |
            uint32_t(resp.body[3])
        );
    }
}

TEST_CASE("Native protocol STARTUP handshake", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    auto resp = recv_frame(client);
    CHECK(resp.version == 0x84);
    CHECK(resp.opcode  == 0x02);  // READY
    CHECK(resp.body_len == 0);

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

TEST_CASE("Native protocol OPTIONS returns SUPPORTED", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_options());
    auto resp = recv_frame(client);
    CHECK(resp.version == 0x84);
    CHECK(resp.opcode  == 0x06);  // SUPPORTED
    CHECK(resp.body_len > 0);
    CHECK(body_contains(resp, "CQL_VERSION"));
    CHECK(body_contains(resp, "3.0.0"));
    CHECK(body_contains(resp, "COMPRESSION"));

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

TEST_CASE("Native protocol CQL DDL and DML operations", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    REQUIRE(recv_frame(client).opcode == 0x02);

    // CREATE KEYSPACE -> RESULT Schema_change (0x0005)
    send_frame(client, make_query("CREATE KEYSPACE ks;"));
    auto resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0005);
    CHECK(body_contains(resp, "CREATED"));
    CHECK(body_contains(resp, "KEYSPACE"));

    // CREATE TABLE -> RESULT Schema_change
    send_frame(client, make_query("CREATE TABLE ks.users (id int PRIMARY KEY, name text, age int);"));
    resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0005);
    CHECK(body_contains(resp, "TABLE"));

    // INSERT -> RESULT Void (0x0001)
    send_frame(client, make_query("INSERT INTO ks.users (id, name, age) VALUES (1, 'Alice', 30);"));
    resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0001);

    send_frame(client, make_query("INSERT INTO ks.users (id, name, age) VALUES (2, 'Bob', 25);"));
    resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0001);

    // SELECT -> RESULT Rows (0x0002)
    send_frame(client, make_query("SELECT * FROM ks.users;"));
    resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0002);
    CHECK(body_contains(resp, "Alice"));
    CHECK(body_contains(resp, "Bob"));

    // USE keyspace -> RESULT Set_keyspace (0x0003)
    send_frame(client, make_query("USE ks;"));
    resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0003);

    // DROP TABLE -> RESULT Schema_change
    send_frame(client, make_query("DROP TABLE ks.users;"));
    resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0005);

    // DROP KEYSPACE -> RESULT Schema_change
    send_frame(client, make_query("DROP KEYSPACE ks;"));
    resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    CHECK(result_kind(resp) == 0x0005);

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

TEST_CASE("Native protocol error responses", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    REQUIRE(recv_frame(client).opcode == 0x02);

    // Query non-existent keyspace -> ERROR (0x00)
    send_frame(client, make_query("SELECT * FROM no_such_ks.no_table;"));
    auto resp = recv_frame(client);
    CHECK(resp.opcode == 0x00);

    // Stream id is preserved in the response
    send_frame(client, make_query("SELECT * FROM missing.tbl;", /*stream=*/42));
    resp = recv_frame(client);
    CHECK(resp.opcode == 0x00);
    CHECK(resp.stream == 42);

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

TEST_CASE("Native protocol data persists across restarts", "[objstore.native]") {
    int port1 = get_unique_port();
    int port2 = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    {
        os::Notifier signal_pipe;
        std::binary_semaphore server_ready{0};
        volatile bool exit_signal = false;

        std::thread srv([port1, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
            U64 page_size = 4_kb;
            pager::create(db_file, page_size);
            Pager pager{db_file};
            engine::create_database(pager);
            engine::Engine engine{&pager};
            native::run(port1, signal_pipe, exit_signal, engine, [&server_ready]() {
                server_ready.release();
            });
        });

        server_ready.acquire();
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port1));

        send_frame(client, make_startup());
        REQUIRE(recv_frame(client).opcode == 0x02);

        send_frame(client, make_query("CREATE KEYSPACE pks;"));
        REQUIRE(recv_frame(client).opcode == 0x08);

        send_frame(client, make_query("CREATE TABLE pks.data (id int PRIMARY KEY, val text);"));
        REQUIRE(recv_frame(client).opcode == 0x08);

        send_frame(client, make_query("INSERT INTO pks.data (id, val) VALUES (99, 'persisted');"));
        REQUIRE(recv_frame(client).opcode == 0x08);

        exit_signal = true;
        os::signal_notify_safe(signal_pipe);
        srv.join();
    }

    {
        os::Notifier signal_pipe;
        std::binary_semaphore server_ready{0};
        volatile bool exit_signal = false;

        std::thread srv([port2, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
            Pager pager{db_file};
            engine::Engine engine{&pager};
            native::run(port2, signal_pipe, exit_signal, engine, [&server_ready]() {
                server_ready.release();
            });
        });

        server_ready.acquire();
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port2));

        send_frame(client, make_startup());
        REQUIRE(recv_frame(client).opcode == 0x02);

        send_frame(client, make_query("SELECT * FROM pks.data;"));
        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);  // Rows
        CHECK(body_contains(resp, "persisted"));

        exit_signal = true;
        os::signal_notify_safe(signal_pipe);
        srv.join();
    }
}

TEST_CASE("Native protocol system.local virtual view", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    REQUIRE(recv_frame(client).opcode == 0x02);

    SECTION("SELECT * FROM system.local returns rows with expected columns") {
        send_frame(client, make_query("SELECT * FROM system.local;"));
        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "local"));
        CHECK(body_contains(resp, "datacenter1"));
        CHECK(body_contains(resp, "rack1"));
        CHECK(body_contains(resp, "objstore"));
    }

    SECTION("SELECT * FROM system.peers returns empty rows") {
        send_frame(client, make_query("SELECT * FROM system.peers;"));
        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "peer"));
    }

    SECTION("SELECT * FROM system.peers_v2 returns empty rows") {
        send_frame(client, make_query("SELECT * FROM system.peers_v2;"));
        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
    }

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

TEST_CASE("Native protocol system_schema virtual views", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    REQUIRE(recv_frame(client).opcode == 0x02);

    send_frame(client, make_query("CREATE KEYSPACE test_ks;"));
    REQUIRE(recv_frame(client).opcode == 0x08);

    send_frame(client, make_query("CREATE TABLE test_ks.users (id int PRIMARY KEY, name text);"));
    REQUIRE(recv_frame(client).opcode == 0x08);

    // system_schema.keyspaces
    send_frame(client, make_query("SELECT * FROM system_schema.keyspaces;"));
    {
        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "keyspace_name"));
        CHECK(body_contains(resp, "test_ks"));
    }

    // system_schema.tables
    send_frame(client, make_query("SELECT * FROM system_schema.tables;"));
    {
        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "table_name"));
        CHECK(body_contains(resp, "users"));
    }

    // system_schema.columns
    send_frame(client, make_query("SELECT * FROM system_schema.columns;"));
    {
        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "column_name"));
        CHECK(body_contains(resp, "partition_key"));
        CHECK(body_contains(resp, "name"));
    }

    send_frame(client, make_query("DROP TABLE test_ks.users;"));
    CHECK(recv_frame(client).opcode == 0x08);
    send_frame(client, make_query("DROP KEYSPACE test_ks;"));
    CHECK(recv_frame(client).opcode == 0x08);

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

namespace {
    bool body_contains_bytes(const NativeResponse& resp, std::initializer_list<uint8_t> needle) {
        const auto* data = resp.body.data();
        size_t size = resp.body.size();
        size_t n = needle.size();
        if (n == 0 || size < n) return false;
        const uint8_t* pat = needle.begin();
        for (size_t i = 0; i + n <= size; i++) {
            if (memcmp(data + i, pat, n) == 0) return true;
        }
        return false;
    }
}

TEST_CASE("Native protocol collection serialization", "[objstore.native]") {
    // system.local has a tokens column of type set<text> with one element "0".
    // This test verifies the CQL native binary encoding of collection types and values.
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    REQUIRE(recv_frame(client).opcode == 0x02);

    send_frame(client, make_query("SELECT * FROM system.local;"));
    auto resp = recv_frame(client);
    REQUIRE(resp.opcode == 0x08);
    REQUIRE(result_kind(resp) == 0x0002);  // Rows

    SECTION("tokens column type option is Set<Varchar>") {
        // [short] 0x0022 (Set) + [short] 0x000D (Varchar)
        CHECK(body_contains_bytes(resp, {0x00, 0x22, 0x00, 0x0D}));
    }

    SECTION("tokens set value encodes one-element set containing '0'") {
        // [int] body=9 | [int] count=1 | [int] elemlen=1 | '0'(0x30)
        CHECK(body_contains_bytes(resp, {0x00, 0x00, 0x00, 0x09,
                                         0x00, 0x00, 0x00, 0x01,
                                         0x00, 0x00, 0x00, 0x01,
                                         0x30}));
    }

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

TEST_CASE("Native protocol PREPARE and EXECUTE", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    REQUIRE(recv_frame(client).opcode == 0x02);

    // Setup keyspace and table
    send_frame(client, make_query("CREATE KEYSPACE prep_ks;"));
    REQUIRE(recv_frame(client).opcode == 0x08);

    send_frame(client, make_query("CREATE TABLE prep_ks.data (id int PRIMARY KEY, name text, score double);"));
    REQUIRE(recv_frame(client).opcode == 0x08);

    // PREPARE an INSERT statement with bind markers
    {
        Bytes b;
        b.cql_long_string("INSERT INTO prep_ks.data (id, name, score) VALUES (?, ?, ?)");
        b.prepend_header(0x09, 1);  // PREPARE opcode
        send_frame(client, b);

        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);  // RESULT
        REQUIRE(result_kind(resp) == 0x0004);  // Prepared
        CHECK(resp.body.size() > 8);  // has prepared id and metadata

        // Extract the prepared id (after [int] kind): [short] id_len + id_bytes
        const uint8_t* p = resp.body.data() + 4;
        uint16_t id_len = (uint16_t(p[0]) << 8) | p[1];
        CHECK(id_len == 8);
        std::vector<uint8_t> prepared_id(p + 2, p + 2 + id_len);

        // EXECUTE the prepared statement with bound values
        {
            Bytes ex;
            // [short bytes] id
            ex.u16(uint16_t(prepared_id.size()));
            for (auto byte : prepared_id) ex.u8(byte);
            // query parameters
            ex.u16(0x0001); // consistency = ONE
            ex.u8(0x01);    // flags: Values
            ex.u16(3);      // n_values = 3

            // value 0: int 42 (4 bytes BE)
            ex.s32(4);
            ex.u8(0); ex.u8(0); ex.u8(0); ex.u8(42);

            // value 1: text "Alice" (5 bytes)
            ex.s32(5);
            ex.u8('A'); ex.u8('l'); ex.u8('i'); ex.u8('c'); ex.u8('e');

            // value 2: double 99.5 (8 bytes BE IEEE 754)
            ex.s32(8);
            uint64_t dbl_bits;
            double dbl_val = 99.5;
            memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
            for (int i = 7; i >= 0; i--) ex.u8(uint8_t(dbl_bits >> (i * 8)));

            ex.prepend_header(0x0A, 2);  // EXECUTE opcode
            send_frame(client, ex);

            auto exec_resp = recv_frame(client);
            REQUIRE(exec_resp.opcode == 0x08);  // RESULT
            CHECK(result_kind(exec_resp) == 0x0001);  // Void
        }

        // Verify data was inserted by querying
        send_frame(client, make_query("SELECT * FROM prep_ks.data;"));
        auto select_resp = recv_frame(client);
        REQUIRE(select_resp.opcode == 0x08);
        CHECK(result_kind(select_resp) == 0x0002);  // Rows
        CHECK(body_contains(select_resp, "Alice"));
    }

    // PREPARE with named bind markers, then EXECUTE with values in a different order
    {
        Bytes b;
        b.cql_long_string("INSERT INTO prep_ks.data (id, name, score) VALUES (:id, :name, :score)");
        b.prepend_header(0x09, 3);  // PREPARE opcode
        send_frame(client, b);

        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);  // RESULT
        REQUIRE(result_kind(resp) == 0x0004);  // Prepared

        // Extract the prepared id
        const uint8_t* p = resp.body.data() + 4;
        uint16_t id_len = (uint16_t(p[0]) << 8) | p[1];
        CHECK(id_len == 8);
        std::vector<uint8_t> prepared_id(p + 2, p + 2 + id_len);

        // EXECUTE with named values in REVERSED order (score, name, id)
        {
            Bytes ex;
            // [short bytes] id
            ex.u16(uint16_t(prepared_id.size()));
            for (auto byte : prepared_id) ex.u8(byte);
            // query parameters
            ex.u16(0x0001); // consistency = ONE
            ex.u8(0x41);    // flags: Values (0x01) | Named values (0x40)
            ex.u16(3);      // n_values = 3

            // Named value 1: "score" = 77.7 (sent first, but should go to third bind position)
            ex.cql_string("score");
            ex.s32(8);
            uint64_t dbl_bits;
            double dbl_val = 77.7;
            memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
            for (int i = 7; i >= 0; i--) ex.u8(uint8_t(dbl_bits >> (i * 8)));

            // Named value 2: "name" = "Bob" (sent second, but should go to second bind position)
            ex.cql_string("name");
            ex.s32(3);
            ex.u8('B'); ex.u8('o'); ex.u8('b');

            // Named value 3: "id" = 99 (sent last, but should go to first bind position)
            ex.cql_string("id");
            ex.s32(4);
            ex.u8(0); ex.u8(0); ex.u8(0); ex.u8(99);

            ex.prepend_header(0x0A, 4);  // EXECUTE opcode
            send_frame(client, ex);

            auto exec_resp = recv_frame(client);
            REQUIRE(exec_resp.opcode == 0x08);  // RESULT
            CHECK(result_kind(exec_resp) == 0x0001);  // Void
        }

        // Verify data was inserted correctly (names matched to correct columns)
        send_frame(client, make_query("SELECT * FROM prep_ks.data WHERE id = 99;"));
        auto select_resp = recv_frame(client);
        REQUIRE(select_resp.opcode == 0x08);
        CHECK(result_kind(select_resp) == 0x0002);  // Rows
        CHECK(body_contains(select_resp, "Bob"));
    }

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}

TEST_CASE("Native protocol QUERY with bind values", "[objstore.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    os::Notifier signal_pipe;
    std::binary_semaphore server_ready{0};
    volatile bool exit_signal = false;

    std::thread server_thread([port, &signal_pipe, &server_ready, &exit_signal, &db_file]() {
        U64 page_size = 4_kb;
        pager::create(db_file, page_size);
        Pager pager{db_file};
        engine::create_database(pager);
        engine::Engine engine{&pager};
        native::run(port, signal_pipe, exit_signal, engine, [&server_ready]() {
            server_ready.release();
        });
    });

    server_ready.acquire();
    Socket client{socket_open()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));

    send_frame(client, make_startup());
    REQUIRE(recv_frame(client).opcode == 0x02);

    send_frame(client, make_query("CREATE KEYSPACE qbind_ks;"));
    REQUIRE(recv_frame(client).opcode == 0x08);
    send_frame(client, make_query("CREATE TABLE qbind_ks.items (id int PRIMARY KEY, label text, weight double);"));
    REQUIRE(recv_frame(client).opcode == 0x08);

    SECTION("positional bind values") {
        Bytes b;
        b.cql_long_string("INSERT INTO qbind_ks.items (id, label, weight) VALUES (?, ?, ?)");
        b.u16(0x0001); // consistency = ONE
        b.u8(0x01);    // flags: Values
        b.u16(3);      // n_values = 3
        // int 7
        b.s32(4); b.u8(0); b.u8(0); b.u8(0); b.u8(7);
        // text "Gadget"
        b.s32(6); b.u8('G'); b.u8('a'); b.u8('d'); b.u8('g'); b.u8('e'); b.u8('t');
        // double 3.14
        b.s32(8);
        uint64_t dbl_bits; double dbl_val = 3.14;
        memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
        for (int i = 7; i >= 0; i--) b.u8(uint8_t(dbl_bits >> (i * 8)));
        b.prepend_header(0x07, 1);
        send_frame(client, b);

        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0001); // Void

        send_frame(client, make_query("SELECT * FROM qbind_ks.items;"));
        auto sel = recv_frame(client);
        REQUIRE(sel.opcode == 0x08);
        CHECK(result_kind(sel) == 0x0002); // Rows
        CHECK(body_contains(sel, "Gadget"));
    }

    SECTION("named bind values") {
        Bytes b;
        b.cql_long_string("INSERT INTO qbind_ks.items (id, label, weight) VALUES (:id, :label, :weight)");
        b.u16(0x0001); // consistency = ONE
        b.u8(0x41);    // flags: Values | Named values
        b.u16(3);      // n_values = 3
        // weight first (out of order)
        b.cql_string("weight");
        b.s32(8);
        uint64_t dbl_bits; double dbl_val = 2.71;
        memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
        for (int i = 7; i >= 0; i--) b.u8(uint8_t(dbl_bits >> (i * 8)));
        // id second
        b.cql_string("id");
        b.s32(4); b.u8(0); b.u8(0); b.u8(0); b.u8(13);
        // label last
        b.cql_string("label");
        b.s32(5); b.u8('G'); b.u8('i'); b.u8('z'); b.u8('m'); b.u8('o');
        b.prepend_header(0x07, 2);
        send_frame(client, b);

        auto resp = recv_frame(client);
        REQUIRE(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0001); // Void

        send_frame(client, make_query("SELECT * FROM qbind_ks.items WHERE id = 13;"));
        auto sel = recv_frame(client);
        REQUIRE(sel.opcode == 0x08);
        CHECK(result_kind(sel) == 0x0002); // Rows
        CHECK(body_contains(sel, "Gizmo"));
    }

    exit_signal = true;
    os::signal_notify_safe(signal_pipe);
    server_thread.join();
}
