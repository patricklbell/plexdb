export module cql.engine.schema;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.coroutine;
import plexdb.pager;
import plexdb.blob;
import plexdb.btree;

import cql.engine.statements;
import cql.engine.types;
import cql.engine.clustering_compare;

using namespace plexdb;

// ============================================================================
// schema
// ============================================================================
//
// On-disk layout (see Schema below):
//
//   schema_blob (static, sizeof(SchemaHeader)):
//     SchemaHeader { U64 keyspaces_page, tables_page, columns_page, udts_page, indexes_page }
//
//   keyspaces_blob (dynamic, append-only):
//     [KeyspaceHeader][name bytes]  repeated, one record per keyspace
//
//   tables_blob (dynamic, append-only):
//     [TableHeader][name bytes]  repeated, one record per table
//
//   columns_blob (dynamic, append-only):
//     [ColumnHeader][name bytes][serialized type::Type]  repeated, one record per column
//
//   udts_blob (dynamic, append-only):
//     Two record kinds, discriminated by UdtRecordHeader.is_field:
//       UDT-decl (is_field=0):
//         [UdtRecordHeader][keyspace bytes][name bytes]
//       UDT-field (is_field=1):
//         [UdtRecordHeader][field name bytes][serialized type::Type]
//     Field records always follow their parent UDT-decl record (not directly) and reference
//     it via UdtRecordHeader.parent_udt_record_idx (the count of preceding UDT-decl records).
//
//   indexes_blob (dynamic, append-only):
//     [IndexHeader][name bytes]  repeated, one record per index
//
// Type serialization (inline within columns_blob and UDT-field records):
//     TypeKind kind (U8), then per-kind:
//       basic kinds (kind < Basic::COUNT):   nothing
//       List / Set:                          U8 frozen, then 1 sub-type
//       Map:                                 U8 frozen, then 2 sub-types
//       Vector:                              U8 frozen, U64 count, then 1 sub-type
//       Tuple:                               U8 frozen, U64 count, then `count` sub-types
//       Udt:                                 U8 frozen, U64 name_length, name bytes
//     UDT references serialize the name only; resolution happens against the column's
//     parent keyspace (or the udt-field's parent UDT keyspace) at load time.
// ============================================================================
export namespace cql::schema {
#pragma pack(push, 1)
    struct PartitionEntry {
        // if (table has clustering keys) then (clustering btree page) else (row blob page)
        U64 data_page;
        // if (this partition contains static columns) then (static row blob page) else (0)
        U64 static_page;
    };
    static_assert(sizeof(PartitionEntry) == 16);
#pragma pack(pop)

    // Outer partition BTree: Murmur3 token (S64) → PartitionEntry
    using PartitionBTree = btree::BTreePaged<btree::FixedKeyPolicy<S64>, btree::FixedValuePolicy<sizeof(PartitionEntry)>>;

    // Inner clustering BTree: ck_bytes → row_page (U64)
    using ClusteringKeyPolicy = btree::VarlenKeyPolicy<NumericLimits<U16>::max(), clustering_compare::ClusteringKeyComparator>;
    using ClusteringBTree     = btree::BTreePaged<ClusteringKeyPolicy, btree::FixedValuePolicy<sizeof(U64)>>;

#pragma pack(push, 1)
    // static_page/row_page of the indexed row, captured at index-entry-write time so a hit
    // jumps straight to the row without re-walking the partition/clustering BTrees. row_page
    // is 0 for an entry keyed off a static column (no single row owns the value).
    struct IndexEntry {
        U64 static_page;
        U64 row_page;
    };
    static_assert(sizeof(IndexEntry) == 16);
#pragma pack(pop)

    // Secondary index BTree: index_key → IndexEntry. Same key policy shape as
    // ClusteringBTree — an index key is a longer sequence of typed components (indexed
    // value, then partition token, then pk columns, then ck columns; see Index::key_specs)
    // using the identical wire encoding and comparator.
    using IndexBTree = btree::BTreePaged<ClusteringKeyPolicy, btree::FixedValuePolicy<sizeof(IndexEntry)>>;
}

export namespace cql::schema {
    enum class KeyKind : U8 {
        None = 0,
        PartitionKey,
        ClusteringKey,
    };

    // @note first part of enum maps directly to basic types (TypeKind value < Basic::COUNT
    // serializes as a basic type)
    enum TypeKind : U8 {
        List = static_cast<U8>(type::Basic::COUNT),
        Set,
        Map,
        Vector,
        Tuple,
        Udt,
    };

    enum class IndexKind : U8 {
        Values = 0,
        Keys,
        Entries,
        Full,
    };

    enum class ReplicationClass : U8 {
        Unknown = 0,
        SimpleStrategy,
        NetworkTopologyStrategy,
    };

    struct Column {
        bool       tombstone;
        bool       is_static = false;
        String8    name;
        type::Type type;
        KeyKind    key_kind     = KeyKind::None;
        U16        key_position = 0;
        // @note clustering_order only meaningful when key_kind == ClusteringKey; default ASC.
        Sort clustering_order = Sort::ASC;
    };

    struct Index {
        U64        idx; // index into storage.indexes
        bool       tombstone;
        String8    name;
        U64        col_idx; // which column in tbl.cols is indexed
        IndexKind  kind;
        IndexBTree btree;
        // Flat component spec for this index's BTree key, computed once (see
        // make_index_key_specs): [indexed value component(s) — 1, or 2 for Entries kind] ++
        // [bigint: the embedded partition token] ++ [table's partition-key columns, ASC] ++
        // [table's clustering-key columns, = tbl.clustering_key_specs]. Feeds
        // make_index_key_policy the same way Table::clustering_key_specs feeds
        // make_clustering_key_policy.
        DynamicArray<clustering_compare::ClusteringColumnSpec> key_specs;
    };

    // @note packed POD; persisted byte-identical inside TableHeader.
#pragma pack(push, 1)
    struct TableOptions {
        S64 default_ttl_ms              = 0;
        S32 gc_grace_seconds            = 864000;
        S32 min_index_interval          = 128;
        S32 max_index_interval          = 2048;
        S32 memtable_flush_period_in_ms = 0;
    };
    static_assert(sizeof(TableOptions) == 24);
#pragma pack(pop)

    struct Table {
        U64                  idx;
        bool                 tombstone;
        String8              name;
        DynamicArray<Column> cols;
        DynamicArray<U64>    partition_key_col_indices;  // sorted by key_position
        DynamicArray<U64>    clustering_key_col_indices; // sorted by key_position
        // One entry per clustering_key_col_indices entry, same order — feeds ClusteringKeyComparator.
        DynamicArray<clustering_compare::ClusteringColumnSpec> clustering_key_specs;
        DynamicArray<U64>                                      static_col_indices;  // sorted alphabetically by name
        DynamicArray<U64>                                      regular_col_indices; // sorted alphabetically by name
        // Cassandra SELECT * ordering: partition, clustering, static (alphabetical), regular (alphabetical).
        DynamicArray<U64>        select_star_col_indices;
        DynamicArray<Index>      indexes;
        DynamicMap<String8, U64> cols_by_name;
        DynamicMap<String8, U64> indexes_by_name;
        PartitionBTree           btree;
        TableOptions             options;
    };

    bool has_clustering_keys(const Table& tbl) {
        return tbl.clustering_key_col_indices.length > 0;
    }

    ClusteringKeyPolicy make_clustering_key_policy(const Table& tbl) {
        return ClusteringKeyPolicy{
            clustering_compare::ClusteringKeyComparator{
                TArrayView<const clustering_compare::ClusteringColumnSpec, U64>(tbl.clustering_key_specs.ptr, tbl.clustering_key_specs.length)
            }
        };
    }

    // Builds Index::key_specs: the flat component sequence an index-BTree key encodes.
    // [indexed value component(s) — 1, or 2 for IndexKind::Entries (map key then map value)]
    // ++ [bigint: the embedded partition token, see key::serialize_partition_index_bytes]
    // ++ [table's partition-key columns, ASC — pk order never varies] ++ [table's
    // clustering-key columns, verbatim]. Called once at index create/load time (schema.cpp),
    // not per BTree construction — mirrors clustering_key_specs' lifetime.
    DynamicArray<clustering_compare::ClusteringColumnSpec> make_index_key_specs(const Table& tbl, U64 col_idx, IndexKind kind) {
        DynamicArray<clustering_compare::ClusteringColumnSpec> specs;
        const type::Type&                                      col_type = tbl.cols[col_idx].type;
        auto                                                   push     = [&](type::Basic b) {
            push_back(specs, clustering_compare::ClusteringColumnSpec{b, Sort::ASC});
        };

        visit(col_type.value, [&](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                push(v);
            } else if constexpr (SameAs<T, type::List> || SameAs<T, type::Vector>) {
                push(get<type::Basic>(v.element.value));
            } else if constexpr (SameAs<T, type::Set>) {
                push(get<type::Basic>(v.key.value));
            } else if constexpr (SameAs<T, type::Map>) {
                switch (kind) {
                    case IndexKind::Values:
                        push(get<type::Basic>(v.value.value));
                        break;
                    case IndexKind::Keys:
                        push(get<type::Basic>(v.key.value));
                        break;
                    case IndexKind::Entries:
                        push(get<type::Basic>(v.key.value));
                        push(get<type::Basic>(v.value.value));
                        break;
                    case IndexKind::Full:
                        assert_not_implemented("Full map index lookup");
                        break;
                }
            } else {
                assert_not_implemented("index key spec for this column type");
            }
        });

        push(type::Basic::bigint); // embedded partition token
        for (U64 pk_ci : tbl.partition_key_col_indices) {
            assert_true(type_matches_tag<type::Basic>(tbl.cols[pk_ci].type.value), "partition key must be a basic type");
            push(get<type::Basic>(tbl.cols[pk_ci].type.value));
        }
        for (const auto& s : tbl.clustering_key_specs) {
            push_back(specs, s);
        }
        return specs;
    }

    ClusteringKeyPolicy make_index_key_policy(const Index& idx) {
        return ClusteringKeyPolicy{
            clustering_compare::ClusteringKeyComparator{
                TArrayView<const clustering_compare::ClusteringColumnSpec, U64>(idx.key_specs.ptr, idx.key_specs.length)
            }
        };
    }

    struct Keyspace {
        U64                             idx;
        bool                            tombstone;
        String8                         name;
        DynamicArray<Table>             tbls;
        DynamicMap<String8, U64>        tbls_by_name;
        DynamicMap<String8, type::UDT*> udts_by_name;
        ReplicationClass                replication_class  = ReplicationClass::Unknown;
        U64                             replication_factor = 0;
        bool                            do_durable_writes  = false;
    };

    // ========================================================================
    // on-disk headers
    // ========================================================================
#pragma pack(push, 1)
    struct SchemaHeader {
        U64 keyspaces_page;
        U64 tables_page;
        U64 columns_page;
        U64 udts_page;
        U64 indexes_page;
    };
    static_assert(sizeof(SchemaHeader) == 40);

    struct KeyspaceHeader {
        U8               tombstone;
        ReplicationClass replication_class;
        U8               do_durable_writes;
        U64              replication_factor;
        U64              name_length; // payload: name bytes
    };
    static_assert(sizeof(KeyspaceHeader) == 19);

    struct TableHeader {
        U8           tombstone;
        U64          keyspace_idx;
        U64          btree_page;
        TableOptions options;
        U64          name_length; // payload: name bytes
    };
    static_assert(sizeof(TableHeader) == 49);

    struct ColumnHeader {
        U8      tombstone;
        U8      is_static;
        KeyKind key_kind;
        Sort    clustering_order;
        U16     key_position;
        U64     table_idx;
        U64     name_length; // payload: name bytes, then serialized type
    };
    static_assert(sizeof(ColumnHeader) == 22);

    struct IndexHeader {
        U8        tombstone;
        IndexKind kind;
        U64       table_idx;
        U64       col_idx;
        U64       btree_page;
        U64       name_length; // payload: name bytes
    };
    static_assert(sizeof(IndexHeader) == 34);

    // Single record header for the UDTs blob; covers both UDT-decl records (is_field=0)
    // and UDT-field records (is_field=1). See file-top layout comment.
    struct UdtRecordHeader {
        U8  tombstone;
        U8  is_field;
        U64 parent_udt_record_idx; // is_field=1 only: index of parent UDT-decl record in blob order
        U64 keyspace_length;       // is_field=0 only: length of keyspace bytes
        U64 name_length;           // payload: name bytes (UDT name or field name)
    };
    static_assert(sizeof(UdtRecordHeader) == 26);
#pragma pack(pop)

    // ========================================================================
    // storage (parallel to runtime view; carries blob offsets for in-place mutation)
    // ========================================================================
    struct KeyspaceStorage {
        U64               offset_in_blob_bytes;
        KeyspaceHeader    header;
        AutoString8       name;
        DynamicArray<U64> tables; // table indices into Storage.tables
    };
    struct TableStorage {
        U64               offset_in_blob_bytes;
        TableHeader       header;
        AutoString8       name;
        DynamicArray<U64> columns; // column indices into Storage.columns
    };
    struct ColumnStorage {
        U64          offset_in_blob_bytes;
        ColumnHeader header;
        AutoString8  name;
        type::Type   type;
    };
    struct IndexStorage {
        U64         offset_in_blob_bytes;
        IndexHeader header;
        AutoString8 name;
    };
    // UDT-decl record. Field records are written to the udts_blob too but are folded
    // back into the parent UdtStorage on load (their state lives in field_names/field_types).
    struct UdtStorage {
        U64             offset_in_blob_bytes;
        UdtRecordHeader header;
        AutoString8     keyspace;
        AutoString8     name;
        // owning storage for field names; type::UDT in Schema.udts views into these via String8.
        DynamicArray<AutoString8> field_names;
        DynamicArray<type::Type>  field_types;
        // matching record offsets in the udts blob, one entry per (live) field record
        DynamicArray<U64> field_record_offsets;
    };

    struct Storage {
        DynamicArray<ColumnStorage>   columns;
        DynamicArray<TableStorage>    tables;
        DynamicArray<KeyspaceStorage> keyspaces;
        DynamicArray<IndexStorage>    indexes;
        DynamicArray<UdtStorage>      udts;
    };

    // ========================================================================
    // schema
    // ========================================================================
    struct Schema {
        DynamicArray<Keyspace>   keyspaces;
        DynamicMap<String8, U64> keyspaces_by_name;

        // @warn pointers into this deque are stable for the deque's lifetime. type::UDT*
        // references from columns and from Keyspace.udts_by_name point into here.
        DynamicDeque<type::UDT> udts;

        Storage storage;

        blob::BlobStaticPaged  schema_blob;
        blob::BlobDynamicPaged keyspaces_blob;
        blob::BlobDynamicPaged tables_blob;
        blob::BlobDynamicPaged columns_blob;
        blob::BlobDynamicPaged udts_blob;
        blob::BlobDynamicPaged indexes_blob;

        // @note in-memory schema epoch: monotonic counter bumped on every DDL mutation.
        U64 version = 0;

        Schema() = default;
    };

    inline void bump_version(Schema& schema) {
        schema.version++;
    }

    coroutine::Task<>    load(Schema& schema, Pager* in_pager, U64 page);
    coroutine::Task<U64> create_schema(Pager& pager);

    // ========================================================================
    // errors and result wrapper
    // ========================================================================
    enum class Error {
        None = 0,
        InvalidOptions,
        SyntaxOptions,
        MissingKeyspace,
        MissingPrimaryKey,
        ColumnNameCollision,
        MissingTable,
        MissingColumn,
        MissingIndex,
        MissingType,
        TypeInUse,
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

    // ========================================================================
    // option parsing (single source of truth for create/alter)
    // ========================================================================
    struct KeyspaceOptionsParsed {
        ReplicationClass replication_class  = ReplicationClass::SimpleStrategy;
        U64              replication_factor = 1;
        bool             do_durable_writes  = true;
    };
    Result<KeyspaceOptionsParsed> parse_keyspace_options(const Options& opts);

    struct TableOptionsParsed {
        TableOptions options;
    };
    Result<TableOptionsParsed> parse_table_options(const CreateTable::TableOptions& opts);

    // ========================================================================
    // lookups (O(1) via *_by_name maps; tombstoned entries are excluded)
    // ========================================================================
    Result<Keyspace*>  read_keyspace(Schema& schema, String8 name);
    Result<Table*>     read_table(Schema& schema, Keyspace& ks, String8 name);
    Result<Column*>    read_column(Schema& schema, Table& tbl, String8 name);
    Result<Index*>     read_index(Schema& schema, Table& tbl, String8 name);
    Result<type::UDT*> read_udt(Schema& schema, Keyspace& ks, String8 name);

    // ========================================================================
    // DDL
    // ========================================================================
    coroutine::Task<Result<Keyspace*>> create_keyspace(Schema& schema, const CreateKeyspace& create);
    coroutine::Task<Result<void>>      alter_keyspace(Schema& schema, Keyspace& ks, const Options& opts);
    coroutine::Task<Result<void>>      delete_keyspace(Schema& schema, String8 name);

    coroutine::Task<Result<Table*>> create_table(Schema& schema, Keyspace& ks, const CreateTable& create);
    coroutine::Task<Result<void>>   set_table_options(Schema& schema, Table& tbl, const TableOptions& options);
    coroutine::Task<Result<void>>   delete_table(Schema& schema, Keyspace& ks, String8 name);

    coroutine::Task<Result<Column*>> create_column(Schema& schema, Table& tbl, String8 name, type::Type type, bool is_static = false, KeyKind key_kind = KeyKind::None, U16 key_position = 0, Sort clustering_order = Sort::ASC);
    coroutine::Task<Result<void>>    delete_column(Schema& schema, Table& tbl, String8 name);

    coroutine::Task<Result<Index*>> create_index(Schema& schema, Table& tbl, U64 col_idx, String8 index_name, IndexKind kind);
    coroutine::Task<Result<void>>   delete_index(Schema& schema, Table& tbl, String8 name);

    // ========================================================================
    // user-defined types
    // ========================================================================

    struct UdtDependent {
        AutoString8 description;
    };
    DynamicArray<UdtDependent> find_udt_dependents(Schema& schema, const Keyspace& ks, String8 name);

    coroutine::Task<Result<type::UDT*>> create_udt(Schema& schema, Keyspace& ks, String8 name, DynamicArray<AutoString8>&& field_names, DynamicArray<type::Type>&& field_types);
    coroutine::Task<Result<void>>       alter_udt_add_field(Schema& schema, Keyspace& ks, String8 name, AutoString8 field_name, type::Type field_type);
    coroutine::Task<Result<void>>       alter_udt_rename_fields(Schema& schema, Keyspace& ks, String8 name, const DynamicArray<Pair<ColumnName, ColumnName>>& renames);
    coroutine::Task<Result<void>>       delete_udt(Schema& schema, Keyspace& ks, String8 name);
}
