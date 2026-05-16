#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <atomic>
#include <vector>
#include <string>
#include <cstring>

import plexdb.base;
import plexdb.os;
import plexdb.threads;
import plexdb.aio;
import plexdb.server.test_helpers;

import keyvalue.store;
import keyvalue.resp;
import keyvalue.resp.protocol;

using namespace plexdb;
using namespace plexdb::os;

using namespace keyvalue;

namespace {
    constexpr int RESP_TEST_PORT_BASE = 25000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return RESP_TEST_PORT_BASE + port_counter.fetch_add(1);
    }

    // ========================================================================
    // client helpers
    // ========================================================================
    void send_cmd(Socket& sock, const char* s) {
        socket_send_all(sock, s, strlen(s));
    }

    std::string make_resp(std::initializer_list<const char*> args) {
        std::string out;
        out += "*";
        out += std::to_string(args.size());
        out += "\r\n";
        for (const char* a : args) {
            size_t n = strlen(a);
            out += "$";
            out += std::to_string(n);
            out += "\r\n";
            out += a;
            out += "\r\n";
        }
        return out;
    }

    std::string recv_line(Socket& sock) {
        std::string buf;
        char c = 0;
        while (true) {
            auto res = socket_receive(sock, &c, 1);
            if (res.byte_count <= 0) break;
            buf += c;
            if (buf.size() >= 2 && buf[buf.size()-2] == '\r' && buf[buf.size()-1] == '\n') {
                buf.resize(buf.size() - 2);
                break;
            }
        }
        return buf;
    }

    std::string recv_n(Socket& sock, size_t n) {
        std::string buf(n, '\0');
        size_t got = 0;
        while (got < n) {
            auto res = socket_receive(sock, buf.data() + got, (int)(n - got));
            if (res.byte_count <= 0) break;
            got += (size_t)res.byte_count;
        }
        return buf;
    }

    struct Resp {
        char type = 0;
        std::string str;
        int64_t integer = 0;
        std::vector<Resp> array;
        bool is_null = false;
    };

    Resp recv_resp(Socket& sock) {
        Resp r;
        std::string line = recv_line(sock);
        if (line.empty()) return r;
        r.type = line[0];
        std::string body = line.substr(1);

        switch (r.type) {
            case '+': r.str = body; break;
            case '-': r.str = body; break;
            case ':': r.integer = std::stoll(body); break;
            case '$': {
                int64_t n = std::stoll(body);
                if (n < 0) { r.is_null = true; break; }
                r.str = recv_n(sock, (size_t)n);
                recv_line(sock);
                break;
            }
            case '*': {
                int64_t count = std::stoll(body);
                if (count < 0) { r.is_null = true; break; }
                for (int64_t i = 0; i < count; i++)
                    r.array.push_back(recv_resp(sock));
                break;
            }
        }
        return r;
    }

    template<typename ClientFn>
    void run_test(int port, ClientFn&& client_fn) {
        store::Store store{};
        run_server_test("resp-client", std::forward<ClientFn>(client_fn),
            [&](auto on_ready, auto& signal_consumer, auto& poll) {
                resp::run((U16)port, store, on_ready, false, signal_consumer, poll);
            });
    }
}

// ============================================================================
TEST_CASE("RESP PING", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        auto cmd = make_resp({"PING"});
        send_cmd(client, cmd.c_str());
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "PONG");

        cmd = make_resp({"PING", "hello"});
        send_cmd(client, cmd.c_str());
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.str  == "hello");

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP SET and GET", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"SET", "foo", "bar"}).c_str());
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        send_cmd(client, make_resp({"GET", "foo"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.str  == "bar");

        send_cmd(client, make_resp({"GET", "nosuchkey"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.is_null);

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP DEL and EXISTS", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"SET", "k1", "v1"}).c_str());
        recv_resp(client);
        send_cmd(client, make_resp({"SET", "k2", "v2"}).c_str());
        recv_resp(client);

        send_cmd(client, make_resp({"EXISTS", "k1", "k2", "k3"}).c_str());
        auto r = recv_resp(client);
        CHECK(r.type == ':');
        CHECK(r.integer == 2);

        send_cmd(client, make_resp({"DEL", "k1", "k3"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == ':');
        CHECK(r.integer == 1);

        send_cmd(client, make_resp({"EXISTS", "k1"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == ':');
        CHECK(r.integer == 0);

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP MSET and MGET", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"MSET", "a", "1", "b", "2", "c", "3"}).c_str());
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        send_cmd(client, make_resp({"MGET", "a", "b", "missing", "c"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == '*');
        REQUIRE(r.array.size() == 4);
        CHECK(r.array[0].str == "1");
        CHECK(r.array[1].str == "2");
        CHECK(r.array[2].is_null);
        CHECK(r.array[3].str == "3");

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP KEYS and SCAN", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"MSET", "user:1", "alice", "user:2", "bob", "other", "x"}).c_str());
        recv_resp(client);

        send_cmd(client, make_resp({"KEYS", "user:*"}).c_str());
        auto r = recv_resp(client);
        CHECK(r.type == '*');
        CHECK(r.array.size() == 2);

        send_cmd(client, make_resp({"SCAN", "0", "COUNT", "100"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == '*');
        REQUIRE(r.array.size() == 2);
        CHECK(r.array[0].str  == "0");
        CHECK(r.array[1].array.size() == 3);

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP SET NX and XX flags", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"SET", "nx_key", "first", "NX"}).c_str());
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        send_cmd(client, make_resp({"SET", "nx_key", "second", "NX"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.is_null);

        send_cmd(client, make_resp({"GET", "nx_key"}).c_str());
        r = recv_resp(client);
        CHECK(r.str == "first");

        send_cmd(client, make_resp({"SET", "new_key", "val", "XX"}).c_str());
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.is_null);

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP QUIT closes connection", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"QUIT"}).c_str());
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP pipelining", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        std::string batch;
        batch += make_resp({"SET", "pipe1", "a"});
        batch += make_resp({"SET", "pipe2", "b"});
        batch += make_resp({"MGET", "pipe1", "pipe2"});
        send_cmd(client, batch.c_str());

        auto r1 = recv_resp(client);
        CHECK(r1.type == '+');
        auto r2 = recv_resp(client);
        CHECK(r2.type == '+');
        auto r3 = recv_resp(client);
        CHECK(r3.type == '*');
        REQUIRE(r3.array.size() == 2);
        CHECK(r3.array[0].str == "a");
        CHECK(r3.array[1].str == "b");

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP inline command", "[resp.server]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        Socket client{socket_open()};
        socket_set_timeout(client, 2000);
        CHECK(socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, "PING\r\n");
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "PONG");

        os::signal_notify_safe(interrupt);
    });
}

// ============================================================================
TEST_CASE("RESP protocol: parse round-trip", "[resp.protocol]") {
    using namespace keyvalue::resp::protocol;

    DynamicArray<U8> buf;

    append_simple_string(buf, "OK");
    append_error(buf, "ERR", "msg");
    append_integer(buf, 42);
    append_bulk_string(buf, "hello");
    append_null_bulk_string(buf);
    append_array_header(buf, 3);

    bool found_plus  = false, found_minus = false, found_colon = false;
    bool found_dollar= false, found_star  = false;
    for (U64 i = 0; i < buf.length; i++) {
        switch (buf[i]) {
            case '+': found_plus   = true; break;
            case '-': found_minus  = true; break;
            case ':': found_colon  = true; break;
            case '$': found_dollar = true; break;
            case '*': found_star   = true; break;
        }
    }
    CHECK(found_plus);
    CHECK(found_minus);
    CHECK(found_colon);
    CHECK(found_dollar);
    CHECK(found_star);
}
