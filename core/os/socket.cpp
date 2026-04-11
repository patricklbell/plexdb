module;
#include "macros.h"

#if PLEXDB_OS_LINUX
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#elif PLEXDB_OS_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
#endif

module plexdb.os.socket;

import plexdb.base;
import plexdb.os.core;

namespace plexdb::os {
    #if PLEXDB_OS_LINUX
        static int handle_to_fd(Handle h) { return static_cast<int>(h.u32[0]); }
        static Handle fd_to_handle(int fd) { return Handle{.u32={static_cast<U32>(fd)}}; }

        Handle socket_open() {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            
            if (fd < 0) {
                return zero_handle();
            }
            return fd_to_handle(fd);
        }

        void socket_close(Handle socket) {
            if (is_zero_handle(socket)) {
                return;
            }
            ::close(handle_to_fd(socket));
        }

        bool socket_set_option(Handle socket, SocketOption option, bool enable) {
            int fd = handle_to_fd(socket);
            int opt = enable ? 1 : 0;
            
            switch (option) {
                case SocketOption::NonBlocking: {
                    int flags = fcntl(fd, F_GETFL, 0);

                    if (flags == -1) {
                        return false;
                    }

                    if (enable) {
                        flags |= O_NONBLOCK;
                    } else {
                        flags &= ~O_NONBLOCK;
                    }
                    return fcntl(fd, F_SETFL, flags) != -1;
                }
                case SocketOption::NoDelay: {
                    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
                }
                case SocketOption::Reuse: {
                    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
                    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0;
                }
            }
            return false;
        }

        bool socket_set_timeout(Handle socket, int timeout_ms) {
            int fd = handle_to_fd(socket);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            bool ok = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
            ok = ok && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
            return ok;
        }

        bool socket_connect(Handle socket, const char* host, U16 port) {
            int fd = handle_to_fd(socket);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) return false;
            return ::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        }

        bool socket_bind(Handle socket, U16 port) {
            int fd = handle_to_fd(socket);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            return ::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        }

        bool socket_listen(Handle socket, int backlog) {
            return ::listen(handle_to_fd(socket), backlog) == 0;
        }

        Handle socket_accept(Handle socket) {
            int client_fd = ::accept(handle_to_fd(socket), nullptr, nullptr);
            if (client_fd < 0) {
                return zero_handle();
            }
            return fd_to_handle(client_fd);
        }

        SocketResult socket_send(Handle socket, const void* data, S32 length) {
            ssize_t result = ::send(handle_to_fd(socket), data, length, 0);
            
            assert_true(result <= MAX_S32, "send length out of range, this should never happen!");
            if (result >= 0)                            return {static_cast<S32>(result), SocketError::None};
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK)    return {0, SocketError::WouldBlock};
            if (err == ECONNRESET)                      return {0, SocketError::ConnectionReset};
            if (err == EPIPE)                           return {0, SocketError::ConnectionClosed};
            return {0, SocketError::Other};
        }

        SocketResult socket_receive(Handle socket, void* data, S32 length) {
            ssize_t result = ::recv(handle_to_fd(socket), data, length, 0);
            
            assert_true(result <= MAX_S32, "send length out of range, this should never happen!");
            if (result > 0)                             return {static_cast<S32>(result), SocketError::None};
            if (result == 0)                            return {0, SocketError::ConnectionClosed};
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK)    return {0, SocketError::WouldBlock};
            if (err == ECONNRESET)                      return {0, SocketError::ConnectionReset};
            if (err == ETIMEDOUT)                       return {0, SocketError::Timeout};
            return {0, SocketError::Other};
        }

        SocketResult socket_send_all(Handle socket, const void* data, S32 length) {
            const U8* ptr = (const U8*)data;
            S32 total = 0;

            while (total < length) {
                SocketResult res = socket_send(socket, ptr, length - total);
                if (res.error != SocketError::None && res.error != SocketError::WouldBlock) {
                    return {total, res.error};
                }
                if (res.byte_count > 0) {
                    ptr += res.byte_count;
                    total += res.byte_count;
                }
            }
            return {total, SocketError::None};
        }

        SocketResult socket_receive_all(Handle socket, void* data, S32 length) {
            U8* ptr = (U8*)data;
            S32 total = 0;

            while (total < length) {
                SocketResult res = socket_receive(socket, ptr, length - total);
                if (res.error == SocketError::ConnectionClosed) {
                    return {total, SocketError::ConnectionClosed};
                }
                if (res.error != SocketError::None && res.error != SocketError::WouldBlock) {
                    return {total, res.error};
                }
                if (res.byte_count > 0) {
                    ptr += res.byte_count;
                    total += res.byte_count;
                }
            }
            return {total, SocketError::None};
        }
    #elif PLEXDB_OS_WINDOWS
        static SOCKET handle_to_sock(Handle h) { return static_cast<SOCKET>(h.u64[0]); }
        static Handle sock_to_handle(SOCKET s) { return Handle{.u64={static_cast<U64>(s)}}; }

        Handle socket_create_tcp() {
            static bool initialized = false;
            if (!initialized) {
                WSADATA wsa_data;
                if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return zero_handle();
                initialized = true;
            }
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) return zero_handle();
            return sock_to_handle(s);
        }

        void socket_close(Handle socket) {
            if (!socket_valid(socket)) return;
            ::closesocket(handle_to_sock(socket));
        }

        bool socket_set_option(Handle socket, SocketOption option, bool enabled) {
            SOCKET s = handle_to_sock(socket);
            int opt = enabled ? 1 : 0;
            
            switch (option) {
                case SocketOption::NonBlocking: {
                    u_long mode = enabled ? 1 : 0;
                    return ioctlsocket(s, FIONBIO, &mode) == 0;
                }
                case SocketOption::NoDelay: {
                    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt)) == 0;
                }
                case SocketOption::Reuse: {
                    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) == 0;
                }
            }
            return false;
        }

        bool socket_set_timeout(Handle socket, int timeout_ms) {
            SOCKET s = handle_to_sock(socket);
            DWORD timeout = timeout_ms;
            bool ok = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) == 0;
            ok = ok && setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout)) == 0;
            return ok;
        }

        bool socket_connect(Handle socket, const char* host, U16 port) {
            SOCKET s = handle_to_sock(socket);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (InetPtonA(AF_INET, host, &addr.sin_addr) <= 0) return false;
            return ::connect(s, (sockaddr*)&addr, sizeof(addr)) == 0;
        }

        bool socket_bind(Handle socket, U16 port) {
            SOCKET s = handle_to_sock(socket);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            return ::bind(s, (sockaddr*)&addr, sizeof(addr)) == 0;
        }

        bool socket_listen(Handle socket, int backlog) {
            return ::listen(handle_to_sock(socket), backlog) == 0;
        }

        Handle socket_accept(Handle socket) {
            SOCKET client = ::accept(handle_to_sock(socket), nullptr, nullptr);
            if (client == INVALID_SOCKET) return zero_handle();
            return sock_to_handle(client);
        }

        SocketResult socket_send(Handle socket, const void* data, S32 length) {
            int result = ::send(handle_to_sock(socket), (const char*)data, length, 0);
            if (result >= 0) return {result, SocketError::None};
            
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return {0, SocketError::WouldBlock};
            if (err == WSAECONNRESET) return {0, SocketError::ConnectionReset};
            return {0, SocketError::Other};
        }

        SocketResult socket_receive(Handle socket, void* data, S32 length) {
            int result = ::recv(handle_to_sock(socket), (char*)data, length, 0);
            if (result > 0) return {result, SocketError::None};
            if (result == 0) return {0, SocketError::ConnectionClosed};
            
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return {0, SocketError::WouldBlock};
            if (err == WSAECONNRESET) return {0, SocketError::ConnectionReset};
            if (err == WSAETIMEDOUT) return {0, SocketError::Timeout};
            return {0, SocketError::Other};
        }

        SocketResult socket_send_all(Handle socket, const void* data, S32 length) {
            const U8* ptr = (const U8*)data;
            S32 total = 0;

            while (total < length) {
                SocketResult res = socket_send(socket, ptr, length - total);
                if (res.error != SocketError::None && res.error != SocketError::WouldBlock) {
                    return {total, res.error};
                }
                if (res.bytes > 0) {
                    ptr += res.bytes;
                    remaining -= res.bytes;
                    total += res.bytes;
                }
            }
            return {total, SocketError::None};
        }

        SocketResult socket_receive_all(Handle socket, void* data, S32 length) {
            U8* ptr = (U8*)data;
            U64 remaining = length;
            S32 total = 0;

            while (total < length) {
                SocketResult res = socket_receive(socket, ptr, length - total);
                if (res.error == SocketError::ConnectionClosed) {
                    return {total, SocketError::ConnectionClosed};
                }
                if (res.error != SocketError::None && res.error != SocketError::WouldBlock) {
                    return {(S64)total, res.error};
                }
                if (res.bytes > 0) {
                    ptr += res.bytes;
                    remaining -= res.bytes;
                    total += res.bytes;
                }
            }
            return {(S64)total, SocketError::None};
        }
    #else
        #error "Socket library not implemented for OS"
    #endif

    Socket::~Socket() {
        socket_close(handle);
    }

    Socket::Socket(Socket&& other) noexcept : handle(other.handle) {
        other.handle = zero_handle();
    }

    Socket& Socket::operator=(Socket&& other) noexcept {
        if (this != &other) {
            socket_close(handle);
            handle = other.handle;
            other.handle = zero_handle();
        }
        return *this;
    }

    Socket::operator bool() const {
        return !is_zero_handle(this->handle);
    }
}
