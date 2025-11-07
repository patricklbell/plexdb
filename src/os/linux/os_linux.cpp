#include <stdlib.h>

namespace plexdb::os {
    U8* allocate(U64 size) {
        return reinterpret_cast<U8*>(malloc(size));
    }
    
    void deallocate(void* ptr) {
        free(ptr);
    }
}