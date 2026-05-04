export module objstore.tcp;

import plexdb.os;
import plexdb.os.uring;
import plexdb.base;
import plexdb.arena;

import objstore.log;
import objstore.tcp.detail;

// @todo limit export
export import objstore.tcp.types;

using namespace plexdb;

export namespace objstore::tcp {
    // ========================================================================
    // listen
    // ========================================================================
    struct Listener {
        Stats stats;
        uring::Ring ring;
        os::Handle socket;

        Listener(os::Handle socket, bool try_uring = true) : socket(socket) {
            if (try_uring) {
                auto ring_settings = uring::get_ring_settings();
                if (ring_settings->recommended) {
                    this->ring = uring::Ring{
                        socket,
                        ring_settings->recommended_queue_depth, ring_settings->recommended_buffer_size, ring_settings->recommended_buffer_count
                    };
                }
            }
        }
    };
    
    void listen(
        Listener& listener,
        const ConnectionHandler auto& connection_handler,
        const os::Notifier& interrupt, volatile bool& should_exit
    ) {
        if (listener.ring) {
            return listen_uring(listener.ring, listener.stats, connection_handler, interrupt, should_exit);
        }

        log::tcp_warn("IO Uring disabled or not available, falling back to async sockets. This may reduce performance.");
        return listen_socket(listener.socket, listener.stats, connection_handler, interrupt, should_exit);
    }

    // ========================================================================
    // printing
    // ========================================================================
    AutoString8 to_str(const Stats& s);
}