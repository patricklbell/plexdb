#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <atomic>
#include <initializer_list>
#include <cstring>

import plexdb.base;
import plexdb.os;
import plexdb.threads;
import plexdb.aio;
import plexdb.server.test_helpers;

import keyvalue.engine;
import keyvalue.resp;

using namespace plexdb;
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
    void send_cmd(os::Socket& sock, String8 s) {
        os::socket_send_all(sock, s, s.length);
    }

    AutoString8 make_resp(std::initializer_list<const char*> args) {
        AutoString8 out;
        out += "*";
        out += to_str(args.size());
        out += "\r\n";
        for (const char* a : args) {
            size_t n = ::strlen(a);
            out += "$";
            out += to_str(n);
            out += "\r\n";
            out += a;
            out += "\r\n";
        }
        return out;
    }

    AutoString8 recv_line(os::Socket& sock) {
        AutoString8 buf;
        char c = 0;
        while (true) {
            auto res = os::socket_receive(sock, &c, 1);
            if (res.byte_count <= 0) break;
            push_back(buf, c);
            if (buf.length >= 2 && buf[buf.length-2] == '\r' && buf[buf.length-1] == '\n') {
                resize(buf, buf.length - 2);
                break;
            }
        }
        return buf;
    }

    AutoString8 recv_n(os::Socket& sock, size_t n) {
        AutoString8 buf(n);
        size_t got = 0;
        while (got < n) {
            auto res = os::socket_receive(sock, &buf.c_str[got], (int)(n - got));
            if (res.byte_count <= 0) break;
            got += bounds_checked_cast<size_t>(res.byte_count);
        }
        return buf;
    }

    struct Resp {
        char type = 0;
        AutoString8 str;
        S64 integer = 0;
        DynamicArray<Resp> array;
        bool is_null = false;
    };

    Resp recv_resp(os::Socket& sock) {
        Resp r;
        AutoString8 line = recv_line(sock);
        if (line.length == 0) return r;
        r.type = line[0];
        String8 body{line.c_str + 1, line.length - 1};

        switch (r.type) {
            case '+': r.str = body; break;
            case '-': r.str = body; break;
            case ':': r.integer = s64_from_str(body); break;
            case '$': {
                S64 n = s64_from_str(body);
                if (n < 0) { r.is_null = true; break; }
                r.str = recv_n(sock, (size_t)n);
                recv_line(sock);
                break;
            }
            case '*': {
                S64 count = s64_from_str(body);
                if (count < 0) { r.is_null = true; break; }
                for (S64 i = 0; i < count; i++)
                    push_back(r.array, recv_resp(sock));
                break;
            }
        }
        return r;
    }

    template<typename ClientFn>
    void run_test(int port, ClientFn&& client_fn) {
        engine::InMemoryEngine eng{};
        run_server_test(
            "resp-client",
            forward<ClientFn>(client_fn),
            [&](auto on_ready, auto& signal_consumer, auto& poll) {
                resp::run((U16)port, eng, on_ready, false, signal_consumer, poll);
            }
        );
    }
}

TEST_CASE("RESP PING", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        auto cmd = make_resp({"PING"});
        send_cmd(client, cmd);
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "PONG");

        cmd = make_resp({"PING", "hello"});
        send_cmd(client, cmd);
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.str  == "hello");

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP SET and GET", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"SET", "foo", "bar"}));
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        send_cmd(client, make_resp({"GET", "foo"}));
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.str  == "bar");

        send_cmd(client, make_resp({"GET", "nosuchkey"}));
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.is_null);

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP DEL and EXISTS", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"SET", "k1", "v1"}));
        recv_resp(client);
        send_cmd(client, make_resp({"SET", "k2", "v2"}));
        recv_resp(client);

        send_cmd(client, make_resp({"EXISTS", "k1", "k2", "k3"}));
        auto r = recv_resp(client);
        CHECK(r.type == ':');
        CHECK(r.integer == 2);

        send_cmd(client, make_resp({"DEL", "k1", "k3"}));
        r = recv_resp(client);
        CHECK(r.type == ':');
        CHECK(r.integer == 1);

        send_cmd(client, make_resp({"EXISTS", "k1"}));
        r = recv_resp(client);
        CHECK(r.type == ':');
        CHECK(r.integer == 0);

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP MSET and MGET", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"MSET", "a", "1", "b", "2", "c", "3"}));
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        send_cmd(client, make_resp({"MGET", "a", "b", "missing", "c"}));
        r = recv_resp(client);
        CHECK(r.type == '*');
        REQUIRE(r.array.length == 4);
        CHECK(r.array[0].str == "1");
        CHECK(r.array[1].str == "2");
        CHECK(r.array[2].is_null);
        CHECK(r.array[3].str == "3");

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP KEYS and SCAN", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"MSET", "user:1", "alice", "user:2", "bob", "other", "x"}));
        recv_resp(client);

        send_cmd(client, make_resp({"KEYS", "user:*"}));
        auto r = recv_resp(client);
        CHECK(r.type == '*');
        CHECK(r.array.length == 2);

        send_cmd(client, make_resp({"SCAN", "0", "COUNT", "100"}));
        r = recv_resp(client);
        CHECK(r.type == '*');
        REQUIRE(r.array.length == 2);
        CHECK(r.array[0].str  == "0");
        CHECK(r.array[1].array.length == 3);

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP SET NX and XX flags", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"SET", "nx_key", "first", "NX"}));
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        send_cmd(client, make_resp({"SET", "nx_key", "second", "NX"}));
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.is_null);

        send_cmd(client, make_resp({"GET", "nx_key"}));
        r = recv_resp(client);
        CHECK(r.str == "first");

        send_cmd(client, make_resp({"SET", "new_key", "val", "XX"}));
        r = recv_resp(client);
        CHECK(r.type == '$');
        CHECK(r.is_null);

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP QUIT closes connection", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, make_resp({"QUIT"}));
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "OK");

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP pipelining", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        AutoString8 batch;
        batch += make_resp({"SET", "pipe1", "a"});
        batch += make_resp({"SET", "pipe2", "b"});
        batch += make_resp({"MGET", "pipe1", "pipe2"});
        send_cmd(client, batch);

        auto r1 = recv_resp(client);
        CHECK(r1.type == '+');
        auto r2 = recv_resp(client);
        CHECK(r2.type == '+');
        auto r3 = recv_resp(client);
        CHECK(r3.type == '*');
        REQUIRE(r3.array.length == 2);
        CHECK(r3.array[0].str == "a");
        CHECK(r3.array[1].str == "b");

        os::signal_notify_safe(interrupt);
    });
}

TEST_CASE("RESP inline command", "[keyvalue.resp]") {
    int port = get_unique_port();
    run_test(port, [port](os::Notifier& interrupt) {
        os::Socket client{os::socket_open()};
        os::socket_set_timeout(client, 2000);
        CHECK(os::socket_connect(client, "127.0.0.1", (U16)port));

        send_cmd(client, "PING\r\n");
        auto r = recv_resp(client);
        CHECK(r.type == '+');
        CHECK(r.str  == "PONG");

        os::signal_notify_safe(interrupt);
    });
}
