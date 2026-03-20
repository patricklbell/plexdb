module;
#include "macros.h"
#include <cstdio>
#include <cstring>

module objstore.tcp.detail;

import plexdb.base;
import objstore.tcp.types;

using namespace plexdb;

namespace objstore::tcp {
    AutoString8 to_str(const Stats& s) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Stats{dropped:%llu total:%llu active:%llu read:%llu written:%llu}",
            (unsigned long long)s.dropped_connections,
            (unsigned long long)s.total_connections,
            (unsigned long long)s.active_connections,
            (unsigned long long)s.total_bytes_read,
            (unsigned long long)s.total_bytes_written);
        AutoString8 str;
        append(str, String8{buf, (U64)strlen(buf)});
        return str;
    }
}
