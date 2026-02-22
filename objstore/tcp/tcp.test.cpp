#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "macros.h"

#include <thread>
#include <chrono>
#include <atomic>

#if PLEXDB_OS_LINUX
#include <unistd.h>
#endif

import plexdb.base;
import plexdb.os;

import objstore.tcp;

using namespace plexdb;
using namespace plexdb::os;
using namespace objstore::tcp;

namespace {
    constexpr int TEST_PORT_BASE = 19000;
    std::atomic<int> port_counter{0};

    int get_unique_port() {
        return TEST_PORT_BASE + port_counter.fetch_add(1);
    }
}

TEST_CASE("Socket simple client-server exchange", "[objstore.tcp]") {
    int port = get_unique_port();
    
    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};
    AutoString8 received_data;
    
    std::thread server_thread([port, &server_ready, &server_done, &received_data]() {
        Socket listen_sock{socket_create_tcp()};
        socket_set_option(listen_sock, SocketOption::ReuseAddress, true);
        socket_bind(listen_sock, (U16)port);
        socket_listen(listen_sock, 1);
        socket_set_timeout(listen_sock, 2000);
        
        server_ready = true;
        
        Handle client_fd = socket_accept(listen_sock);
        if (socket_valid(client_fd)) {
            Socket client{client_fd};
            socket_set_timeout(client, 1000);
            
            char buffer[256];
            auto res = socket_receive(client, buffer, sizeof(buffer) - 1);
            if (res.bytes > 0) {
                buffer[res.bytes] = '\0';
                received_data = AutoString8{buffer};
                
                String8 response = "PONG";
                socket_send(client, response.data, response.length);
            }
        }
        server_done = true;
    });
    
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    Socket client{socket_create_tcp()};
    socket_set_timeout(client, 1000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
    
    String8 message = "PING";
    auto send_res = socket_send_all(client, message.data, message.length);
    REQUIRE(send_res.error == SocketError::None);
    
    char buffer[256];
    auto recv_res = socket_receive(client, buffer, sizeof(buffer) - 1);
    REQUIRE(recv_res.bytes > 0);
    buffer[recv_res.bytes] = '\0';
    
    REQUIRE(String8{buffer, (U64)recv_res.bytes} == "PONG");
    
    server_thread.join();
    REQUIRE(received_data == "PING");
}

TEST_CASE("TCP pool creation and destruction", "[objstore.tcp]") {
    int port = get_unique_port();
    int pipe_fds[2];
    
#if PLEXDB_OS_LINUX
    REQUIRE(pipe(pipe_fds) == 0);
#endif

    {
        Pool pool{port, pipe_fds[0]};
        REQUIRE(pool.listen_fd >= 0);
        REQUIRE(pool.epoll_fd >= 0);
    }

#if PLEXDB_OS_LINUX
    close(pipe_fds[0]);
    close(pipe_fds[1]);
#endif
}

TEST_CASE("Socket recv timeout", "[objstore.tcp]") {
    int port = get_unique_port();
    
    std::atomic<bool> server_ready{false};
    std::thread server_thread([port, &server_ready]() {
        Socket listen_sock{socket_create_tcp()};
        socket_set_option(listen_sock, SocketOption::ReuseAddress, true);
        socket_bind(listen_sock, (U16)port);
        socket_listen(listen_sock, 1);
        socket_set_timeout(listen_sock, 3000);
        
        server_ready = true;
        
        Handle client_fd = socket_accept(listen_sock);
        if (socket_valid(client_fd)) {
            Socket client{client_fd};
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    });
    
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    Socket client{socket_create_tcp()};
    socket_set_timeout(client, 500);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
    
    char buffer[256];
    auto start = std::chrono::steady_clock::now();
    auto recv_res = socket_receive(client, buffer, sizeof(buffer));
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    REQUIRE(recv_res.bytes <= 0);
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);
    
    server_thread.join();
}

TEST_CASE("Socket multiple rapid connections", "[objstore.tcp]") {
    int port = get_unique_port();
    
    std::atomic<bool> server_running{true};
    std::atomic<int> connections_handled{0};
    
    std::thread server_thread([port, &server_running, &connections_handled]() {
        Socket listen_sock{socket_create_tcp()};
        socket_set_option(listen_sock, SocketOption::ReuseAddress, true);
        socket_bind(listen_sock, (U16)port);
        socket_listen(listen_sock, 32);
        socket_set_option(listen_sock, SocketOption::Nonblocking, true);
        
        while (server_running) {
            Handle client_fd = socket_accept(listen_sock);
            if (socket_valid(client_fd)) {
                connections_handled++;
                socket_close(client_fd);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    constexpr int NUM_CONNECTIONS = 10;
    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 500);
        socket_connect(client, "127.0.0.1", (U16)port);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    server_running = false;
    server_thread.join();
    
    REQUIRE(connections_handled >= NUM_CONNECTIONS / 2);
}

TEST_CASE("Socket send_all sends complete data", "[objstore.tcp]") {
    int port = get_unique_port();
    
    std::atomic<bool> server_ready{false};
    U64 total_received = 0;
    
    std::thread server_thread([port, &server_ready, &total_received]() {
        Socket listen_sock{socket_create_tcp()};
        socket_set_option(listen_sock, SocketOption::ReuseAddress, true);
        socket_bind(listen_sock, (U16)port);
        socket_listen(listen_sock, 1);
        socket_set_timeout(listen_sock, 2000);
        
        server_ready = true;
        
        Handle client_fd = socket_accept(listen_sock);
        if (socket_valid(client_fd)) {
            Socket client{client_fd};
            socket_set_timeout(client, 1000);
            
            char buffer[8192];
            while (true) {
                auto res = socket_receive(client, buffer, sizeof(buffer));
                if (res.bytes > 0) {
                    total_received += res.bytes;
                } else {
                    break;
                }
            }
        }
    });
    
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    Socket client{socket_create_tcp()};
    socket_set_timeout(client, 2000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
    
    constexpr U64 DATA_SIZE = 4096;
    char data[DATA_SIZE];
    for (U64 i = 0; i < DATA_SIZE; i++) {
        data[i] = (char)('A' + (i % 26));
    }
    
    auto send_res = socket_send_all(client, data, DATA_SIZE);
    REQUIRE(send_res.error == SocketError::None);
    REQUIRE(send_res.bytes == (S64)DATA_SIZE);
    
    socket_close(client.handle);
    client.handle = zero_handle();
    
    server_thread.join();
    REQUIRE(total_received == DATA_SIZE);
}

TEST_CASE("Socket handles empty connection (connect and close)", "[objstore.tcp]") {
    int port = get_unique_port();
    
    std::atomic<bool> server_ready{false};
    std::atomic<bool> client_closed{false};
    
    std::thread server_thread([port, &server_ready, &client_closed]() {
        Socket listen_sock{socket_create_tcp()};
        socket_set_option(listen_sock, SocketOption::ReuseAddress, true);
        socket_bind(listen_sock, (U16)port);
        socket_listen(listen_sock, 1);
        socket_set_timeout(listen_sock, 2000);
        
        server_ready = true;
        
        Handle client_fd = socket_accept(listen_sock);
        if (socket_valid(client_fd)) {
            Socket client{client_fd};
            socket_set_timeout(client, 1000);
            
            char buffer[256];
            auto res = socket_receive(client, buffer, sizeof(buffer));
            if (res.error == SocketError::ConnectionClosed || res.bytes == 0) {
                client_closed = true;
            }
        }
    });
    
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    {
        Socket client{socket_create_tcp()};
        socket_set_timeout(client, 500);
        REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
    }
    
    server_thread.join();
    REQUIRE(client_closed);
}

TEST_CASE("Socket handles partial data", "[objstore.tcp]") {
    int port = get_unique_port();
    
    std::atomic<bool> server_ready{false};
    U64 total_received = 0;
    
    std::thread server_thread([port, &server_ready, &total_received]() {
        Socket listen_sock{socket_create_tcp()};
        socket_set_option(listen_sock, SocketOption::ReuseAddress, true);
        socket_bind(listen_sock, (U16)port);
        socket_listen(listen_sock, 1);
        socket_set_timeout(listen_sock, 2000);
        
        server_ready = true;
        
        Handle client_fd = socket_accept(listen_sock);
        if (socket_valid(client_fd)) {
            Socket client{client_fd};
            socket_set_timeout(client, 1000);
            
            char buffer[1024];
            while (true) {
                auto res = socket_receive(client, buffer, sizeof(buffer));
                if (res.bytes > 0) {
                    total_received += res.bytes;
                } else {
                    break;
                }
            }
        }
    });
    
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    Socket client{socket_create_tcp()};
    socket_set_timeout(client, 1000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
    
    socket_send(client, "Hello", 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    socket_send(client, "World", 5);
    
    socket_close(client.handle);
    client.handle = zero_handle();
    
    server_thread.join();
    REQUIRE(total_received == 10);
}

TEST_CASE("Socket handles binary data with null bytes", "[objstore.tcp]") {
    int port = get_unique_port();
    
    std::atomic<bool> server_ready{false};
    char received_data[256] = {0};
    U64 received_len = 0;
    
    std::thread server_thread([port, &server_ready, &received_data, &received_len]() {
        Socket listen_sock{socket_create_tcp()};
        socket_set_option(listen_sock, SocketOption::ReuseAddress, true);
        socket_bind(listen_sock, (U16)port);
        socket_listen(listen_sock, 1);
        socket_set_timeout(listen_sock, 2000);
        
        server_ready = true;
        
        Handle client_fd = socket_accept(listen_sock);
        if (socket_valid(client_fd)) {
            Socket client{client_fd};
            socket_set_timeout(client, 1000);
            
            auto res = socket_receive(client, received_data, sizeof(received_data));
            if (res.bytes > 0) {
                received_len = res.bytes;
            }
        }
    });
    
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    Socket client{socket_create_tcp()};
    socket_set_timeout(client, 1000);
    REQUIRE(socket_connect(client, "127.0.0.1", (U16)port));
    
    char binary_data[10] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00};
    socket_send_all(client, binary_data, 10);
    
    socket_close(client.handle);
    client.handle = zero_handle();
    
    server_thread.join();
    
    REQUIRE(received_len == 10);
    REQUIRE(received_data[0] == 0x01);
    REQUIRE(received_data[1] == 0x00);
    REQUIRE(received_data[2] == 0x02);
}
