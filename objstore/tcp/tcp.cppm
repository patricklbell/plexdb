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
    Stats listen(
        os::Handle socket,
        const OnChunk auto& on_chunk_callback, const OnOpen auto& on_open_callback, const OnClose auto& on_close_callback,
        const os::Notifier& signal_pipe, volatile bool& should_exit,
        bool use_uring = true
    ) {
        if (use_uring) {
            auto ring_settings = uring::get_ring_settings();

            if (ring_settings->recommended) {
                uring::Ring ring{
                    socket,
                    ring_settings->recommended_queue_depth, ring_settings->recommended_buffer_size, ring_settings->recommended_buffer_count
                };

                if (ring) {
                    return listen_uring(ring, on_chunk_callback, on_close_callback, on_open_callback, signal_pipe, should_exit);
                }
            }
        }

        log::tcp_warn("IO Uring disabled or not available, falling back to async sockets. This may reduce performance.");
        return listen_socket(socket, on_chunk_callback, on_close_callback, on_open_callback, signal_pipe, should_exit);
    }

    // ========================================================================
    // printing
    // ========================================================================
    AutoString8 to_str(const Stats& s);
}