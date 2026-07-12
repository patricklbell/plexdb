module cql.engine.column_value;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

using namespace plexdb;

namespace cql {
    static S32 clamp_cmp(S32 c) {
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

    template<typename T>
    static S32 compare_scalar(T a, T b) {
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    static S32 compare_bytes(const U8* a, U64 alen, const U8* b, U64 blen) {
        U64 n = alen < blen ? alen : blen;
        S32 c = n > 0 ? os::memory_compare(a, b, n) : 0;
        if (c != 0) {
            return clamp_cmp(c);
        }
        return compare_scalar(alen, blen);
    }

    // @note flips the sign bit (positive) or inverts all bits (negative) for an unsigned
    // total order; NaN payload ordering is not reproduced.
    static U64 f64_order_key(F64 v) {
        U64 bits;
        os::memory_copy(&bits, &v, sizeof(bits));
        return (bits & 0x8000000000000000ULL) ? ~bits : (bits ^ 0x8000000000000000ULL);
    }
    static U32 f32_order_key(F32 v) {
        U32 bits;
        os::memory_copy(&bits, &v, sizeof(bits));
        return (bits & 0x80000000U) ? ~bits : (bits ^ 0x80000000U);
    }

    // RFC4122 v1 timestamp reassembly: time_low | time_mid | (time_hi & 0x0FFF).
    static U64 timeuuid_timestamp(const UUID& u) {
        U32 time_low = (U32(u.value[0]) << 24) | (U32(u.value[1]) << 16) | (U32(u.value[2]) << 8) | U32(u.value[3]);
        U16 time_mid = (U16(u.value[4]) << 8) | U16(u.value[5]);
        U16 time_hi  = ((U16(u.value[6]) << 8) | U16(u.value[7])) & 0x0FFF;
        return (U64(time_hi) << 48) | (U64(time_mid) << 32) | U64(time_low);
    }
    static S32 compare_timeuuid(const UUID& a, const UUID& b) {
        U64 ta = timeuuid_timestamp(a);
        U64 tb = timeuuid_timestamp(b);
        if (ta != tb) {
            return compare_scalar(ta, tb);
        }
        return compare_bytes(&a.value[8], 8, &b.value[8], 8);
    }
    // Cassandra's UUIDType.compareCustom: compare by version nibble first; version-1
    // (time-based) UUIDs then compare by embedded timestamp; everything else compares
    // as raw unsigned bytes.
    static S32 compare_uuid(const UUID& a, const UUID& b) {
        U8 va = (a.value[6] >> 4) & 0x0F;
        U8 vb = (b.value[6] >> 4) & 0x0F;
        if (va != vb) {
            return compare_scalar(va, vb);
        }
        if (va == 1) {
            return compare_timeuuid(a, b);
        }
        return compare_bytes(&a.value[0], UUID::length, &b.value[0], UUID::length);
    }

    // Strips leading zero bytes conceptually rather than assuming a canonical
    // (non-zero-padded) magnitude representation.
    static void magnitude_bounds(const DynamicArray<U8>& m, U64& start, U64& len) {
        start = 0;
        while (start < m.length && m.ptr[start] == 0) {
            start++;
        }
        len = m.length - start;
    }
    static S32 compare_magnitude(const DynamicArray<U8>& a, const DynamicArray<U8>& b) {
        U64 a_start, a_len, b_start, b_len;
        magnitude_bounds(a, a_start, a_len);
        magnitude_bounds(b, b_start, b_len);
        if (a_len != b_len) {
            return compare_scalar(a_len, b_len);
        }
        if (a_len == 0) {
            return 0;
        }
        return clamp_cmp(os::memory_compare(a.ptr + a_start, b.ptr + b_start, a_len));
    }
    static bool magnitude_is_zero(const DynamicArray<U8>& m) {
        U64 start, len;
        magnitude_bounds(m, start, len);
        return len == 0;
    }
    static S32 compare_varint(const VarInt& a, const VarInt& b) {
        bool a_zero = magnitude_is_zero(a.magnitude);
        bool b_zero = magnitude_is_zero(b.magnitude);
        if (a_zero && b_zero) {
            return 0;
        }
        if (a.negative != b.negative) {
            if (a_zero) {
                return b.negative ? 1 : -1;
            }
            if (b_zero) {
                return a.negative ? -1 : 1;
            }
            return a.negative ? -1 : 1;
        }
        S32 m = compare_magnitude(a.magnitude, b.magnitude);
        return a.negative ? -m : m;
    }

    // Multiply a big-endian unsigned magnitude by 10^times, growing as needed.
    static DynamicArray<U8> magnitude_mul_pow10(const DynamicArray<U8>& m, U32 times) {
        DynamicArray<U8> cur;
        resize(cur, m.length);
        if (m.length > 0) {
            os::memory_copy(cur.ptr, m.ptr, m.length);
        }
        for (U32 t = 0; t < times; t++) {
            U32 carry = 0;
            for (U64 i = cur.length; i-- > 0;) {
                U32 v      = U32(cur.ptr[i]) * 10 + carry;
                cur.ptr[i] = U8(v & 0xFF);
                carry      = v >> 8;
            }
            while (carry > 0) {
                DynamicArray<U8> grown;
                resize(grown, cur.length + 1);
                grown.ptr[0] = U8(carry & 0xFF);
                if (cur.length > 0) {
                    os::memory_copy(grown.ptr + 1, cur.ptr, cur.length);
                }
                carry = carry >> 8;
                cur   = move(grown);
            }
        }
        return cur;
    }
    // Aligns scale (Decimal value = unscaled * 10^-scale) by growing the
    // coarser-scale side's magnitude, then delegates to compare_varint.
    static S32 compare_decimal(const Decimal& a, const Decimal& b) {
        if (a.scale == b.scale) {
            return compare_varint(a.unscaled, b.unscaled);
        }
        VarInt av = a.unscaled;
        VarInt bv = b.unscaled;
        if (a.scale > b.scale) {
            bv.magnitude = magnitude_mul_pow10(bv.magnitude, U32(a.scale - b.scale));
        } else {
            av.magnitude = magnitude_mul_pow10(av.magnitude, U32(b.scale - a.scale));
        }
        return compare_varint(av, bv);
    }

    S32 compare_column_value(const ColumnValue& a, const ColumnValue& b, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time:
                return compare_scalar(get<S64>(a), get<S64>(b));
            case type::Basic::int_:
            case type::Basic::date:
                return compare_scalar(get<S32>(a), get<S32>(b));
            case type::Basic::smallint:
                return compare_scalar(get<S16>(a), get<S16>(b));
            case type::Basic::tinyint:
                return compare_scalar(static_cast<S8>(get<U8>(a)), static_cast<S8>(get<U8>(b)));
            case type::Basic::boolean:
                return compare_scalar(get<U8>(a), get<U8>(b));
            case type::Basic::double_:
                return compare_scalar(f64_order_key(get<F64>(a)), f64_order_key(get<F64>(b)));
            case type::Basic::float_:
                return compare_scalar(f32_order_key(get<F32>(a)), f32_order_key(get<F32>(b)));
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                const AutoString8& sa = get<AutoString8>(a);
                const AutoString8& sb = get<AutoString8>(b);
                return compare_bytes(reinterpret_cast<const U8*>(sa.c_str), sa.length, reinterpret_cast<const U8*>(sb.c_str), sb.length);
            }
            case type::Basic::blob:
            case type::Basic::hex: {
                const Blob& ba = get<Blob>(a);
                const Blob& bb = get<Blob>(b);
                return compare_bytes(ba.value.ptr, ba.value.length, bb.value.ptr, bb.value.length);
            }
            case type::Basic::inet: {
                const Inet& ia = get<Inet>(a);
                const Inet& ib = get<Inet>(b);
                U64         la = ia.is_v6 ? 16 : 4;
                U64         lb = ib.is_v6 ? 16 : 4;
                return compare_bytes(ia.is_v6 ? &ia.v6[0] : &ia.v4[0], la, ib.is_v6 ? &ib.v6[0] : &ib.v4[0], lb);
            }
            case type::Basic::uuid:
                return compare_uuid(get<UUID>(a), get<UUID>(b));
            case type::Basic::timeuuid:
                return compare_timeuuid(get<UUID>(a), get<UUID>(b));
            case type::Basic::varint:
                return compare_varint(get<VarInt>(a), get<VarInt>(b));
            case type::Basic::decimal:
                return compare_decimal(get<Decimal>(a), get<Decimal>(b));
            case type::Basic::duration:
                assert_true(false, "duration has no total order defined (not usable as a clustering/index key column)");
                return 0;
            case type::Basic::COUNT:
                break;
        }
        assert_true(false, "invalid basic type in compare_column_value");
        return 0;
    }
}

namespace plexdb {
    AutoString8 to_str(const cql::ColumnValue& value, [[maybe_unused]] cql::type::Basic dtype) {
        return visit(value, [](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (Either<T, AutoString8, S64, S32, S16, U8, F64, F32>) {
                return to_str(v);
            } else if constexpr (SameAs<T, cql::Null>) {
                return "null"_as;
            } else if constexpr (SameAs<T, cql::UUID>) {
                AutoString8 hex = bytes_to_hex(&v.value[0], 16);
                AutoString8 result{36};
                const char* h = hex.c_str;
                char*       o = result.c_str;
                for (int i = 0; i < 8; i++) {
                    o[i] = h[i];
                }
                o[8] = '-';
                for (int i = 0; i < 4; i++) {
                    o[9 + i] = h[8 + i];
                }
                o[13] = '-';
                for (int i = 0; i < 4; i++) {
                    o[14 + i] = h[12 + i];
                }
                o[18] = '-';
                for (int i = 0; i < 4; i++) {
                    o[19 + i] = h[16 + i];
                }
                o[23] = '-';
                for (int i = 0; i < 12; i++) {
                    o[24 + i] = h[20 + i];
                }
                return result;
            } else if constexpr (SameAs<T, cql::Blob>) {
                return "0x"_as + bytes_to_hex(v.value.ptr, v.value.length);
            } else if constexpr (SameAs<T, cql::Inet>) {
                if (v.is_v6) {
                    AutoString8 result = ""_as;
                    for (int i = 0; i < 8; i++) {
                        if (i > 0) {
                            result = result + ":";
                        }
                        result = result + bytes_to_hex(&v.v6[i * 2], 2);
                    }
                    return result;
                } else {
                    return to_str(static_cast<U64>(v.v4[0])) + "." + to_str(static_cast<U64>(v.v4[1])) + "." + to_str(static_cast<U64>(v.v4[2])) + "." + to_str(static_cast<U64>(v.v4[3]));
                }
            } else if constexpr (SameAs<T, cql::VarInt>) {
                AutoString8 prefix = v.negative ? "-0x"_as : "0x"_as;
                return prefix + bytes_to_hex(v.magnitude.ptr, v.magnitude.length);
            } else if constexpr (SameAs<T, cql::Decimal>) {
                AutoString8 prefix = v.unscaled.negative ? "-0x"_as : "0x"_as;
                return prefix + bytes_to_hex(v.unscaled.magnitude.ptr, v.unscaled.magnitude.length) + "e-" + to_str(static_cast<S64>(v.scale));
            } else if constexpr (SameAs<T, cql::Duration>) {
                return to_str(static_cast<S64>(v.months)) + "mo" + to_str(static_cast<S64>(v.days)) + "d" + to_str(v.nanoseconds) + "ns";
            }
            assert_not_implemented("to_str for collection ColumnValue types is not implemented");
            return ""_as;
        });
    }

    AutoString8 to_str(const cql::ColumnValue& value, const cql::type::Type& cdtype) {
        if (type_matches_tag<cql::type::Basic>(cdtype.value)) {
            return to_str(value, get<cql::type::Basic>(cdtype.value));
        }
        return visit(value, [&cdtype](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, DynamicArray<cql::NestedColumnValue>>) {
                if (type_matches_tag<cql::type::Tuple>(cdtype.value)) {
                    const auto& t      = get<cql::type::Tuple>(cdtype.value);
                    AutoString8 result = "("_as;
                    for (U64 i = 0; i < v.length; i++) {
                        if (i > 0) {
                            result = result + ", ";
                        }
                        if (i < t.elements.length) {
                            result = result + to_str(v[i].value, t.elements[i]);
                        }
                    }
                    return result + ")";
                }
                if (type_matches_tag<cql::type::UDT*>(cdtype.value)) {
                    cql::type::UDT* u = get<cql::type::UDT*>(cdtype.value);
                    assert_true(u != nullptr, "UDT* column type is null");
                    AutoString8 result = "{"_as;
                    for (U64 i = 0; i < v.length; i++) {
                        if (i > 0) {
                            result = result + ", ";
                        }
                        if (i < u->field_names.length) {
                            result = result + AutoString8(u->field_names[i]) + ": ";
                            result = result + to_str(v[i].value, u->field_types[i]);
                        }
                    }
                    return result + "}";
                }
                bool        is_vec = type_matches_tag<cql::type::Vector>(cdtype.value);
                const auto& elem   = is_vec ? get<cql::type::Vector>(cdtype.value).element : get<cql::type::List>(cdtype.value).element;
                AutoString8 result = "["_as;
                bool        first  = true;
                for (const auto& el : v) {
                    if (!first) {
                        result = result + ", ";
                    }
                    result = result + to_str(el.value, elem);
                    first  = false;
                }
                return result + "]";
            } else if constexpr (SameAs<T, DynamicSet<cql::NestedColumnValue>>) {
                AutoString8 result = "{"_as;
                bool        first  = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) {
                        result = result + ", ";
                    }
                    result = result + to_str((*it).value, get<cql::type::Set>(cdtype.value).key);
                    first  = false;
                }
                return result + "}";
            } else if constexpr (SameAs<T, DynamicMap<cql::NestedColumnValue, cql::NestedColumnValue>>) {
                const auto& m      = get<cql::type::Map>(cdtype.value);
                AutoString8 result = "{"_as;
                bool        first  = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) {
                        result = result + ", ";
                    }
                    result = result + to_str((*it).first.value, m.key);
                    result = result + ": ";
                    result = result + to_str((*it).second.value, m.value);
                    first  = false;
                }
                return result + "}";
            } else {
                return to_str(cql::ColumnValue{v}, get<cql::type::Basic>(cdtype.value));
            }
        });
    }
}

namespace cql {
    static U64 mix_hash(U64 h, U64 x) {
        h ^= x;
        h *= 0x9e3779b97f4a7c15ULL;
        h ^= h >> 32;
        return h;
    }

    // @note recursion terminates: collection arms recurse through NestedColumnValue, whose
    // basic and Null arms are non-recursive leaves.
    bool operator==(const NestedColumnValue& a, const NestedColumnValue& b) {
        return visit(a.value, [&b](const auto& av) -> bool {
            using T = Decay<decltype(av)>;
            if constexpr (SameAs<T, Null>) {
                return type_matches_tag<Null>(b.value);
            } else if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                return type_matches_tag<T>(b.value) && av == get<T>(b.value);
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                if (!type_matches_tag<T>(b.value)) {
                    return false;
                }
                const auto& bv = get<T>(b.value);
                if (av.length != bv.length) {
                    return false;
                }
                for (U64 i = 0; i < av.length; i++) {
                    if (!(av[i] == bv[i])) {
                        return false;
                    }
                }
                return true;
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                // @note iteration order is bucket-layout-dependent, so membership-check both
                // directions rather than walking in lockstep.
                if (!type_matches_tag<T>(b.value)) {
                    return false;
                }
                const auto& bv = get<T>(b.value);
                if (length(av) != length(bv)) {
                    return false;
                }
                for (auto it = av.begin(); it != av.end(); ++it) {
                    if (!contains(bv, *it)) {
                        return false;
                    }
                }
                return true;
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                if (!type_matches_tag<T>(b.value)) {
                    return false;
                }
                const auto& bv = get<T>(b.value);
                if (length(av) != length(bv)) {
                    return false;
                }
                for (auto it = av.begin(); it != av.end(); ++it) {
                    const NestedColumnValue* bval = find(bv, (*it).first);
                    if (bval == nullptr || !(*bval == (*it).second)) {
                        return false;
                    }
                }
                return true;
            } else {
                static_assert(!SameAs<T, T>, "unhandled ColumnValue arm in NestedColumnValue equality");
                return false;
            }
        });
    }

    U64 hash(const NestedColumnValue& ncv) {
        return visit(ncv.value, [](const auto& v) -> U64 {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, Null>) {
                return 0x9e3779b97f4a7c15ULL;
            } else if constexpr (SameAs<T, AutoString8>) {
                return plexdb::hash(v);
            } else if constexpr (Either<T, S64, S32, S16, U8>) {
                return plexdb::hash(U64(v));
            } else if constexpr (Either<T, F32, F64>) {
                // @note canonicalize the sign bit of zero so equal values hash equal.
                T normalized = (v == T(0)) ? T(0) : v;
                return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&normalized), sizeof(normalized)));
            } else if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                return hash(v);
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                U64 h = 0x1_u64;
                for (const auto& el : v) {
                    h = mix_hash(h, hash(el));
                }
                return h;
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                // @note commutative combination keeps this order-independent, matching the
                // membership-based operator== above.
                U64 h = 0x2_u64;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    h += mix_hash(0x2_u64, hash(*it));
                }
                return h;
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                U64 h = 0x3_u64;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    U64 pair_h = mix_hash(hash((*it).first), hash((*it).second));
                    h += mix_hash(0x3_u64, pair_h);
                }
                return h;
            } else {
                static_assert(!SameAs<T, T>, "unhandled ColumnValue arm in NestedColumnValue hash");
                return 0;
            }
        });
    }
}
