export module plexdb.tcp;

import plexdb.os;
import plexdb.os.uring;
import plexdb.aio;
import plexdb.base;
import plexdb.arena;
import plexdb.plugin;
import plexdb.tagged_union;

import plexdb.tcp.detail;

export import plexdb.tcp.types;

using namespace plexdb;

namespace plexdb::tcp {
    plugin::Producer producer{"plexdb::tcp"};
}

export namespace plexdb::tcp {
    // @note non-movable: EventConsumer holds raw pointers into state
    struct TcpServer {
        TaggedUnion<UringListenerState, SocketListenerState> state;
        aio::EventConsumer                                   consumer;
        Stats*                                               stats;

        TcpServer(const TcpServer&)            = delete;
        TcpServer& operator=(const TcpServer&) = delete;
        TcpServer(TcpServer&&)                 = delete;
        TcpServer& operator=(TcpServer&&)      = delete;

        TcpServer(UringListenerState s, const ConnectionHandler auto* in_handler, os::Poll& poll)
            : state(move(s)) {
            consumer = listen_uring_start(&get<UringListenerState>(state), in_handler, poll);
            stats    = &get<UringListenerState>(state).stats;
        }

        TcpServer(SocketListenerState s, const ConnectionHandler auto* in_handler, os::Poll& poll)
            : state(move(s)) {
            consumer = listen_socket_start(&get<SocketListenerState>(state), in_handler, poll);
            stats    = &get<SocketListenerState>(state).stats;
        }
    };

    // @note relies on copy elision
    TcpServer create_tcp_server(
        os::Handle                    socket,
        const ConnectionHandler auto* in_handler,
        os::Poll&                     poll,
        bool                          try_uring = true
    ) {
        if (try_uring) {
            auto ring_settings = uring::get_ring_settings();
            if (ring_settings->recommended) {
                uring::Ring ring{
                    socket,
                    ring_settings->recommended_queue_depth,
                    ring_settings->recommended_buffer_size,
                    ring_settings->recommended_buffer_count
                };
                if (ring && !os::is_zero_handle(ring.event_fd)) {
                    return TcpServer{UringListenerState{move(ring)}, in_handler, poll};
                }
            }
        }

        plugin::message(producer, plugin::Level::Warn, "io_uring not available, falling back to async sockets");
        return TcpServer{
            SocketListenerState{socket, &poll},
            in_handler, poll
        };
    }
}

export namespace plexdb {
    AutoString8 to_str(const tcp::Stats& s);
}
