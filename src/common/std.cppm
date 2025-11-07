module;

#include <cstring>

export module plexdb.common:std;

namespace plexdb {
    export template<typename T> constexpr T max(const T& a, const T& b) { return (a >= b) ? a : b; }
    export template<typename T> constexpr T min(const T& a, const T& b) { return (a <= b) ? a : b; }

    export template<typename T> constexpr T align_pow2(T x, T b) { return (x + b - 1)&(~(b - 1)); }

    export void* memcpy(void* dest, void* src, size_t n);
    export void* memmove(void* dest, void* src, size_t n);
    export void* memset(void* str, int c, size_t n);
}