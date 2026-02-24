#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

import plexdb.base;
import plexdb.os;

using namespace plexdb;
using namespace plexdb::os;

namespace {
    constexpr int EL_TEST_PORT_BASE = 22000;
    std::atomic<int> el_port_counter{0};

    int get_unique_port() {
        return EL_TEST_PORT_BASE + el_port_counter.fetch_add(1);
    }

    int create_listen_socket(int port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        bind(fd, (sockaddr*)&addr, sizeof(addr));
        listen(fd, 16);
        return fd;
    }
}

TEST_CASE("event_loop create and destroy", "[plexdb.os.event_loop]") {
    SignalPipe sp = signal_pipe_create();
    EventLoop loop = event_loop_create(64, sp.read_fd);
    event_loop_destroy(loop);
    signal_pipe_destroy(sp);
}

TEST_CASE("event_loop drain returns zero when empty", "[plexdb.os.event_loop]") {
    SignalPipe sp = signal_pipe_create();
    EventLoop loop = event_loop_create(64, sp.read_fd);

    CompletionEvent events[8];
    int count = event_loop_drain(loop, events, 8);
    REQUIRE(count == 0);

    event_loop_destroy(loop);
    signal_pipe_destroy(sp);
}

TEST_CASE("event_loop wait unblocks on signal", "[plexdb.os.event_loop]") {
    SignalPipe sp = signal_pipe_create();
    EventLoop loop = event_loop_create(64, sp.read_fd);

    std::atomic<bool> waiting{false};
    std::atomic<bool> done{false};

    std::thread t([&]() {
        waiting = true;
        event_loop_wait(loop);
        done = true;
    });

    while (!waiting) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(!done);
    signal_pipe_notify(sp);

    auto start = std::chrono::steady_clock::now();
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }
    REQUIRE(done);

    t.join();
    event_loop_destroy(loop);
    signal_pipe_destroy(sp);
}

TEST_CASE("event_loop accept completion", "[plexdb.os.event_loop]") {
    int port = get_unique_port();
    SignalPipe sp = signal_pipe_create();
    EventLoop loop = event_loop_create(64, sp.read_fd);

    int listen_fd = create_listen_socket(port);
    U64 accept_tag = 0xA0;

    event_loop_submit_accept(loop, listen_fd, accept_tag);
    event_loop_flush(loop);

    // connect a client
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    int res = connect(client_fd, (sockaddr*)&addr, sizeof(addr));
    REQUIRE(res == 0);

    // drain should give us the accept completion
    CompletionEvent events[8];
    int count = 0;
    auto start = std::chrono::steady_clock::now();
    while (count == 0) {
        count = event_loop_drain(loop, events, 8);
        if (count == 0) event_loop_wait(loop);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }

    REQUIRE(count >= 1);
    REQUIRE(events[0].user_data == accept_tag);
    REQUIRE(events[0].result >= 0);

    close(events[0].result);
    close(client_fd);
    close(listen_fd);
    event_loop_destroy(loop);
    signal_pipe_destroy(sp);
}

TEST_CASE("event_loop read and write completions", "[plexdb.os.event_loop]") {
    int port = get_unique_port();
    SignalPipe sp = signal_pipe_create();
    EventLoop loop = event_loop_create(64, sp.read_fd);

    int listen_fd = create_listen_socket(port);

    // register a buffer
    char buf[256] = {0};
    IoBuffer io_buf{ buf, sizeof(buf) };
    event_loop_register_buffers(loop, &io_buf, 1);

    // submit accept
    U64 accept_tag = 0xA0;
    event_loop_submit_accept(loop, listen_fd, accept_tag);
    event_loop_flush(loop);

    // client connects and sends data
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    connect(client_fd, (sockaddr*)&addr, sizeof(addr));

    // get accept completion
    CompletionEvent events[8];
    int count = 0;
    auto start = std::chrono::steady_clock::now();
    while (count == 0) {
        count = event_loop_drain(loop, events, 8);
        if (count == 0) event_loop_wait(loop);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }
    REQUIRE(count >= 1);
    int server_fd = events[0].result;
    REQUIRE(server_fd >= 0);

    // submit read on accepted connection
    U64 read_tag = 0xB0;
    event_loop_submit_read(loop, server_fd, buf, sizeof(buf), 0, read_tag);
    event_loop_flush(loop);

    // send data from client
    const char* msg = "HELLO";
    send(client_fd, msg, 5, 0);

    // get read completion
    count = 0;
    start = std::chrono::steady_clock::now();
    while (count == 0) {
        count = event_loop_drain(loop, events, 8);
        if (count == 0) event_loop_wait(loop);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }
    REQUIRE(count >= 1);
    REQUIRE(events[0].user_data == read_tag);
    REQUIRE(events[0].result == 5);
    REQUIRE(buf[0] == 'H');
    REQUIRE(buf[4] == 'O');

    // submit write back to client using the registered buffer
    const char* reply = "WORLD";
    for (int i = 0; i < 5; i++) buf[i] = reply[i];

    U64 write_tag = 0xC0;
    event_loop_submit_write(loop, server_fd, buf, 5, 0, write_tag);
    event_loop_flush(loop);

    count = 0;
    start = std::chrono::steady_clock::now();
    while (count == 0) {
        count = event_loop_drain(loop, events, 8);
        if (count == 0) event_loop_wait(loop);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }
    REQUIRE(count >= 1);
    REQUIRE(events[0].user_data == write_tag);
    REQUIRE(events[0].result == 5);

    // verify client received the data
    char recv_buf[256] = {0};
    int bytes = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
    REQUIRE(bytes == 5);
    REQUIRE(recv_buf[0] == 'W');

    event_loop_unregister_buffers(loop);
    close(server_fd);
    close(client_fd);
    close(listen_fd);
    event_loop_destroy(loop);
    signal_pipe_destroy(sp);
}

TEST_CASE("event_loop close completion", "[plexdb.os.event_loop]") {
    int port = get_unique_port();
    SignalPipe sp = signal_pipe_create();
    EventLoop loop = event_loop_create(64, sp.read_fd);

    int listen_fd = create_listen_socket(port);

    event_loop_submit_accept(loop, listen_fd, 0xA0);
    event_loop_flush(loop);

    // connect a client
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    connect(client_fd, (sockaddr*)&addr, sizeof(addr));

    CompletionEvent events[8];
    int count = 0;
    auto start = std::chrono::steady_clock::now();
    while (count == 0) {
        count = event_loop_drain(loop, events, 8);
        if (count == 0) event_loop_wait(loop);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }
    REQUIRE(count >= 1);
    int server_fd = events[0].result;

    // submit close
    U64 close_tag = 0xD0;
    event_loop_submit_close(loop, server_fd, close_tag);
    event_loop_flush(loop);

    count = 0;
    start = std::chrono::steady_clock::now();
    while (count == 0) {
        count = event_loop_drain(loop, events, 8);
        if (count == 0) event_loop_wait(loop);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }
    REQUIRE(count >= 1);
    REQUIRE(events[0].user_data == close_tag);

    close(client_fd);
    close(listen_fd);
    event_loop_destroy(loop);
    signal_pipe_destroy(sp);
}
