export module objstore.tcp;

import plexdb.os;
import plexdb.os.uring;
import plexdb.base;
import objstore.tcp.detail;

// @todo limit export
export import objstore.tcp.types;
export import objstore.tcp.coro;

using namespace plexdb;

export namespace objstore::tcp {
    // ========================================================================
    // printing
    // ========================================================================
    AutoString8 to_str(const Stats& s);
}
