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
    template<typename T>
    inline constexpr T max(const T& a, const T& b) { return (a >= b) ? a : b; }

    template<typename T>
    inline constexpr T min(const T& a, const T& b) { return (a <= b) ? a : b; }

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

    F64 round_to_infinity(F64 x);

    inline U64 hash(U64 x) { return x; }

    U64 bit_count(U64 x);
    U64 bit_count_trailing_zeros(U64 x);
    U64 bit_count_leading_zeros(U64 x);
}