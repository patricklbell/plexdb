module;
#include <plexdb/macros/macros.h>

// @todo used only for std::numeric_limits
#include <limits>

export module plexdb.base.math;

import plexdb.base.types;

namespace plexdb {
    template<typename T>
    struct UnsignedHelper;

    template<> struct UnsignedHelper<S8>  { using type = U8;  };
    template<> struct UnsignedHelper<S16> { using type = U16; };
    template<> struct UnsignedHelper<S32> { using type = U32; };
    template<> struct UnsignedHelper<S64> { using type = U64; };

    template<> struct UnsignedHelper<U8>  { using type = U8;  };
    template<> struct UnsignedHelper<U16> { using type = U16; };
    template<> struct UnsignedHelper<U32> { using type = U32; };
    template<> struct UnsignedHelper<U64> { using type = U64; };
}

export namespace plexdb {
    template<typename T>
    concept IsUnsigned = requires { T(0); T(-1); } && (T(-1) > T(0));

    template<typename T>
    concept IsSigned = !IsUnsigned<T>;

    template<typename T>
    concept Integer = requires(T a, T b) {
        T(0);
        T(1);
        a + b;
        a - b;
        a * b;
        a / b;
        a % b;
        a & b;
    };

    // ========================================================================
    // ranges
    // ========================================================================
    struct Rng1U64 {
        U64 start;
        U64 end;
    };
    static_assert(sizeof(Rng1U64) == sizeof(U64)*2);

    // ========================================================================
    // helpers
    // ========================================================================
    template<typename T, typename... Ts>
    constexpr T max(const T& a, const Ts&... rest) {
        static_assert(sizeof...(rest) > 0);
        T result = a;
        ((result = (result >= static_cast<T>(rest)) ? result : static_cast<T>(rest)), ...);
        return result;
    }

    template<typename T, typename... Ts>
    constexpr T min(const T& a, const Ts&... rest) {
        static_assert(sizeof...(rest) > 0);
        T result = a;
        ((result = (result <= static_cast<T>(rest)) ? result : static_cast<T>(rest)), ...);
        return result;
    }

    template<typename T>
        requires IsUnsigned<T> && Integer<T>
    inline constexpr T ceil_div(const T& a, const T& b) { return (a  + b - 1) / b; }

    template<typename T>
        requires IsSigned<T> && Integer<T>
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
        if consteval {
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

    template<typename T>
    using Unsigned = typename UnsignedHelper<T>::type;

    template<typename To, typename From>
        requires Integer<To> && Integer<From>
    inline constexpr To bounds_checked_cast(const From& value) {
        constexpr auto to_min = std::numeric_limits<To>::min();
        constexpr auto to_max = std::numeric_limits<To>::max();

        if constexpr (IsSigned<From> && IsSigned<To>) {
            assert_true(value >= static_cast<From>(to_min), "overflow in numeric conversion");
            assert_true(value <= static_cast<From>(to_max), "underoverflow in numeric conversion");
        } else if constexpr (IsSigned<From> && IsUnsigned<To>) {
            assert_true(value >= 0, "underflow in numeric conversion");
            assert_true(static_cast<Unsigned<From>>(value) <= static_cast<Unsigned<From>>(to_max), "overflow in numeric conversion");
        } else if constexpr (IsUnsigned<From> && IsSigned<To>) {
            assert_true(value <= static_cast<From>(to_max), "overflow in numeric conversion");
        } else {
            assert_true(value <= static_cast<From>(to_max), "overflow in numeric conversion");
        }

        return static_cast<To>(value);
    }
}
