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
#endif
}
