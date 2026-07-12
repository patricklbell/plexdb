export module cql.engine.column_value;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.types;

using namespace plexdb;

export namespace cql {
    struct NestedColumnValue; // @note forward declare for recursive collection types

    // @todo support UDTs
    using ColumnValueBasicTypes = TypeList<
        AutoString8, S64, S32, S16, U8, F64, F32,
        Blob, UUID, Inet, VarInt, Decimal, Duration>;
    using ColumnValueTypes = Concat<
        ColumnValueBasicTypes,
        TypeList<
            DynamicArray<NestedColumnValue>,
            DynamicSet<NestedColumnValue>,
            DynamicMap<NestedColumnValue, NestedColumnValue>>,
        TypeList<Null>>;
    using ColumnValue = ExpandTaggedUnion<ColumnValueTypes>;

    struct NestedColumnValue {
        ColumnValue value;
    };

    inline U64 mix_hash(U64 h, U64 x) {
        h ^= x;
        h *= 0x9e3779b97f4a7c15ULL;
        h ^= h >> 32;
        return h;
    }

    // @note recursion terminates: collection arms recurse through NestedColumnValue, whose
    // basic and Null arms are non-recursive leaves.
    inline bool operator==(const NestedColumnValue& a, const NestedColumnValue& b) {
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
                // @note sets are unordered: membership-check both directions rather than
                // walking iteration order, which is bucket-layout-dependent (core/dynamic/containers.cppm)
                // and not guaranteed to agree between two sets holding the same elements.
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
                // @note same bucket-order caveat as DynamicSet above: look each key up by value
                // rather than walking both maps in lockstep.
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

    inline U64 hash(const NestedColumnValue& ncv) {
        return visit(ncv.value, [](const auto& v) -> U64 {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, Null>) {
                return 0x9e3779b97f4a7c15ULL;
            } else if constexpr (SameAs<T, AutoString8>) {
                return plexdb::hash(v);
            } else if constexpr (Either<T, S64, S32, S16, U8>) {
                return plexdb::hash(U64(v));
            } else if constexpr (Either<T, F32, F64>) {
                // @note -0.0 == 0.0 under operator== above, so canonicalize the sign bit before
                // hashing raw bytes — otherwise equal values could hash unequal.
                T normalized = (v == T(0)) ? T(0) : v;
                return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&normalized), sizeof(normalized)));
            } else if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                // UUID, Blob, Inet, VarInt, Decimal, Duration — found via ADL in cql namespace
                return hash(v);
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                // @note lists are ordered, so folding sequentially (order-dependent) is correct here.
                U64 h = 0x1_u64;
                for (const auto& el : v) {
                    h = mix_hash(h, hash(el));
                }
                return h;
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                // @note sets are unordered and iteration order is bucket-layout-dependent
                // (core/dynamic/containers.cppm), so elements are combined with a commutative `+`
                // after an order-independent per-element mix, keeping hash consistent with the
                // membership-based operator== above regardless of insertion history.
                U64 h = 0x2_u64;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    h += mix_hash(0x2_u64, hash(*it));
                }
                return h;
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                // @note same unordered-combination rationale as DynamicSet; key/value order within
                // a single pair is fixed so mixing those sequentially is fine.
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

    S32 compare_column_value(const ColumnValue& a, const ColumnValue& b, type::Basic dtype);
}

export namespace plexdb {
    AutoString8 to_str(const cql::ColumnValue& value, cql::type::Basic dtype);
    AutoString8 to_str(const cql::ColumnValue& value, const cql::type::Type& cdtype);
}
