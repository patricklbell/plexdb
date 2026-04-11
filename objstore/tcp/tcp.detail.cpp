module objstore.tcp.detail;

import plexdb.os;
import plexdb.arena;

namespace objstore::tcp {
    // ========================================================================
    // printing
    // ========================================================================
    AutoString8 to_str(const Stats& s) {
        AutoString8 res = "=== Stats ===\n"_as;
        res += "Total connections: " + plexdb::to_str(s.total_connections) + "\n";
        res += "Active connections: " + plexdb::to_str(s.active_connections) + "\n";
        res += "Total bytes read: " + plexdb::to_str(s.total_bytes_read) + "(" + plexdb::to_str((F64)s.total_bytes_read / 1_mb) + "MB)\n";
        res += "Total bytes written: " + plexdb::to_str(s.total_bytes_written) + "(" + plexdb::to_str((F64)s.total_bytes_written / 1_mb) + "MB)\n";
        res += "=============";
        return res;
    }
}