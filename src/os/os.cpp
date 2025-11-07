module plexdb.os;

#include "common/macros.h"

#if OS_LINUX
    #include "linux/os_linux.cpp"
#else
    #error "OS library not implemented."
#endif