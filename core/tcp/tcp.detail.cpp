module plexdb.tcp.detail;

import plexdb.os;
import plexdb.base;
import plexdb.arena;

namespace plexdb {
    AutoString8 to_str(const tcp::Stats& s) {
        AutoString8 res = "=== TCP Stats ===\n"_as;
        res += "Total connections: "_as  + to_str(s.total_connections)   + "\n";
        res += "Active connections: "_as + to_str(s.active_connections)  + "\n";
        res += "Dropped connections: "_as+ to_str(s.dropped_connections) + "\n";
        res += "Total bytes read: "_as   + to_str(s.total_bytes_read)    + " ("_as + to_str((F64)s.total_bytes_read / 1_mb) + "MB)\n";
        res += "Total bytes written: "_as+ to_str(s.total_bytes_written) + " ("_as + to_str((F64)s.total_bytes_written / 1_mb) + "MB)\n";
        res += "================="_as;
        return res;
    }
}
