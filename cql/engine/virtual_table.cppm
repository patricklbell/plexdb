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
        AutoString8                 keyspace;
        AutoString8                 table;
        DynamicArray<VirtualColumn> columns;
        DynamicArray<VirtualRow>    rows;
        // Owns label bytes when a column name is dynamically computed (aliases,
        // ttl(...)/writetime(...) labels). Static literals and schema-owned column
        // names go directly into VirtualColumn::name without an entry here.
        // @note callers must reserve capacity to match `columns.length` before pushing
        // so the String8 views stay valid across appends.
        DynamicArray<AutoString8> column_name_storage;
    };
}
