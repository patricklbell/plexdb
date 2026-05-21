export module cql.engine.schema;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.coroutine;
import plexdb.pager;
import plexdb.blob;
import plexdb.btree;

import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;

export namespace cql::schema {
    enum class KeyKind : U8 { None = 0, PartitionKey = 1, ClusteringKey = 2 };

    struct Column {
        bool tombstone;
        String8 name;
        const Type type;
        KeyKind key_kind = KeyKind::None;
        U16 key_position = 0;
    };

    using PartitionBTree = btree::BTreePaged<btree::VarlenKeyPolicy<>, btree::FixedValuePolicy<sizeof(U64)>>;

    struct Table {
        U64 idx;
        bool tombstone;
        String8 name;
        DynamicArray<Column> cols;
        DynamicArray<U64> partition_key_col_indices;   // sorted by key_position; replaces primary_col_idx
        DynamicArray<U64> clustering_key_col_indices;  // sorted by key_position
        PartitionBTree btree;
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
        U64 types_page;
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
    // IDs 0-21 map directly to BasicType enum values (no registry entry).
    // IDs >= 22 index into the types_blob registry (entry index = id - 22).
    inline constexpr U32 type_registry_base = 22;
    struct TypeRegistryEntry {
        U8  kind;       // 1=List, 2=Set, 3=Map, 4=Vector  (5=UDT future)
        U32 elem_id;    // element/key type ID
        U32 val_id;     // value type ID (Map only)
        U64 vec_count;  // Vector only
        bool frozen;
    };
    struct ColumnHeader {
        bool tombstone;
        U64 name_length;
        U32 type_id;
        U64 table_idx;
        KeyKind key_kind;
        U16 key_position;
    };
    #pragma pack(pop)

    enum class ReplicationClass {
        Unknown,
        SimpleStrategy,
        NetworkTopologyStrategy,
    };
    // @todo use long held transaction with pager cache as storage?
    struct KeyspaceStorage {
        U64 offset_in_blob_bytes;
        KeyspaceHeader header;
        AutoString8 name;
        DynamicArray<U64> tables;
        ReplicationClass replication_class;
        U64 replication_factor;
        bool do_durable_writes;
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
        DynamicArray<TypeRegistryEntry> type_entries;
    };

    struct Schema {
        DynamicArray<Keyspace> keyspaces;
        Storage storage;

        blob::BlobStaticPaged schema_blob;
        blob::BlobDynamicPaged keyspaces_blob;
        blob::BlobDynamicPaged tables_blob;
        blob::BlobDynamicPaged columns_blob;
        blob::BlobDynamicPaged types_blob;

        Schema() = default;
    };

    coroutine::Task<> load(Schema& schema, Pager* in_pager, U64 page);
    coroutine::Task<U64> create_schema(Pager& pager);

    enum class Error {
        None,
        InvalidOptions,
        MissingKeyspace,
        MissingPrimaryKey,
        ColumnNameCollision,
        MissingTable,
        MissingColumn,
    };
    template<typename T>
    struct Result {
        T value;
        Error error = Error::None;
        String8 message = "";
    };
    template<>
    struct Result<void> {
        Error error = Error::None;
        String8 message = "";
    };

    Result<Keyspace*> read_keyspace(Schema& schema, String8 name);
    Result<Table*>    read_table(Schema& schema, Keyspace& ks, String8 name);
    Result<Column*>   read_column(Schema& schema, Table& tbl, String8 name);

    coroutine::Task<Result<Keyspace*>> create_keyspace(Schema& schema, const CreateKeyspace& create);
    coroutine::Task<Result<void>>      delete_keyspace(Schema& schema, String8 name);

    coroutine::Task<Result<Table*>> create_table(Schema& schema, Keyspace& ks, const CreateTable& create);
    coroutine::Task<Result<void>>   delete_table(Schema& schema, Keyspace& ks, String8 name);

    coroutine::Task<Result<Column*>> create_column(Schema& schema, Table& tbl, const ColumnDefinition& create, KeyKind key_kind = KeyKind::None, U16 key_position = 0);
    coroutine::Task<Result<void>>    delete_column(Schema& schema, Table& tbl, String8 name);
}
