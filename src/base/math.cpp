module;
#include "macros.h"

#if PLEXDB_COMPILER_MSVC
    #include <intrin.h>
#endif

module plexdb.base.math;

namespace plexdb {
    #if PLEXDB_COMPILER_GCC || PLEXDB_COMPILER_CLANG
        U64 bit_count(U64 x) {
            return (U64)__builtin_popcountll(x);
        }
        U64 bit_count_trailing_zeros(U64 x) {
            assert_true(x != 0, "zero ctz");
            return (U64)__builtin_ctzll(x);
        }
        U64 bit_count_leading_zeros(U64 x) {
            assert_true(x != 0, "zero clz");
            return (U64)__builtin_clzll(x);
        }
    #elif PLEXDB_COMPILER_MSVC
        U64 bit_count(U64 x) {
            #if ARCH_X86
                return __popcnt((u32)(x)) + __popcnt((u32)(x >> 32));
            #else
                return __popcnt64(x);
            #endif
        }
        U64 bit_count_trailing_zeros(U64 x) {
            assert_true(x != 0, "zero ctz");
            unsigned long index;
            _BitScanForward64(&index, x);
            return (U64)index;
        }
        U64 bit_count_leading_zeros(U64 x) {
            assert_true(x != 0, "zero clz");
            unsigned long index;
            _BitScanReverse64(&index, x);
            return (U64)(63 - index);
        }
    #else
        #error Compiler instrinsics not implemented.
    #endif
}
