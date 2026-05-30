module;
#include <plexdb/macros/macros.h>
#if PLEXDB_OS_LINUX
    #include <time.h>
#endif

module plexdb.os.time;

namespace plexdb::os {
#if PLEXDB_OS_LINUX
    S64 monotonic_us() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return S64(ts.tv_sec) * 1000000 + S64(ts.tv_nsec) / 1000;
    }

    U64 unix_ms_now() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return U64(ts.tv_sec) * 1000 + U64(ts.tv_nsec) / 1000000;
    }

    S64 unix_days_now() {
        return static_cast<S64>(unix_ms_now() / 86400000_u64);
    }

    S64 unix_ns_since_midnight_now() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        S64 day_seconds = static_cast<S64>(ts.tv_sec % 86400);
        return day_seconds * 1000000000LL + static_cast<S64>(ts.tv_nsec);
    }
#endif
}
