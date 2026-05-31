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
        ExpandDynamicArray<ColumnValueBasicTypes>,
        ExpandDynamicMap<ColumnValueBasicTypes, ColumnValueBasicTypes>,
        ExpandDynamicSet<ColumnValueBasicTypes>,
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
}
