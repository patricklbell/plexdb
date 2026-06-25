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
    enum class KeyKind : U8 {
        None          = 0,
        PartitionKey  = 1,
        ClusteringKey = 2
    };

    struct Column {
        bool             tombstone;
        bool             is_static = false;
        String8          name;
        const type::Type type;
        KeyKind          key_kind     = KeyKind::None;
        U16              key_position = 0;
        // @note clustering_order only meaningful when key_kind == ClusteringKey; default ASC.
        Sort clustering_order = Sort::ASC;
    };

// Stored as the fixed-size value in the outer (partition) BTree for all tables.
// data_page: row blob page (no clustering keys) or clustering BTree page (with clustering keys).
// static_page: 0 if no static row exists for that partition; otherwise the static row blob page.
#pragma pack(push, 1)
    struct PartitionEntry {
        U64 data_page;
        U64 static_page;
    };
#pragma pack(pop)
    static_assert(sizeof(PartitionEntry) == 16);

    // Outer partition BTree: pk_bytes → PartitionEntry
    using PartitionBTree = btree::BTreePaged<btree::VarlenKeyPolicy<>, btree::FixedValuePolicy<sizeof(PartitionEntry)>>;
    // Inner clustering BTree: ck_bytes → row_page (U64)
    using ClusteringBTree = btree::BTreePaged<btree::VarlenKeyPolicy<>, btree::FixedValuePolicy<sizeof(U64)>>;
    // Secondary index BTree: index_key → dummy (1 byte); all data is in the key
    using IndexBTree = btree::BTreePaged<btree::VarlenKeyPolicy<>, btree::FixedValuePolicy<1>>;

    enum class IndexKind : U8 {
        Values  = 0,
        Keys    = 1,
        Entries = 2,
        Full    = 3,
    };

    struct Index {
        U64        idx; // index into storage.indexes
        bool       tombstone;
        String8    name;
        U64        col_idx; // which column in tbl.cols is indexed
        IndexKind  kind;
        IndexBTree btree;
    };

    struct Table {
        U64                  idx;
        bool                 tombstone;
        String8              name;
        DynamicArray<Column> cols;
        DynamicArray<U64>    partition_key_col_indices;  // sorted by key_position
        DynamicArray<U64>    clustering_key_col_indices; // sorted by key_position
        DynamicArray<U64>    static_col_indices;         // col indices where is_static == true
        DynamicArray<Index>  indexes;
        PartitionBTree       btree;
        S64                  default_ttl_ms              = 0;
        S32                  gc_grace_seconds            = 864000;
        S32                  min_index_interval          = 128;
        S32                  max_index_interval          = 2048;
        S32                  memtable_flush_period_in_ms = 0;
    };

    bool has_clustering_keys(const Table& tbl) {
        return tbl.clustering_key_col_indices.length > 0;
    }

    struct Keyspace {
        U64                 idx;
        bool                tombstone;
        String8             name;
        DynamicArray<Table> tbls;
    };

#pragma pack(push, 1)
    struct SchemaHeader {
        U64 keyspaces_page;
        U64 tables_page;
        U64 columns_page;
        U64 types_page;
        U64 indexes_page;
    };
    struct KeyspaceHeader {
        bool tombstone;
        U64  name_length;
    };
    struct TableHeader {
        bool tombstone;
        U64  name_length;
        U64  keyspace_idx;
        U64  btree_page;
        // @note WITH default_time_to_live in milliseconds; 0 = no default TTL.
        S64 default_ttl_ms;
        S32 gc_grace_seconds;
        S32 min_index_interval;
        S32 max_index_interval;
        S32 memtable_flush_period_in_ms;
    };

    enum TypeRegistryKind : U8 {
        List,
        Set,
        Map,
        Vector,
    };

    // IDs 0-21 map directly to type::Basic enum values (no registry entry).
    // IDs >= 22 index into the types_blob registry (entry index = id - 22).
    inline constexpr U32 type_registry_base = 22;
    struct TypeRegistryEntry {
        TypeRegistryKind kind;
        U32              elem_id;   // element/key type ID
        U32              val_id;    // value type ID (Map only)
        U64              vec_count; // Vector only
        bool             frozen;
    };
    struct ColumnHeader {
        bool    tombstone;
        bool    is_static;
        U64     name_length;
        U32     type_id;
        U64     table_idx;
        KeyKind key_kind;
        U16     key_position;
        Sort    clustering_order;
    };
    struct IndexHeader {
        bool      tombstone;
        U64       name_length;
        U64       table_idx;
        U64       col_idx;
        U64       btree_page;
        IndexKind kind;
    };
#pragma pack(pop)

    enum class ReplicationClass {
        Unknown,
        SimpleStrategy,
        NetworkTopologyStrategy,
    };
    struct IndexStorage {
        U64         offset_in_blob_bytes;
        IndexHeader header;
        AutoString8 name;
    };

    // @todo use long held transaction with pager cache as storage?
    struct KeyspaceStorage {
        U64               offset_in_blob_bytes;
        KeyspaceHeader    header;
        AutoString8       name;
        DynamicArray<U64> tables;
        ReplicationClass  replication_class;
        U64               replication_factor;
        bool              do_durable_writes;
    };
    struct TableStorage {
        U64               offset_in_blob_bytes;
        TableHeader       header;
        AutoString8       name;
        DynamicArray<U64> columns;
    };
    struct ColumnStorage {
        U64          offset_in_blob_bytes;
        ColumnHeader header;
        AutoString8  name;
    };

    struct Storage {
        DynamicArray<ColumnStorage>     columns;
        DynamicArray<TableStorage>      tables;
        DynamicArray<KeyspaceStorage>   keyspaces;
        DynamicArray<TypeRegistryEntry> type_entries;
        DynamicArray<IndexStorage>      indexes;
    };

    struct Schema {
        DynamicArray<Keyspace> keyspaces;
        Storage                storage;

        blob::BlobStaticPaged  schema_blob;
        blob::BlobDynamicPaged keyspaces_blob;
        blob::BlobDynamicPaged tables_blob;
        blob::BlobDynamicPaged columns_blob;
        blob::BlobDynamicPaged types_blob;
        blob::BlobDynamicPaged indexes_blob;

        Schema() = default;
    };

    coroutine::Task<>    load(Schema& schema, Pager* in_pager, U64 page);
    coroutine::Task<U64> create_schema(Pager& pager);

    enum class Error {
        None,
        InvalidOptions,
        SyntaxOptions,
        MissingKeyspace,
        MissingPrimaryKey,
        ColumnNameCollision,
        MissingTable,
        MissingColumn,
        MissingIndex,
    };
    template<typename T>
    struct Result {
        T       value;
        Error   error   = Error::None;
        String8 message = "";
    };
    template<>
    struct Result<void> {
        Error   error   = Error::None;
        String8 message = "";
    };

    Result<Keyspace*> read_keyspace(Schema& schema, String8 name);
    Result<Table*>    read_table(Schema& schema, Keyspace& ks, String8 name);
    Result<Column*>   read_column(Schema& schema, Table& tbl, String8 name);

    coroutine::Task<Result<Keyspace*>> create_keyspace(Schema& schema, const CreateKeyspace& create);
    Result<void>                       validate_keyspace_options(const Options& opts);
    coroutine::Task<Result<void>>      delete_keyspace(Schema& schema, String8 name);

    struct TableExtraOptions {
        S32 gc_grace_seconds            = 864000;
        S32 min_index_interval          = 128;
        S32 max_index_interval          = 2048;
        S32 memtable_flush_period_in_ms = 0;
    };

    coroutine::Task<Result<Table*>> create_table(Schema& schema, Keyspace& ks, const CreateTable& create, S64 default_ttl_ms = 0, TableExtraOptions extras = {});
    coroutine::Task<Result<void>>   set_default_ttl_ms(Schema& schema, Table& tbl, S64 default_ttl_ms);
    coroutine::Task<Result<void>>   set_table_extra_options(Schema& schema, Table& tbl, TableExtraOptions extras);
    coroutine::Task<Result<void>>   delete_table(Schema& schema, Keyspace& ks, String8 name);

    coroutine::Task<Result<Column*>> create_column(Schema& schema, Table& tbl, const ColumnDefinition& create, KeyKind key_kind = KeyKind::None, U16 key_position = 0, Sort clustering_order = Sort::ASC);
    coroutine::Task<Result<void>>    delete_column(Schema& schema, Table& tbl, String8 name);

    Result<Index*>                  read_index(Schema& schema, Table& tbl, String8 name);
    coroutine::Task<Result<Index*>> create_index(Schema& schema, Table& tbl, U64 col_idx, String8 index_name, IndexKind kind);
    coroutine::Task<Result<void>>   drop_index(Schema& schema, Table& tbl, String8 name);
}
