export module cql.engine.virtual_table;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

import cql.engine.column_value;
import cql.engine.io;
import cql.engine.types;
import cql.engine.statements;

using namespace plexdb;

export namespace cql::engine {
    struct VirtualColumn {
        String8    name;
        type::Type type;
    };

    struct VirtualRow {
        DynamicArray<ColumnValue> values;
    };

    struct VirtualRows {
        String8                     keyspace;
        String8                     table;
        DynamicArray<VirtualColumn> columns;
        DynamicArray<VirtualRow>    rows;
    };
}
