export module plexdb.os;

import plexdb.common;

namespace plexdb::os {
    export U8* allocate(U64 size);
    export void deallocate(void* ptr);
}