module plexdb.tcp.detail;

import plexdb.os;
import plexdb.base;
import plexdb.arena;

namespace plexdb::tcp {
    AutoString8 to_str(const Stats& s) {
        AutoString8 res = "=== TCP Stats ===\n"_as;
        res += "Total connections: "_as  + plexdb::to_str(s.total_connections)   + "\n";
        res += "Active connections: "_as + plexdb::to_str(s.active_connections)  + "\n";
        res += "Dropped connections: "_as+ plexdb::to_str(s.dropped_connections) + "\n";
        res += "Total bytes read: "_as   + plexdb::to_str(s.total_bytes_read)    + " ("_as + plexdb::to_str((F64)s.total_bytes_read / 1_mb) + "MB)\n";
        res += "Total bytes written: "_as+ plexdb::to_str(s.total_bytes_written) + " ("_as + plexdb::to_str((F64)s.total_bytes_written / 1_mb) + "MB)\n";
        res += "================="_as;
        return res;
    }
}
