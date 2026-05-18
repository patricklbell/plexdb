export module plexdb.btree.policy;

import plexdb.base;
import plexdb.os;

export namespace plexdb::btree {
    // ========================================================================
    // default varlen comparator
    // ========================================================================
    struct LexicographicComparator {
        Ordering operator()(TArrayView<const U8, U16> a, TArrayView<const U8, U16> b) const noexcept {
            U16 min_len = a.length < b.length ? a.length : b.length;
            S32 res = os::memory_compare(a.ptr, b.ptr, min_len);
            if (res < 0) return Ordering::Less;
            if (res > 0) return Ordering::Greater;
            if (a.length < b.length) return Ordering::Less;
            if (a.length > b.length) return Ordering::Greater;
            return Ordering::Equal;
        }
    };

    // ========================================================================
    // concepts
    // ========================================================================
    template<typename P>
    concept KeyPolicy = requires {
        typename P::key_type;
        { P::is_fixed_size } -> ConvertibleTo<bool>;
    } && requires(const P& p, typename P::key_type a, typename P::key_type b,
                  U8* dst, const U8* src, U16 len) {
        { stored_key_size(p, a) } -> SameAs<U16>;
        { write_key(p, dst, a) };
        { read_key(p, src, len) } -> SameAs<typename P::key_type>;
        { compare_key(p, a, b) }  -> SameAs<Ordering>;
    };

    template<typename P>
    concept ValuePolicy = requires {
        typename P::value_type;
        { P::is_fixed_size } -> ConvertibleTo<bool>;
    } && requires(const P& p, typename P::value_type v, U8* dst) {
        { stored_value_size(p, v) } -> SameAs<U16>;
        { write_value(p, dst, v) };
    };

    struct U64KeyPolicy {
        using key_type = U64;
        static constexpr bool is_fixed_size = true;
        static constexpr U16 key_stride = sizeof(U64);

        friend U16 stored_key_size(U64KeyPolicy, U64) noexcept { return sizeof(U64); }
        friend void write_key(U64KeyPolicy, U8* dst, U64 k) noexcept {
            os::memory_copy(dst, &k, sizeof(U64));
        }
        friend U64 read_key(U64KeyPolicy, const U8* src, U16) noexcept {
            U64 k; os::memory_copy(&k, src, sizeof(U64)); return k;
        }
        friend Ordering compare_key(U64KeyPolicy, U64 a, U64 b) noexcept {
            return a < b ? Ordering::Less : a > b ? Ordering::Greater : Ordering::Equal;
        }
    };

    template<typename Comparator = LexicographicComparator>
    struct VarlenKeyPolicy {
        using key_type = TArrayView<const U8, U16>;
        static constexpr bool is_fixed_size = false;
        [[no_unique_address]] Comparator comparator{};

        friend U16 stored_key_size(const VarlenKeyPolicy&, TArrayView<const U8, U16> k) noexcept {
            return k.length;
        }
        friend void write_key(const VarlenKeyPolicy&, U8* dst, TArrayView<const U8, U16> k) noexcept {
            os::memory_copy(dst, k.ptr, k.length);
        }
        friend TArrayView<const U8, U16> read_key(const VarlenKeyPolicy&, const U8* src, U16 len) noexcept {
            return {const_cast<U8*>(src), len};
        }
        friend Ordering compare_key(const VarlenKeyPolicy& p,
                                    TArrayView<const U8, U16> a,
                                    TArrayView<const U8, U16> b) noexcept {
            return p.comparator(a, b);
        }
    };

    struct FixedValuePolicy {
        using value_type = TArrayView<const U8, U16>;
        static constexpr bool is_fixed_size = true;
        U16 stride;

        friend U16 stored_value_size(FixedValuePolicy p, TArrayView<const U8, U16>) noexcept {
            return p.stride;
        }
        friend void write_value(FixedValuePolicy p, U8* dst, TArrayView<const U8, U16> v) noexcept {
            os::memory_copy(dst, v.ptr, p.stride);
        }
    };

    struct VarlenValuePolicy {
        using value_type = TArrayView<const U8, U16>;
        static constexpr bool is_fixed_size = false;

        friend U16 stored_value_size(VarlenValuePolicy, TArrayView<const U8, U16> v) noexcept {
            return v.length;
        }
        friend void write_value(VarlenValuePolicy, U8* dst, TArrayView<const U8, U16> v) noexcept {
            os::memory_copy(dst, v.ptr, v.length);
        }
    };
}
