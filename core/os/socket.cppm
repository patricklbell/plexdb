export module plexdb.os.socket;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb::os {
    enum class SocketError {
        None,
        WouldBlock,
        ConnectionRefused,
        ConnectionReset,
        ConnectionClosed,
        Timeout,
        Other,
    };

    enum class SocketOption {
        NonBlocking = 0,
        NoDelay,
        Reuse,
    };

    struct SocketResult {
        U32 byte_count;
        SocketError error;
    };

    Handle socket_open();
    void   socket_close(Handle socket);
    bool   socket_set_option(Handle socket, SocketOption option, bool enabled);
    bool   socket_set_timeout(Handle socket, int timeout_ms);
    bool   socket_connect(Handle socket, const char* host, U16 port);
    bool   socket_bind(Handle socket, U16 port);
    bool   socket_listen(Handle socket, int backlog);
    Handle socket_accept(Handle socket);

    SocketResult socket_send(Handle socket, const void* data, U32 length);
    SocketResult socket_receive(Handle socket, void* data, U32 length);
    SocketResult socket_send_all(Handle socket, const void* data, U32 length);
    SocketResult socket_receive_all(Handle socket, void* data, U32 length);

    struct Socket {
        Handle handle = zero_handle();

        Socket() = default;
        explicit Socket(Handle h) : handle(h) {}
        ~Socket();

        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;

        Socket(Socket&& other) noexcept;
        Socket& operator=(Socket&& other) noexcept;

        operator Handle() const { return handle; }
        operator bool() const;
    };
}
