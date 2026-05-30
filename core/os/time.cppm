export module plexdb.os.time;

import plexdb.base;

export namespace plexdb::os {
    S64 monotonic_us();
    U64 unix_ms_now();
    S64 unix_days_now();
    S64 unix_ns_since_midnight_now();
}
