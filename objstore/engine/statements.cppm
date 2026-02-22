export module objstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.tagged_union;

import objstore.engine.dtype;

using namespace plexdb;

export namespace objstore {
    constexpr U64 MAX_KEYSPACE_OPTIONS = 32;

    struct CreateKeyspaceOption {
        String8 key;
        TaggedUnion<AutoString8, S64> value;
    };

    struct CreateKeyspace {
        String8 keyspace_name;
        bool if_not_exists;
        CappedArray<CreateKeyspaceOption, MAX_KEYSPACE_OPTIONS> options;
    };

    struct CreateColumn {
        String8 name;
        DType dtype;
        bool is_primary_key;
    };

    struct CreateTable {
        String8 keyspace_name;
        String8 table_name;
        bool if_not_exists;
        DynamicArray<CreateColumn> columns;
    };

    // @todo support auto increment etc.
    
    struct InsertInto {
        String8 keyspace_name;
        String8 table_name;
        DynamicArray<dtype::WriteValue> values;
    };

    struct SelectFrom {
        String8 keyspace_name;
        String8 table_name;
        // @todo
    };

    struct Statement {
        TaggedUnion<
            CreateKeyspace,
            CreateTable,
            InsertInto,
            SelectFrom
        > value;
    };
}
