module;
#include "macros.h"

export module objstore.tcp.types;

import plexdb.os;
import plexdb.base;

using namespace plexdb;

export namespace objstore::tcp {
    // ========================================================================
    // buffer
    //   a connection can hold multiple buffers, there is a many-to-one mapping
    //   between buffers -> client
    // ========================================================================
    // @note fat struct, not always meaningful depending on the handler
    struct BufferInfo {
        PLEXDB_DEBUG_X(U32 buffer_idx = -1;)
        os::Handle client = os::zero_handle();
    };

    // ========================================================================
    // connection
    // ========================================================================
    constexpr int MAX_CONCURRENT_CONNECTIONS = 1000;

    struct Connection {
        os::Handle client;
    };

    // ========================================================================
    // stats
    // ========================================================================
    struct Stats {
        U64 dropped_connections = 0;
        U64 total_connections   = 0;
        U64 active_connections  = 0;
        U64 total_bytes_read    = 0;
        U64 total_bytes_written = 0;
    };
}
