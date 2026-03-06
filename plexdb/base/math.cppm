module;
#include "macros.h"

export module plexdb.base.math;

import plexdb.base.types;

export namespace plexdb {
    // ========================================================================
    // ranges
    // ========================================================================
    union Rng1U64 {
        struct {
            U64 start;
            U64 end;
        };
        U64 u64[2];
    };
    static_assert(sizeof(Rng1U64) == sizeof(U64)*2);

    // ========================================================================
    // helpers
    // ========================================================================
    template<typename T, typename... Ts>
    constexpr T max(const T& a, const Ts&... rest) {
        static_assert(sizeof...(rest) > 0);
        T result = a;
        ((result = (result >= rest) ? result : rest), ...);
        return result;
    }

    template<typename T, typename... Ts>
    constexpr T min(const T& a, const Ts&... rest) {
        static_assert(sizeof...(rest) > 0);
        T result = a;
        ((result = (result <= rest) ? result : rest), ...);
        return result;
    }
    
    template<typename T>
        requires Unsigned<T> && Integer<T>
    inline constexpr T ceil_div(const T& a, const T& b) { return (a  + b - 1) / b; }

    template<typename T>
        requires Signed<T> && Integer<T>
    inline constexpr T ceil_div(const T& a, const T& b) { return 1 + (a - 1) / b; }

    template<typename T>
    inline constexpr T align_pow2(T x, T b) { return (x + b - 1)&(~(b - 1)); }

    template<typename T>
        requires Integer<T>
    inline constexpr T align_down(const T& x, const T& align) { return (x / align) * align; }

    template<typename T>
        requires Integer<T>
    inline constexpr T align_up(const T& x, const T& align) { return ceil_div(x, align)*align; }

    inline constexpr U64 hash(U64 x) { return 1 + x; }

#if PLEXDB_COMPILER_GCC || PLEXDB_COMPILER_CLANG
    inline constexpr U64 bit_count(U64 x) {
        return (U64)__builtin_popcountll(x);
    }
    inline constexpr U64 bit_count_trailing_zeros(U64 x) {
        assert_true(x != 0, "zero ctz");
        return (U64)__builtin_ctzll(x);
    }
    inline constexpr U64 bit_count_leading_zeros(U64 x) {
        assert_true(x != 0, "zero clz");
        return (U64)__builtin_clzll(x);
    }
    inline constexpr F64 round_to_infinity(F64 x) {
        if (PLEXDB_IS_CONSTEVAL()) {
            // @note NaN and infinities are not allowed in constexpr
            S64 i = (S64)x;
            if ((F64)i == x) {
                return x;
            }
            return x > 0 ? (F64)(i + 1) : (F64)i;
        } else {
            return __builtin_ceil(x);
        }
    }
#else
    #error Compiler instrinsics not implemented.
#endif

    inline constexpr bool has_single_bit(U64 x) {
        return bit_count(x) == 1;
    }
}