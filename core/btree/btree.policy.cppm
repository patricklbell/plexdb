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
            S32 res     = os::memory_compare(a.ptr, b.ptr, min_len);
            if (res < 0) {
                return Ordering::Less;
            }
            if (res > 0) {
                return Ordering::Greater;
            }
            if (a.length < b.length) {
                return Ordering::Less;
            }
            if (a.length > b.length) {
                return Ordering::Greater;
            }
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
    } && requires(const P& p, typename P::key_type a, typename P::key_type b, U8* dst, const U8* src, U16 len) {
        { stored_key_size(p, a) } -> SameAs<U16>;
        { write_key(p, dst, a) };
        { read_key(p, src, len) } -> SameAs<typename P::key_type>;
        { compare_key(p, a, b) } -> SameAs<Ordering>;
    };

    template<typename P>
    concept ValuePolicy = requires {
        typename P::value_type;
        { P::is_fixed_size } -> ConvertibleTo<bool>;
    } && requires(const P& p, typename P::value_type v, U8* dst) {
        { stored_value_size(p, v) } -> SameAs<U16>;
        { write_value(p, dst, v) };
    };

    template<TriviallyCopyable T>
    struct FixedKeyPolicy {
        static_assert(sizeof(T) <= NumericLimits<U16>::max(), "key stride must fit in U16");

        using key_type                      = T;
        static constexpr bool is_fixed_size = true;
        static constexpr U16  key_stride    = sizeof(T);

        friend U16 stored_key_size(FixedKeyPolicy, T) noexcept {
            return sizeof(T);
        }
        friend void write_key(FixedKeyPolicy, U8* dst, T k) noexcept {
            os::memory_copy(dst, &k, sizeof(T));
        }
        friend T read_key(FixedKeyPolicy, const U8* src, U16 len) noexcept {
            assert_true(len == sizeof(T), "key length mismatch for fixed key policy");
            T k;
            os::memory_copy(&k, src, sizeof(T));
            return k;
        }
        friend Ordering compare_key(FixedKeyPolicy, T a, T b) noexcept {
            return a < b ? Ordering::Less : a > b ? Ordering::Greater
                                                  : Ordering::Equal;
        }
    };

    template<U16 max_key_length = NumericLimits<U16>::max(), typename Comparator = LexicographicComparator>
    struct VarlenKeyPolicy {
        using key_type                                 = TArrayView<const U8, U16>;
        static constexpr bool            is_fixed_size = false;
        static constexpr U16             max_length    = max_key_length;
        [[no_unique_address]] Comparator comparator{};

        friend U16 stored_key_size(const VarlenKeyPolicy&, TArrayView<const U8, U16> k) noexcept {
            assert_true(k.length <= max_key_length, "key exceeds VarlenKeyPolicy's max_key_length");
            return k.length;
        }
        friend void write_key(const VarlenKeyPolicy&, U8* dst, TArrayView<const U8, U16> k) noexcept {
            assert_true(k.length <= max_key_length, "key exceeds VarlenKeyPolicy's max_key_length");
            os::memory_copy(dst, k.ptr, k.length);
        }
        friend TArrayView<const U8, U16> read_key(const VarlenKeyPolicy&, const U8* src, U16 len) noexcept {
            assert_true(len <= max_key_length, "stored key length exceeds VarlenKeyPolicy's max_key_length");
            return {const_cast<U8*>(src), len};
        }
        friend Ordering compare_key(const VarlenKeyPolicy& p, TArrayView<const U8, U16> a, TArrayView<const U8, U16> b) noexcept {
            return p.comparator(a, b);
        }
    };

    template<U64 stride_byte_count>
    struct FixedValuePolicy {
        static_assert(stride_byte_count > 0 && stride_byte_count <= NumericLimits<U16>::max(), "value stride must be nonzero and fit in U16");

        using value_type                    = TArrayView<const U8, U16>;
        static constexpr bool is_fixed_size = true;
        static constexpr U16  value_stride  = stride_byte_count;

        friend U16 stored_value_size(FixedValuePolicy, TArrayView<const U8, U16>) noexcept {
            return stride_byte_count;
        }
        friend void write_value(FixedValuePolicy, U8* dst, TArrayView<const U8, U16> v) noexcept {
            assert_true(v.length <= static_cast<U16>(stride_byte_count), "value too large for fixed stride");
            os::memory_copy(dst, v.ptr, v.length);
            os::memory_zero(dst + v.length, stride_byte_count - v.length);
        }
    };

    template<U16 max_value_length = NumericLimits<U16>::max()>
    struct VarlenValuePolicy {
        using value_type                    = TArrayView<const U8, U16>;
        static constexpr bool is_fixed_size = false;
        static constexpr U16  max_length    = max_value_length;

        friend U16 stored_value_size(VarlenValuePolicy, TArrayView<const U8, U16> v) noexcept {
            assert_true(v.length <= max_value_length, "value exceeds VarlenValuePolicy's max_value_length");
            return v.length;
        }
        friend void write_value(VarlenValuePolicy, U8* dst, TArrayView<const U8, U16> v) noexcept {
            assert_true(v.length <= max_value_length, "value exceeds VarlenValuePolicy's max_value_length");
            os::memory_copy(dst, v.ptr, v.length);
        }
    };
}
