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

    bool operator==(const NestedColumnValue& a, const NestedColumnValue& b);
    U64  hash(const NestedColumnValue& ncv);
    S32  compare_column_value(const ColumnValue& a, const ColumnValue& b, type::Basic dtype);
}

export namespace plexdb {
    AutoString8 to_str(const cql::ColumnValue& value, cql::type::Basic dtype);
    AutoString8 to_str(const cql::ColumnValue& value, const cql::type::Type& cdtype);
}
