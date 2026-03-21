export module objstore.engine.virtual_table;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;

import objstore.engine.types;

using namespace plexdb;

export namespace objstore::engine {
    struct VirtualColumn {
        String8 name;
        CqlType type;
    };

    struct VirtualRow {
        DynamicArray<types::ReadValue> values;
    };

    struct VirtualRows {
        String8 keyspace;
        String8 table;
        DynamicArray<VirtualColumn> columns;
        DynamicArray<VirtualRow> rows;
    };
}