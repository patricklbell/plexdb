export module objstore.engine.schema;

import plexdb.base;
import plexdb.os.containers;
import plexdb.pager;
import plexdb.blob;
import plexdb.btree;

import objstore.engine.dtype;
import objstore.engine.statements;

using namespace plexdb;

export namespace objstore::schema {
    struct Column {
        bool tombstone;
        String8 name;
        const CqlType type;
    };
    
    struct Table {
        U64 idx;
        bool tombstone;
        String8 name;
        DynamicArray<Column> cols;
        U64 primary_col_idx;
        btree::BTreePaged btree;
    };

    struct Keyspace {
        U64 idx;
        bool tombstone;
        String8 name;
        DynamicArray<Table> tbls;
    };

    #pragma pack(push, 1)
    struct SchemaHeader {
        U64 keyspaces_page;
        U64 tables_page;
        U64 columns_page;
    };
    struct KeyspaceHeader {
        bool tombstone;
        U64 name_length;
    };
    struct TableHeader {
        bool tombstone;
        U64 name_length;
        U64 keyspace_idx;
        U64 btree_page;
    };
    struct ColumnHeader {
        bool tombstone;
        U64 name_length;
        CqlType type;
        U64 table_idx;
    };
    #pragma pack(pop)

    // @todo use long held transaction with pager cache as storage?
    struct KeyspaceStorage {
        U64 offset_in_blob_bytes;
        KeyspaceHeader header;
        AutoString8 name;
        DynamicArray<U64> tables;
    };
    struct TableStorage {
        U64 offset_in_blob_bytes;
        TableHeader header;
        AutoString8 name;
        DynamicArray<U64> columns;
    };
    struct ColumnStorage {
        U64 offset_in_blob_bytes;
        ColumnHeader header;
        AutoString8 name;
    };

    struct Storage {
        DynamicArray<ColumnStorage> columns;
        DynamicArray<TableStorage> tables;
        DynamicArray<KeyspaceStorage> keyspaces;
    };

    struct Schema {
        DynamicArray<Keyspace> keyspaces;
        Storage storage;

        blob::BlobStaticPaged schema_blob;
        blob::BlobDynamicPaged keyspaces_blob;
        blob::BlobDynamicPaged tables_blob;
        blob::BlobDynamicPaged columns_blob;

        Schema(Pager* in_pager, U64 page);
    };

    U64 create_schema(Pager& pager);

    Keyspace* create_keyspace(Schema& schema, const CreateKeyspace& create);
    Keyspace* read_keyspace(Schema& schema, String8 name);
    bool delete_keyspace(Schema& schema, String8 name);

    Table* create_table(Schema& schema, Keyspace& ks, const CreateTable& create);
    Table* read_table(Schema& schema, Keyspace& ks, String8 name);
    bool delete_table(Schema& schema, Keyspace& ks, String8 name);

    Column* create_column(Schema& schema, Table& tbl, const ColumnDefinition& create);
    Column* read_column(Schema& schema, Table& tbl, String8 name);
    bool delete_column(Schema& schema, Table& tbl, String8 name);
}