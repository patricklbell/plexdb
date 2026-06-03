export module cql.engine.column_value;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.types;

using namespace plexdb;

export namespace cql {
    struct NestedColumnValue;  // @note forward declare for recursive collection types

    // @todo support UDTs
    using ColumnValueBasicTypes = TypeList<
        AutoString8, S64, S32, S16, U8, F64, F32,
        Blob, UUID, Inet, VarInt, Decimal, Duration
    >;
    using ColumnValueTypes = Concat<
        ColumnValueBasicTypes,
        TypeList<
            DynamicArray<NestedColumnValue>,
            DynamicSet<NestedColumnValue>,
            DynamicMap<NestedColumnValue, NestedColumnValue>
        >,
        TypeList<Null>
    >;
    using ColumnValue = ExpandTaggedUnion<ColumnValueTypes>;

    struct NestedColumnValue {
        ColumnValue value;
    };

    inline bool operator==(const NestedColumnValue& a, const NestedColumnValue& b) {
        return visit(a.value, [&b](const auto& av) -> bool {
            using T = Decay<decltype(av)>;
            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                return type_matches_tag<T>(b.value) && av == get<T>(b.value);
            } else {
                assert_not_implemented("equality for collection/null NestedColumnValue is not implemented");
                return false;
            }
        });
    }

    inline U64 hash(const NestedColumnValue& ncv) {
        return visit(ncv.value, [](const auto& v) -> U64 {
            using T = Decay<decltype(v)>;
            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                if constexpr (Either<T, F32, F64>) {
                    assert_not_implemented("hash for float/double NestedColumnValue is not valid for set/map key");
                    return 0;
                } else if constexpr (SameAs<T, AutoString8>) {
                    return plexdb::hash(v);
                } else if constexpr (Either<T, S64, S32, S16, U8>) {
                    return plexdb::hash(U64(v));
                } else {
                    // UUID, Blob, Inet, VarInt, Decimal, Duration — found via ADL in cql namespace
                    return hash(v);
                }
            } else {
                assert_not_implemented("hash for collection/null NestedColumnValue is not implemented");
                return 0;
            }
        });
    }
}
