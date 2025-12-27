module;
#include "macros.h"
#if PLEXDB_OS_LINUX
    #include <stdlib.h>
    #include <fcntl.h>
    #include <unistd.h>
    #if PLEXDB_DEBUG
        #include <errno.h>
    #endif
#endif

#if PLEXDB_OS_LINUX
    #include "linux/os_linux.cpp"
#else
    #error "OS library not implemented."
#endif