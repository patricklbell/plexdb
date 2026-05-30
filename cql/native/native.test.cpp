#include <catch2/catch_test_macros.hpp>
#include <plexdb/test_macros/test_macros.h>

#include <string.h>
#include <string>
#include <vector>
#include <stdint.h>
#include <atomic>
#include <cinttypes>

#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.threads;
import plexdb.pager;
import plexdb.pager.test_helpers;
import plexdb.aio;
import plexdb.server.test_helpers;

import cql.engine;
import cql.native;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

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

    Bytes create_startup(int16_t stream = 0) {
        Bytes b;
        b.u16(1);
        b.cql_string("CQL_VERSION");
        b.cql_string("3.0.0");
        b.prepend_header(0x01, stream);
        return b;
    }

    Bytes create_options(int16_t stream = 0) {
        Bytes b;
        b.prepend_header(0x05, stream);
        return b;
    }

    Bytes create_query(const char* cql, int16_t stream = 0) {
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

PAGER_TEST_CASE("Native protocol STARTUP handshake", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        auto resp = recv_frame(client);
        CHECK(resp.version == 0x84);
        CHECK(resp.opcode  == 0x02);
        CHECK(resp.body_len == 0);

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol OPTIONS returns SUPPORTED", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_options());
        auto resp = recv_frame(client);
        CHECK(resp.version == 0x84);
        CHECK(resp.opcode  == 0x06);
        CHECK(resp.body_len > 0);
        CHECK(body_contains(resp, "CQL_VERSION"));
        CHECK(body_contains(resp, "3.0.0"));
        CHECK(body_contains(resp, "COMPRESSION"));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol CQL DDL and DML operations", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0005);
        CHECK(body_contains(resp, "CREATED"));
        CHECK(body_contains(resp, "KEYSPACE"));

        send_frame(client, create_query("CREATE TABLE ks.users (id int PRIMARY KEY, name text, age int);"));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0005);
        CHECK(body_contains(resp, "TABLE"));

        send_frame(client, create_query("INSERT INTO ks.users (id, name, age) VALUES (1, 'Alice', 30);"));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0001);

        send_frame(client, create_query("INSERT INTO ks.users (id, name, age) VALUES (2, 'Bob', 25);"));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0001);

        send_frame(client, create_query("SELECT * FROM ks.users;"));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "Alice"));
        CHECK(body_contains(resp, "Bob"));

        send_frame(client, create_query("USE ks;"));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0003);

        send_frame(client, create_query("DROP TABLE ks.users;"));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0005);

        send_frame(client, create_query("DROP KEYSPACE ks;"));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0005);

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol error responses", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("SELECT * FROM no_such_ks.no_table;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x00);

        send_frame(client, create_query("SELECT * FROM missing.tbl;", /*stream=*/42));
        resp = recv_frame(client);
        CHECK(resp.opcode == 0x00);
        CHECK(resp.stream == 42);

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol data persists across restarts", "[cql.native]") {
    int port1 = get_unique_port();
    int port2 = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    {
        U64 page_size = 4_kb;
        auto pager = create_test_pager(db_file, page_size);
        pager::begin_transaction(pager);
        co_await engine::create_database(pager);
        co_await pager::commit_transaction(pager);
        Engine engine;
        co_await engine::init(engine, &pager);

        run_server_test("test-client", [&](os::Notifier& interrupt) {
            Socket client{socket_open()};
            socket_set_timeout(client, 2000);
            CHECK(socket_connect(client, "127.0.0.1", (U16)port1));

            send_frame(client, create_startup());
            CHECK(recv_frame(client).opcode == 0x02);

            send_frame(client, create_query("CREATE KEYSPACE pks;"));
            CHECK(recv_frame(client).opcode == 0x08);

            send_frame(client, create_query("CREATE TABLE pks.data (id int PRIMARY KEY, val text);"));
            CHECK(recv_frame(client).opcode == 0x08);

            send_frame(client, create_query("INSERT INTO pks.data (id, val) VALUES (99, 'persisted');"));
            CHECK(recv_frame(client).opcode == 0x08);

            os::signal_notify_safe(interrupt);
        }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
            native::run(port1, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
        });
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(db_file);
        Engine engine;
        co_await engine::init(engine, &pager);

        run_server_test("test-client", [&](os::Notifier& interrupt) {
            Socket client{socket_open()};
            socket_set_timeout(client, 2000);
            CHECK(socket_connect(client, "127.0.0.1", (U16)port2));

            send_frame(client, create_startup());
            CHECK(recv_frame(client).opcode == 0x02);

            send_frame(client, create_query("SELECT * FROM pks.data;"));
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
            CHECK(body_contains(resp, "persisted"));

            os::signal_notify_safe(interrupt);
        }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
            native::run(port2, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
        });
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("Native protocol system.local virtual view", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        // system.local
        send_frame(client, create_query("SELECT * FROM system.local;"));
        {
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
            CHECK(body_contains(resp, "local"));
            CHECK(body_contains(resp, "datacenter1"));
            CHECK(body_contains(resp, "rack1"));
            CHECK(body_contains(resp, "cql"));
        }

        // system.peers
        send_frame(client, create_query("SELECT * FROM system.peers;"));
        {
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
            CHECK(body_contains(resp, "peer"));
        }

        // system.peers_v2
        send_frame(client, create_query("SELECT * FROM system.peers_v2;"));
        {
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
        }

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol system_schema virtual views", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE test_ks;"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("CREATE TABLE test_ks.users (id int PRIMARY KEY, name text);"));
        CHECK(recv_frame(client).opcode == 0x08);

        // system_schema.keyspaces
        send_frame(client, create_query("SELECT * FROM system_schema.keyspaces;"));
        {
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
            CHECK(body_contains(resp, "keyspace_name"));
            CHECK(body_contains(resp, "test_ks"));
            CHECK(body_contains(resp, "system_schema"));
            CHECK(body_contains(resp, "system"));
        }

        // system_schema.tables
        send_frame(client, create_query("SELECT * FROM system_schema.tables;"));
        {
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
            CHECK(body_contains(resp, "table_name"));
            CHECK(body_contains(resp, "users"));
            CHECK(body_contains(resp, "keyspaces"));
            CHECK(body_contains(resp, "local"));
        }

        // system_schema.columns
        send_frame(client, create_query("SELECT * FROM system_schema.columns;"));
        {
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
            CHECK(body_contains(resp, "column_name"));
            CHECK(body_contains(resp, "partition_key"));
            CHECK(body_contains(resp, "name"));
            CHECK(body_contains(resp, "clustering"));
        }

        send_frame(client, create_query("DROP TABLE test_ks.users;"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("DROP KEYSPACE test_ks;"));
        CHECK(recv_frame(client).opcode == 0x08);

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
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

PAGER_TEST_CASE("Native protocol collection serialization", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("SELECT * FROM system.local;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);

        // tokens column type option is Set<Varchar>
        CHECK(body_contains_bytes(resp, {0x00, 0x22, 0x00, 0x0D}));

        // tokens set value encodes one-element set containing '0'
        CHECK(body_contains_bytes(resp, {0x00, 0x00, 0x00, 0x09,
                                         0x00, 0x00, 0x00, 0x01,
                                         0x00, 0x00, 0x00, 0x01,
                                         0x30}));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol PREPARE and EXECUTE", "[cql.native][!mayfail]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE prep_ks;"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("CREATE TABLE prep_ks.data (id int PRIMARY KEY, name text, score double);"));
        CHECK(recv_frame(client).opcode == 0x08);

        // PREPARE an INSERT statement with bind markers
        {
            Bytes b;
            b.cql_long_string("INSERT INTO prep_ks.data (id, name, score) VALUES (?, ?, ?)");
            b.prepend_header(0x09, 1);
            send_frame(client, b);

            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0004);
            CHECK(resp.body.size() > 8);

            const uint8_t* p = resp.body.data() + 4;
            uint16_t id_len = (uint16_t(p[0]) << 8) | p[1];
            CHECK(id_len == 8);
            std::vector<uint8_t> prepared_id(p + 2, p + 2 + id_len);

            // EXECUTE the prepared statement with bound values
            {
                Bytes ex;
                ex.u16(uint16_t(prepared_id.size()));
                for (auto byte : prepared_id) ex.u8(byte);
                ex.u16(0x0001);
                ex.u8(0x01);
                ex.u16(3);

                ex.s32(4);
                ex.u8(0); ex.u8(0); ex.u8(0); ex.u8(42);

                ex.s32(5);
                ex.u8('A'); ex.u8('l'); ex.u8('i'); ex.u8('c'); ex.u8('e');

                ex.s32(8);
                uint64_t dbl_bits;
                double dbl_val = 99.5;
                memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
                for (int i = 7; i >= 0; i--) ex.u8(uint8_t(dbl_bits >> (i * 8)));

                ex.prepend_header(0x0A, 2);
                send_frame(client, ex);

                auto exec_resp = recv_frame(client);
                CHECK(exec_resp.opcode == 0x08);
                CHECK(result_kind(exec_resp) == 0x0001);
            }

            send_frame(client, create_query("SELECT * FROM prep_ks.data;"));
            auto select_resp = recv_frame(client);
            CHECK(select_resp.opcode == 0x08);
            CHECK(result_kind(select_resp) == 0x0002);
            CHECK(body_contains(select_resp, "Alice"));
        }

        // PREPARE with named bind markers, then EXECUTE with values in a different order
        {
            Bytes b;
            b.cql_long_string("INSERT INTO prep_ks.data (id, name, score) VALUES (:id, :name, :score)");
            b.prepend_header(0x09, 3);
            send_frame(client, b);

            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0004);

            const uint8_t* p = resp.body.data() + 4;
            uint16_t id_len = (uint16_t(p[0]) << 8) | p[1];
            CHECK(id_len == 8);
            std::vector<uint8_t> prepared_id(p + 2, p + 2 + id_len);

            // EXECUTE with named values in REVERSED order
            {
                Bytes ex;
                ex.u16(uint16_t(prepared_id.size()));
                for (auto byte : prepared_id) ex.u8(byte);
                ex.u16(0x0001);
                ex.u8(0x41);
                ex.u16(3);

                ex.cql_string("score");
                ex.s32(8);
                uint64_t dbl_bits;
                double dbl_val = 77.7;
                memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
                for (int i = 7; i >= 0; i--) ex.u8(uint8_t(dbl_bits >> (i * 8)));

                ex.cql_string("name");
                ex.s32(3);
                ex.u8('B'); ex.u8('o'); ex.u8('b');

                ex.cql_string("id");
                ex.s32(4);
                ex.u8(0); ex.u8(0); ex.u8(0); ex.u8(99);

                ex.prepend_header(0x0A, 4);
                send_frame(client, ex);

                auto exec_resp = recv_frame(client);
                CHECK(exec_resp.opcode == 0x08);
                CHECK(result_kind(exec_resp) == 0x0001);
            }

            send_frame(client, create_query("SELECT * FROM prep_ks.data WHERE id = 99;"));
            auto select_resp = recv_frame(client);
            CHECK(select_resp.opcode == 0x08);
            CHECK(result_kind(select_resp) == 0x0002);
            CHECK(body_contains(select_resp, "Bob"));
        }

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol QUERY with bind values", "[cql.native][!mayfail]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    U64 page_size = 4_kb;
    auto pager = create_test_pager(db_file, page_size);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE qbind_ks;"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("CREATE TABLE qbind_ks.items (id int PRIMARY KEY, label text, weight double);"));
        CHECK(recv_frame(client).opcode == 0x08);

        // positional bind values
        {
            Bytes b;
            b.cql_long_string("INSERT INTO qbind_ks.items (id, label, weight) VALUES (?, ?, ?)");
            b.u16(0x0001);
            b.u8(0x01);
            b.u16(3);
            b.s32(4); b.u8(0); b.u8(0); b.u8(0); b.u8(7);
            b.s32(6); b.u8('G'); b.u8('a'); b.u8('d'); b.u8('g'); b.u8('e'); b.u8('t');
            b.s32(8);
            uint64_t dbl_bits; double dbl_val = 3.14;
            memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
            for (int i = 7; i >= 0; i--) b.u8(uint8_t(dbl_bits >> (i * 8)));
            b.prepend_header(0x07, 1);
            send_frame(client, b);

            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0001);

            send_frame(client, create_query("SELECT * FROM qbind_ks.items;"));
            auto sel = recv_frame(client);
            CHECK(sel.opcode == 0x08);
            CHECK(result_kind(sel) == 0x0002);
            CHECK(body_contains(sel, "Gadget"));
        }

        // named bind values
        {
            Bytes b;
            b.cql_long_string("INSERT INTO qbind_ks.items (id, label, weight) VALUES (:id, :label, :weight)");
            b.u16(0x0001);
            b.u8(0x41);
            b.u16(3);
            b.cql_string("weight");
            b.s32(8);
            uint64_t dbl_bits; double dbl_val = 2.71;
            memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
            for (int i = 7; i >= 0; i--) b.u8(uint8_t(dbl_bits >> (i * 8)));
            b.cql_string("id");
            b.s32(4); b.u8(0); b.u8(0); b.u8(0); b.u8(13);
            b.cql_string("label");
            b.s32(5); b.u8('G'); b.u8('i'); b.u8('z'); b.u8('m'); b.u8('o');
            b.prepend_header(0x07, 2);
            send_frame(client, b);

            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0001);

            send_frame(client, create_query("SELECT * FROM qbind_ks.items WHERE id = 13;"));
            auto sel = recv_frame(client);
            CHECK(sel.opcode == 0x08);
            CHECK(result_kind(sel) == 0x0002);
            CHECK(body_contains(sel, "Gizmo"));
        }

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol UPDATE modifies existing row", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    auto pager = create_test_pager(db_file, 4_kb);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));
        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("CREATE TABLE ks.t (id int PRIMARY KEY, name text);"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("INSERT INTO ks.t (id, name) VALUES (1, 'Alice');"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("UPDATE ks.t SET name = 'Bob' WHERE id = 1;"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("SELECT * FROM ks.t;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK( body_contains(resp, "Bob"));
        CHECK(!body_contains(resp, "Alice"));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol DELETE removes a row by primary key", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    auto pager = create_test_pager(db_file, 4_kb);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));
        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("CREATE TABLE ks.t (id int PRIMARY KEY, name text);"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("INSERT INTO ks.t (id, name) VALUES (1, 'Alice');"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("INSERT INTO ks.t (id, name) VALUES (2, 'Bob');"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("DELETE FROM ks.t WHERE id = 1;"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("SELECT * FROM ks.t;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK( body_contains(resp, "Bob"));
        CHECK(!body_contains(resp, "Alice"));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol TRUNCATE clears all rows", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    auto pager = create_test_pager(db_file, 4_kb);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));
        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("CREATE TABLE ks.t (id int PRIMARY KEY, name text);"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("INSERT INTO ks.t (id, name) VALUES (1, 'Alice');"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("INSERT INTO ks.t (id, name) VALUES (2, 'Bob');"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("TRUNCATE TABLE ks.t;"));
        CHECK(recv_frame(client).opcode == 0x08);

        send_frame(client, create_query("SELECT * FROM ks.t;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(!body_contains(resp, "Alice"));
        CHECK(!body_contains(resp, "Bob"));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("Native protocol data persists across WAL-enabled restart", "[cql.native]") {
    int port1 = get_unique_port();
    int port2 = get_unique_port();
    os::Handle pid = os::process_get_handle();
    auto db_path  = fmt("/tmp/plexdb_cql::wal_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_cql::wal_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        auto pager = create_test_pager(db, wal, 4_kb);
        pager::begin_transaction(pager);
        co_await engine::create_database(pager);
        co_await pager::commit_transaction(pager);
        Engine eng;
        co_await engine::init(eng, &pager);

        run_server_test("test-client", [&](os::Notifier& interrupt) {
            Socket client{socket_open()};
            socket_set_timeout(client, 2000);
            CHECK(socket_connect(client, "127.0.0.1", (U16)port1));
            send_frame(client, create_startup());
            CHECK(recv_frame(client).opcode == 0x02);

            send_frame(client, create_query("CREATE KEYSPACE wks;"));
            CHECK(recv_frame(client).opcode == 0x08);
            send_frame(client, create_query("CREATE TABLE wks.entries (id int PRIMARY KEY, msg text);"));
            CHECK(recv_frame(client).opcode == 0x08);
            send_frame(client, create_query("INSERT INTO wks.entries (id, msg) VALUES (1, 'durable');"));
            CHECK(recv_frame(client).opcode == 0x08);

            os::signal_notify_safe(interrupt);
        }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
            native::run(port1, eng, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
        });
        destroy_test_pager(pager);
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        auto pager = test_pager(db, wal);
        Engine eng;
        co_await engine::init(eng, &pager);

        run_server_test("test-client", [&](os::Notifier& interrupt) {
            Socket client{socket_open()};
            socket_set_timeout(client, 2000);
            CHECK(socket_connect(client, "127.0.0.1", (U16)port2));
            send_frame(client, create_startup());
            CHECK(recv_frame(client).opcode == 0x02);

            send_frame(client, create_query("SELECT * FROM wks.entries;"));
            auto resp = recv_frame(client);
            CHECK(resp.opcode == 0x08);
            CHECK(result_kind(resp) == 0x0002);
            CHECK(body_contains(resp, "durable"));

            os::signal_notify_safe(interrupt);
        }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
            native::run(port2, eng, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
        });
        destroy_test_pager(pager);
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("crash consistency after SIGKILL during writes", "[cql.native]") {
    os::Handle pid = os::process_get_handle();
    auto db_path  = fmt("/tmp/plexdb_cql::crash_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_cql::crash_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    int port1 = get_unique_port();
    int port2 = get_unique_port();

    // Number of inserts to confirm (receive ACK) before killing the server.
    // The kill arrives while one additional in-flight insert is being processed.
    int confirmed_count = GENERATE(take(1, random(5, 20)));

    os::Notifier ready{};
    auto child_opt = os::process_fork();
    REQUIRE(static_cast<bool>(child_opt));
    os::Handle child = *child_opt;

    if (os::is_zero_handle(child)) {
        // Child: create WAL-enabled pager, initialise engine, run native server.
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        auto pager = create_test_pager(db, wal, 4_kb);
        pager::begin_transaction(pager);
        co_await engine::create_database(pager);
        co_await pager::commit_transaction(pager);
        Engine eng;
        co_await engine::init(eng, &pager);

        os::Poll poll{};
        os::Notifier interrupt{};
        auto signal_consumer = aio::create_notifier_consumer(interrupt, poll);
        native::run(static_cast<U16>(port1), eng,
                    [&ready]{ os::signal_notify_safe(ready); },
                    false, g_test_sync_consumer, signal_consumer, poll);
        os::process_exit(0);
    }

    // Parent: wait for server ready signal.
    { U8 b; os::stream_read(ready.read, &b, 1); }

    {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port1));

        send_frame(client, create_startup());
        REQUIRE(recv_frame(client).opcode == 0x02);

        // DDL: wait for full ACK so schema is durable before the kill phase.
        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        REQUIRE(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query("CREATE TABLE ks.t (id int PRIMARY KEY, val text);"));
        REQUIRE(recv_frame(client).opcode == 0x08);

        // Send confirmed_count inserts and collect each ACK — these are durably committed.
        for (int i = 0; i < confirmed_count; i++) {
            std::string q = "INSERT INTO ks.t (id, val) VALUES ("
                          + std::to_string(i) + ", 'row_" + std::to_string(i) + "');";
            send_frame(client, create_query(q.c_str()));
            REQUIRE(recv_frame(client).opcode == 0x08);
        }

        // Send one more insert without waiting — the kill lands while the server
        // is in the middle of committing (or just after), so this row may or may
        // not survive. We don't assert on it either way.
        {
            std::string q = "INSERT INTO ks.t (id, val) VALUES ("
                          + std::to_string(confirmed_count) + ", 'row_"
                          + std::to_string(confirmed_count) + "');";
            send_frame(client, create_query(q.c_str()));
        }
    }  // socket closes here

    os::signal_send_kill(child);
    os::process_wait(child);

    // Reopen: WAL recovery replays any committed frames that didn't reach the DB file.
    os::Handle db  = os::file_open(db_path);
    os::Handle wal = os::file_open(wal_path);
    auto pager = test_pager(db, wal);
    Engine eng2;
    co_await engine::init(eng2, &pager);

    run_server_test("verifier", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port2));

        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        // DB must be queryable (schema intact, no structural corruption).
        send_frame(client, create_query("SELECT * FROM ks.t;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);

        // Every ACKed insert must be present — WAL guarantees their durability.
        for (int i = 0; i < confirmed_count; i++) {
            std::string val = "row_" + std::to_string(i);
            CHECK(body_contains(resp, val.c_str()));
        }

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port2, eng2, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });

    destroy_test_pager(pager);
    os::file_close(db);
    os::file_close(wal);
    os::file_delete(db_path);
    os::file_delete(wal_path);
}

// ============================================================================
// Static column tests
// ============================================================================

// Static column value is shared by all clustering rows in the same partition.
PAGER_TEST_CASE("Static columns: value shared across clustering rows", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    auto pager = create_test_pager(db_file, 4_kb);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));
        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        CHECK(recv_frame(client).opcode == 0x08);
        // Table with partition key (country), clustering key (name), and a static column (flag).
        send_frame(client, create_query(
            "CREATE TABLE ks.cyclists ("
            "  country text,"
            "  name    text,"
            "  flag    text STATIC,"
            "  PRIMARY KEY (country, name)"
            ");"));
        CHECK(recv_frame(client).opcode == 0x08);

        // Insert two clustering rows for the same partition, setting flag on the first.
        send_frame(client, create_query(
            "INSERT INTO ks.cyclists (country, name, flag) VALUES ('Belgium', 'Boonen', 'BE');"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query(
            "INSERT INTO ks.cyclists (country, name) VALUES ('Belgium', 'Steels');"));
        CHECK(recv_frame(client).opcode == 0x08);

        // Both rows should see 'BE' in the flag column.
        send_frame(client, create_query("SELECT * FROM ks.cyclists WHERE country = 'Belgium';"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "Boonen"));
        CHECK(body_contains(resp, "Steels"));
        // Flag 'BE' appears — at least once per returned row.
        CHECK(body_contains(resp, "BE"));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

// Inserting a new static value overwrites the previous one partition-wide.
PAGER_TEST_CASE("Static columns: overwrite replaces value partition-wide", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    auto pager = create_test_pager(db_file, 4_kb);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));
        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        CHECK(recv_frame(client).opcode == 0x08);
        send_frame(client, create_query(
            "CREATE TABLE ks.t (pk int, ck int, info text STATIC, PRIMARY KEY (pk, ck));"));
        CHECK(recv_frame(client).opcode == 0x08);

        // First insert sets info = 'v1'.
        send_frame(client, create_query("INSERT INTO ks.t (pk, ck, info) VALUES (1, 10, 'v1');"));
        CHECK(recv_frame(client).opcode == 0x08);

        // Second insert on the same partition updates info = 'v2'.
        send_frame(client, create_query("INSERT INTO ks.t (pk, ck, info) VALUES (1, 20, 'v2');"));
        CHECK(recv_frame(client).opcode == 0x08);

        // Both rows should see 'v2'; 'v1' should no longer appear.
        send_frame(client, create_query("SELECT * FROM ks.t WHERE pk = 1;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK( body_contains(resp, "v2"));
        CHECK(!body_contains(resp, "v1"));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}

// Static values must persist across an engine restart (reopen from disk).
PAGER_TEST_CASE("Static columns: persist across reopen", "[cql.native]") {
    int port1 = get_unique_port();
    int port2 = get_unique_port();

    os::Handle pid = os::process_get_handle();
    auto db_path  = fmt("/tmp/plexdb_cql::static_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_cql::static_%" PRIu64 "_wal", pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    os::Handle db_file  = os::file_open(db_path);
    os::Handle wal_file = os::file_open(wal_path);
    REQUIRE(!os::is_zero_handle(db_file));
    REQUIRE(!os::is_zero_handle(wal_file));

    {
        auto pager = create_test_pager(db_file, wal_file, 4_kb);
        pager::begin_transaction(pager);
        co_await engine::create_database(pager);
        co_await pager::commit_transaction(pager);
        Engine engine;
        co_await engine::init(engine, &pager);

        run_server_test("writer", [&](os::Notifier& interrupt) {
            Socket client{socket_open()};
            socket_set_timeout(client, 2000);
            CHECK(socket_connect(client, "127.0.0.1", (U16)port1));
            send_frame(client, create_startup());
            CHECK(recv_frame(client).opcode == 0x02);

            send_frame(client, create_query("CREATE KEYSPACE ks;"));
            CHECK(recv_frame(client).opcode == 0x08);
            send_frame(client, create_query(
                "CREATE TABLE ks.t (pk int, ck int, info text STATIC, PRIMARY KEY (pk, ck));"));
            CHECK(recv_frame(client).opcode == 0x08);
            send_frame(client, create_query("INSERT INTO ks.t (pk, ck, info) VALUES (7, 1, 'persistent');"));
            CHECK(recv_frame(client).opcode == 0x08);

            os::signal_notify_safe(interrupt);
        }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
            native::run(port1, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
        });
        destroy_test_pager(pager);
    }

    // Reopen
    os::Handle db2  = os::file_open(db_path);
    os::Handle wal2 = os::file_open(wal_path);
    auto pager2 = test_pager(db2, wal2);
    Engine engine2;
    co_await engine::init(engine2, &pager2);

    run_server_test("reader", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port2));
        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("SELECT * FROM ks.t WHERE pk = 7;"));
        auto resp = recv_frame(client);
        CHECK(resp.opcode == 0x08);
        CHECK(result_kind(resp) == 0x0002);
        CHECK(body_contains(resp, "persistent"));

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port2, engine2, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager2);
    os::file_close(db2);
    os::file_close(wal2);
    os::file_delete(db_path);
    os::file_delete(wal_path);
}

// DDL: static column on a table without clustering columns must be rejected.
PAGER_TEST_CASE("Static columns: rejected on table without clustering key", "[cql.native]") {
    int port = get_unique_port();
    os::File db_file{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db_file));

    auto pager = create_test_pager(db_file, 4_kb);
    pager::begin_transaction(pager);
    co_await engine::create_database(pager);
    co_await pager::commit_transaction(pager);
    Engine engine;
    co_await engine::init(engine, &pager);

    run_server_test("test-client", [&](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));
        send_frame(client, create_startup());
        CHECK(recv_frame(client).opcode == 0x02);

        send_frame(client, create_query("CREATE KEYSPACE ks;"));
        CHECK(recv_frame(client).opcode == 0x08);
        // No clustering column — STATIC should be rejected.
        send_frame(client, create_query(
            "CREATE TABLE ks.bad (id int PRIMARY KEY, info text STATIC);"));
        auto resp = recv_frame(client);
        // Expect an error response (opcode 0x00 = ERROR).
        CHECK(resp.opcode == 0x00);

        os::signal_notify_safe(interrupt);
    }, [&](auto on_ready, auto& signal_consumer, auto& poll) {
        native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll);
    });
    destroy_test_pager(pager);
}
