export module objstore.engine.virtual_table;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;

import objstore.engine.io;
import objstore.engine.types;

using namespace plexdb;

export namespace objstore::engine {
    struct VirtualColumn {
        String8 name;
        Type type;
    };

    struct VirtualRow {
        DynamicArray<ColumnValue> values;
    };

    struct VirtualRows {
        String8 keyspace;
        String8 table;
        DynamicArray<VirtualColumn> columns;
        DynamicArray<VirtualRow> rows;
    };
}