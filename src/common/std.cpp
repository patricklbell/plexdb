module plexdb.common;

#include <cstring>

namespace plexdb {
    void* memcpy(void* dest, void* src, size_t n) {
        return std::memcpy(dest, src, n);
    }

    void* memmove(void* dest, void* src, size_t n) {
        return std::memmove(dest, src, n);
    }

    void* memset(void* str, int c, size_t n) {
        return std::memset(str, c, n);
    }
}