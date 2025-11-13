module;
#include "macros.h"
#if PLEXDB_OS_LINUX
    #include <stdlib.h>
#endif

module plexdb.os;

#if PLEXDB_OS_LINUX
    #include "linux/os_linux.cpp"
#else
    #error "OS library not implemented."
#endif