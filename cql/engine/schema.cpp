module;
#include <plexdb/macros/macros.h>
#include <coroutine>

module cql.engine.schema;

import plexdb.threads;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;

using namespace plexdb;

namespace cql::schema {
    static type::Type unpack_type(const Schema& schema, U32 id);
    static U32        register_type(Schema& schema, const type::Type& t);

    static U32 pack_type(Schema& schema, const type::Type& t) {
        if (type_matches_tag<type::Basic>(t.value)) {
            return static_cast<U32>(get<type::Basic>(t.value));
        }
        return register_type(schema, t);
    }

    static U32 register_type(Schema& schema, const type::Type& t) {
        if (type_matches_tag<type::Basic>(t.value)) {
            return static_cast<U32>(get<type::Basic>(t.value));
        }

        TypeRegistryEntry entry = visit(t.value, [&](const auto& v) -> TypeRegistryEntry {
            using T = RemoveCVRef<decltype(v)>;

            if constexpr (SameAs<T, type::Basic>) {
                assert_true(false, "unexpected type reached visitor, this should never happen!");
                return {};
            } else if constexpr (SameAs<T, type::List>) {
                return {TypeRegistryKind::List, pack_type(schema, v.element), 0, 0, v.frozen};
            } else if constexpr (SameAs<T, type::Set>) {
                return {TypeRegistryKind::Set, pack_type(schema, v.key), 0, 0, v.frozen};
            } else if constexpr (SameAs<T, type::Map>) {
                return {TypeRegistryKind::Map, pack_type(schema, v.key), pack_type(schema, v.value), 0, v.frozen};
            } else if constexpr (SameAs<T, type::Vector>) {
                return {TypeRegistryKind::Vector, pack_type(schema, v.element), 0, v.count, v.frozen};
            } else if constexpr (SameAs<T, type::Tuple>) {
                assert_not_implemented("tuple column type is not implemented");
                return {};
            } else {
                static_assert(!SameAs<T, T>, "unhandled Type variant in register_type");
                return {};
            }
        });

        for (U64 i = 0; i < schema.storage.type_entries.length; i++) {
            const TypeRegistryEntry& existing = schema.storage.type_entries[i];
            // @todo comparison operator, map
            if (existing.kind == entry.kind && existing.elem_id == entry.elem_id && existing.val_id == entry.val_id && existing.vec_count == entry.vec_count && existing.frozen == entry.frozen) {
                return static_cast<U32>(type_registry_base + i);
            }
        }

        push_back(schema.storage.type_entries, entry);
        return static_cast<U32>(type_registry_base + schema.storage.type_entries.length - 1);
    }

    static type::Type unpack_type(const Schema& schema, U32 id) {
        if (id < type_registry_base) {
            return type::create_basic(static_cast<type::Basic>(id));
        }
        const TypeRegistryEntry& e = schema.storage.type_entries[id - type_registry_base];
        switch (e.kind) {
            case TypeRegistryKind::List: {
                return type::create_list(unpack_type(schema, e.elem_id), e.frozen);
            }
            case TypeRegistryKind::Set: {
                return type::create_set(unpack_type(schema, e.elem_id), e.frozen);
            }
            case TypeRegistryKind::Map: {
                return type::create_map(unpack_type(schema, e.elem_id), unpack_type(schema, e.val_id), e.frozen);
            }
            case TypeRegistryKind::Vector: {
                return type::create_vector(unpack_type(schema, e.elem_id), e.vec_count, e.frozen);
            }
        }

        assert_true(false, "invalid type registry kind, io may be corrupted");
        return type::create_basic(type::Basic::int_);
    }

    coroutine::Task<void> get_str(blob::BlobDynamicPaged& blob, const AutoString8& str, U64* offset) {
        co_await blob::get(blob, reinterpret_cast<U8*>(str.c_str), str.length, *offset);
        *offset += str.length;
    };

    coroutine::Task<void> update_str(blob::BlobDynamicPaged& blob, String8 str, U64* offset) {
        co_await blob::update(blob, reinterpret_cast<const U8*>(str.data), str.length, *offset);
        *offset += str.length;
    };

    // @todo small schema storage optimization
    coroutine::Task<> load(Schema& schema, Pager* in_pager, U64 page) {
        co_await blob::load(schema.schema_blob, in_pager, page, sizeof(SchemaHeader));

        SchemaHeader schema_header{};
        co_await blob::tget(schema.schema_blob, &schema_header);

        co_await blob::load(schema.keyspaces_blob, in_pager, schema_header.keyspaces_page);
        co_await blob::load(schema.tables_blob, in_pager, schema_header.tables_page);
        co_await blob::load(schema.columns_blob, in_pager, schema_header.columns_page);
        co_await blob::load(schema.types_blob, in_pager, schema_header.types_page);
        co_await blob::load(schema.indexes_blob, in_pager, schema_header.indexes_page);

        //
        // allocate storage and cache blob contents in memory
        //
        for (U64 keyspace_offset_bytes = 0; keyspace_offset_bytes < schema.keyspaces_blob.size_bytes;) {
            KeyspaceStorage ks_storage{
                .offset_in_blob_bytes = keyspace_offset_bytes,
                .header               = {},
                .name                 = {},
                .tables               = {},
                .replication_class    = ReplicationClass::Unknown,
                .replication_factor   = 0,
                .do_durable_writes    = false,
            };

            co_await blob::tget(schema.keyspaces_blob, &ks_storage.header, &keyspace_offset_bytes);

            ks_storage.name = AutoString8{ks_storage.header.name_length};
            co_await get_str(schema.keyspaces_blob, ks_storage.name, &keyspace_offset_bytes);

            push_back(schema.storage.keyspaces, move(ks_storage));
        }

        for (U64 table_offset_bytes = 0; table_offset_bytes < schema.tables_blob.size_bytes;) {
            TableStorage tbl_storage{
                .offset_in_blob_bytes = table_offset_bytes,
                .header               = {},
                .name                 = {},
                .columns              = {},
            };

            co_await blob::tget(schema.tables_blob, &tbl_storage.header, &table_offset_bytes);

            tbl_storage.name = AutoString8{tbl_storage.header.name_length};
            co_await get_str(schema.tables_blob, tbl_storage.name, &table_offset_bytes);

            assert_true(tbl_storage.header.keyspace_idx < schema.storage.keyspaces.length, "invalid keyspace idx");
            KeyspaceStorage& ks_storage = schema.storage.keyspaces[tbl_storage.header.keyspace_idx];
            push_back(ks_storage.tables, schema.storage.tables.length);

            push_back(schema.storage.tables, move(tbl_storage));
        }

        for (U64 column_offset_bytes = 0; column_offset_bytes < schema.columns_blob.size_bytes;) {
            ColumnStorage col_storage{
                .offset_in_blob_bytes = column_offset_bytes,
                .header               = {},
                .name                 = {},
            };

            co_await blob::tget(schema.columns_blob, &col_storage.header, &column_offset_bytes);

            col_storage.name = AutoString8{col_storage.header.name_length};
            co_await get_str(schema.columns_blob, col_storage.name, &column_offset_bytes);

            assert_true(col_storage.header.table_idx < schema.storage.tables.length, "invalid table idx");
            TableStorage& tbl_storage = schema.storage.tables[col_storage.header.table_idx];
            push_back(tbl_storage.columns, schema.storage.columns.length);

            push_back(schema.storage.columns, move(col_storage));
        }

        for (U64 type_offset = 0; type_offset < schema.types_blob.size_bytes;) {
            TypeRegistryEntry entry{};
            co_await blob::tget(schema.types_blob, &entry, &type_offset);
            push_back(schema.storage.type_entries, entry);
        }

        for (U64 index_offset = 0; index_offset < schema.indexes_blob.size_bytes;) {
            IndexStorage idx_storage{
                .offset_in_blob_bytes = index_offset,
                .header               = {},
                .name                 = {},
            };
            co_await blob::tget(schema.indexes_blob, &idx_storage.header, &index_offset);
            idx_storage.name = AutoString8{idx_storage.header.name_length};
            co_await get_str(schema.indexes_blob, idx_storage.name, &index_offset);
            push_back(schema.storage.indexes, move(idx_storage));
        }

        //
        // resolves indices and construct views
        //
        reserve(schema.keyspaces, schema.storage.keyspaces.length);
        for (U64 ks_idx = 0; ks_idx < schema.storage.keyspaces.length; ks_idx++) {
            const KeyspaceStorage& ks_storage = schema.storage.keyspaces[ks_idx];
            Keyspace               ks{
                              .idx       = ks_idx,
                              .tombstone = ks_storage.header.tombstone,
                              .name      = ks_storage.name,
                              .tbls      = {},
            };
            for (const auto& tbl_idx : ks_storage.tables) {
                const TableStorage& tbl_storage = schema.storage.tables[tbl_idx];

                Table tbl{
                    .idx                        = tbl_idx,
                    .tombstone                  = tbl_storage.header.tombstone,
                    .name                       = tbl_storage.name,
                    .cols                       = {},
                    .partition_key_col_indices  = {},
                    .clustering_key_col_indices = {},
                    .static_col_indices         = {},
                    .indexes                    = {},
                    .btree                      = PartitionBTree{
                                                   in_pager, tbl_storage.header.btree_page,
                                                   btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(PartitionEntry)>{}
                    },
                    .default_ttl_ms              = tbl_storage.header.default_ttl_ms,
                    .gc_grace_seconds            = tbl_storage.header.gc_grace_seconds,
                    .min_index_interval          = tbl_storage.header.min_index_interval,
                    .max_index_interval          = tbl_storage.header.max_index_interval,
                    .memtable_flush_period_in_ms = tbl_storage.header.memtable_flush_period_in_ms,
                };

                reserve(tbl.cols, tbl_storage.columns.length);
                for (const auto& col_idx : tbl_storage.columns) {
                    const ColumnStorage& col_storage = schema.storage.columns[col_idx];

                    Column col{
                        .tombstone        = col_storage.header.tombstone,
                        .is_static        = col_storage.header.is_static,
                        .name             = col_storage.name,
                        .type             = unpack_type(schema, col_storage.header.type_id),
                        .key_kind         = col_storage.header.key_kind,
                        .key_position     = col_storage.header.key_position,
                        .clustering_order = col_storage.header.clustering_order,
                    };

                    push_back(tbl.cols, move(col));
                }

                // Build partition/clustering/static index arrays from column key_kind/key_position.
                // Use insertion sort on the small arrays to maintain position order.
                for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                    const Column& col = tbl.cols[ci];
                    if (col.key_kind == KeyKind::PartitionKey) {
                        U64 insert_pos = tbl.partition_key_col_indices.length;
                        push_back(tbl.partition_key_col_indices, ci);
                        while (insert_pos > 0 && tbl.cols[tbl.partition_key_col_indices[insert_pos - 1]].key_position > col.key_position) {
                            tbl.partition_key_col_indices[insert_pos]     = tbl.partition_key_col_indices[insert_pos - 1];
                            tbl.partition_key_col_indices[insert_pos - 1] = ci;
                            insert_pos--;
                        }
                    } else if (col.key_kind == KeyKind::ClusteringKey) {
                        U64 insert_pos = tbl.clustering_key_col_indices.length;
                        push_back(tbl.clustering_key_col_indices, ci);
                        while (insert_pos > 0 && tbl.cols[tbl.clustering_key_col_indices[insert_pos - 1]].key_position > col.key_position) {
                            tbl.clustering_key_col_indices[insert_pos]     = tbl.clustering_key_col_indices[insert_pos - 1];
                            tbl.clustering_key_col_indices[insert_pos - 1] = ci;
                            insert_pos--;
                        }
                    } else if (col.is_static) {
                        push_back(tbl.static_col_indices, ci);
                    }
                }

                push_back(ks.tbls, move(tbl));
            }
            push_back(schema.keyspaces, move(ks));
        }

        // @note post-pass: tables must already be built (above) before indexes can attach to them.
        for (U64 idx_storage_i = 0; idx_storage_i < schema.storage.indexes.length; idx_storage_i++) {
            const IndexStorage& idx_storage = schema.storage.indexes[idx_storage_i];
            assert_true(idx_storage.header.table_idx < schema.storage.tables.length, "invalid index table_idx");

            const TableStorage& tbl_storage = schema.storage.tables[idx_storage.header.table_idx];
            U64                 ks_idx      = tbl_storage.header.keyspace_idx;
            assert_true(ks_idx < schema.keyspaces.length, "invalid index keyspace_idx");
            Keyspace& ks = schema.keyspaces[ks_idx];

            for (auto& tbl : ks.tbls) {
                if (tbl.idx == idx_storage.header.table_idx) {
                    Index idx{
                        .idx       = idx_storage_i,
                        .tombstone = idx_storage.header.tombstone,
                        .name      = idx_storage.name,
                        .col_idx   = idx_storage.header.col_idx,
                        .kind      = idx_storage.header.kind,
                        .btree     = IndexBTree{
                                                in_pager, idx_storage.header.btree_page,
                                                btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<1>{}
                        },
                    };
                    push_back(tbl.indexes, move(idx));
                    break;
                }
            }
        }
    }

    coroutine::Task<U64> create_schema(Pager& pager) {
        U64 schema_page = co_await blob::create_paged_static(pager, sizeof(SchemaHeader));

        U64 keyspaces_page = co_await blob::create_paged_dynamic(pager, 0);
        U64 tables_page    = co_await blob::create_paged_dynamic(pager, 0);
        U64 columns_page   = co_await blob::create_paged_dynamic(pager, 0);
        U64 types_page     = co_await blob::create_paged_dynamic(pager, 0);
        U64 indexes_page   = co_await blob::create_paged_dynamic(pager, 0);

        SchemaHeader header{
            .keyspaces_page = keyspaces_page,
            .tables_page    = tables_page,
            .columns_page   = columns_page,
            .types_page     = types_page,
            .indexes_page   = indexes_page,
        };

        blob::BlobStaticPaged schema_blob;
        co_await blob::load(schema_blob, &pager, schema_page, sizeof(SchemaHeader));
        co_await blob::tupdate(schema_blob, &header);

        co_return schema_page;
    }

    //
    // @todo cache maps for lookups
    //

    static Result<Keyspace*> read_keyspace_impl(Schema& schema, String8 name) {
        for (auto& ks : schema.keyspaces) {
            if (ks.name == name && !ks.tombstone) {
                return {&ks};
            }
        }
        return {nullptr, Error::MissingKeyspace};
    }
    static Result<Table*> read_table_impl([[maybe_unused]] Schema& schema, Keyspace& ks, String8 name) {
        for (auto& tbl : ks.tbls) {
            if (tbl.name == name && !tbl.tombstone) {
                return {&tbl};
            }
        }
        return {nullptr, Error::MissingTable};
    }
    static Result<Column*> read_column_impl([[maybe_unused]] Schema& schema, Table& tbl, String8 name) {
        for (auto& col : tbl.cols) {
            if (col.name == name && !col.tombstone) {
                return {&col};
            }
        }
        return {nullptr, Error::MissingColumn};
    }

    Result<Keyspace*> read_keyspace(Schema& schema, String8 name) {
        return read_keyspace_impl(schema, name);
    }
    Result<Table*> read_table(Schema& schema, Keyspace& ks, String8 name) {
        return read_table_impl(schema, ks, name);
    }
    Result<Column*> read_column(Schema& schema, Table& tbl, String8 name) {
        return read_column_impl(schema, tbl, name);
    }

    [[nodiscard("option may be invalid")]]
    static Result<void> apply_keyspace_replication_option(KeyspaceStorage& storage, const OptionValue& option) {
        if (!type_matches_tag<MapLiteral>(option)) {
            return {Error::InvalidOptions, "replication option is not a map literal"};
        }

        const auto& map = get<MapLiteral>(option);

        // @todo @perf O(n^2) and duplicate keys should be an error at the top level
        // @note Cassandra rejects duplicate keys in the replication map at parse-time as a
        // SyntaxException. We surface the same shape as InvalidOptions; the engine maps both
        // to the wire-level Invalid response.
        for (U64 i = 0; i < map.key_values.length; i++) {
            const auto& ki = map.key_values[i].first;
            if (!type_matches_tag<Constant>(ki.value)) {
                continue;
            }
            const auto& kic = get<Constant>(ki.value);
            if (!type_matches_tag<AutoString8>(kic.value)) {
                continue;
            }
            const auto& ki_str = get<AutoString8>(kic.value);
            for (U64 j = i + 1; j < map.key_values.length; j++) {
                const auto& kj = map.key_values[j].first;
                if (!type_matches_tag<Constant>(kj.value)) {
                    continue;
                }
                const auto& kjc = get<Constant>(kj.value);
                if (!type_matches_tag<AutoString8>(kjc.value)) {
                    continue;
                }
                if (ki_str == get<AutoString8>(kjc.value)) {
                    return {Error::SyntaxOptions, "Multiple definitions for replication option"};
                }
            }
        }

        bool has_class       = false;
        U64  non_class_count = 0;
        for (const auto& [key_term, value_term] : map.key_values) {
            if (!type_matches_tag<Constant>(key_term.value)) {
                return {Error::InvalidOptions, "replication option key should be a string"};
            }
            const auto& key_constant = get<Constant>(key_term.value);
            if (!type_matches_tag<AutoString8>(key_constant.value)) {
                return {Error::InvalidOptions, "replication option key should be a string"};
            }
            const auto& key = get<AutoString8>(key_constant.value);

            if (key == "class") {
                has_class = true;
                if (!type_matches_tag<Constant>(value_term.value)) {
                    return {Error::InvalidOptions, "replication class should be a string"};
                }
                const auto& value_constant = get<Constant>(value_term.value);
                if (!type_matches_tag<AutoString8>(value_constant.value)) {
                    return {Error::InvalidOptions, "replication class should be a string"};
                }
                const auto& value = get<AutoString8>(value_constant.value);

                if (value == "SimpleStrategy") {
                    storage.replication_class = ReplicationClass::SimpleStrategy;
                } else if (value == "NetworkTopologyStrategy") {
                    // @note treat NetworkTopologyStrategy as SimpleStrategy for single-node deployment
                    // @todo implement proper multi-datacenter replication
                    storage.replication_class = ReplicationClass::NetworkTopologyStrategy;
                } else if (value == "org.apache.cassandra.locator.SimpleStrategy") {
                    storage.replication_class = ReplicationClass::SimpleStrategy;
                } else if (value == "org.apache.cassandra.locator.NetworkTopologyStrategy") {
                    storage.replication_class = ReplicationClass::NetworkTopologyStrategy;
                } else {
                    return {Error::InvalidOptions, "unknown replication class"};
                }
            } else if (key == "replication_factor") {
                non_class_count++;
                if (!type_matches_tag<Constant>(value_term.value)) {
                    return {Error::InvalidOptions, "replication factor should be an integer"};
                }
                const auto& value_constant = get<Constant>(value_term.value);
                S64         value          = 0;
                if (type_matches_tag<S64>(value_constant.value)) {
                    value = get<S64>(value_constant.value);
                } else if (type_matches_tag<AutoString8>(value_constant.value)) {
                    value = s64_from_str(get<AutoString8>(value_constant.value));
                    if (value == 0) {
                        return {Error::InvalidOptions, "replication factor should be an integer"};
                    }
                } else {
                    return {Error::InvalidOptions, "replication factor should be an integer"};
                }

                if (value <= 0) {
                    return {Error::InvalidOptions, "replication factor must be strictly positive"};
                }

                storage.replication_factor = value;
            } else {
                non_class_count++;
                // For NetworkTopologyStrategy, non-class keys are DC names and must match
                // a known DC. The current single-node build only knows "datacenter1".
                // SimpleStrategy ignores them.
                if (storage.replication_class == ReplicationClass::NetworkTopologyStrategy) {
                    if (key != "datacenter1") {
                        thread_local AutoString8 msg;
                        msg = AutoString8("Unrecognized datacenter name '") + String8(key) + AutoString8("'");
                        return {Error::InvalidOptions, String8(msg)};
                    }
                }
            }
        }

        (void)has_class;
        (void)non_class_count;
        return {};
    }

    [[nodiscard("option may be invalid")]]
    static Result<void> apply_keyspace_durable_writes_option(KeyspaceStorage& storage, const OptionValue& option) {
        if (!type_matches_tag<Constant>(option)) {
            return {Error::InvalidOptions, "durable write option should be a boolean"};
        }
        const auto& value_constant = get<Constant>(option);
        if (!type_matches_tag<bool>(value_constant.value)) {
            return {Error::InvalidOptions, "durable write option should be a boolean"};
        }
        const auto& value = get<bool>(value_constant.value);

        storage.do_durable_writes = value;
        return {};
    }

    [[nodiscard("options may be invalid")]]
    static Result<void> apply_keyspace_options(KeyspaceStorage& storage, const Options& opts) {
        // @todo determine if a replication strategy is required
        storage.replication_class  = ReplicationClass::SimpleStrategy;
        storage.replication_factor = 1;
        storage.do_durable_writes  = true;

        for (const auto& [key, value] : opts.identifier_values) {
            if (key == "replication") {
                if (auto res = apply_keyspace_replication_option(storage, value); res.error != Error::None) {
                    return res;
                }
            } else if (key == "durable_writes") {
                if (auto res = apply_keyspace_durable_writes_option(storage, value); res.error != Error::None) {
                    return res;
                }
            } else {
                return {Error::InvalidOptions, "Unknown option in keyspace WITH"};
            }
        }

        return {Error::None};
    }

    Result<void> validate_keyspace_options(const Options& opts) {
        KeyspaceStorage scratch{};
        scratch.replication_class  = ReplicationClass::SimpleStrategy;
        scratch.replication_factor = 1;
        scratch.do_durable_writes  = true;
        return apply_keyspace_options(scratch, opts);
    }

    coroutine::Task<Result<Keyspace*>> create_keyspace(Schema& schema, const CreateKeyspace& create) {
        assert_true(read_keyspace_impl(schema, create.name).error == Error::MissingKeyspace, "keyspace already exists");

        U64 offset_bytes = schema.keyspaces_blob.size_bytes;

        KeyspaceStorage ks_storage{
            .offset_in_blob_bytes = offset_bytes,
            .header               = KeyspaceHeader{
                                                   .tombstone   = false,
                                                   .name_length = create.name.length,
                                                   },
            .name               = AutoString8(create.name),
            .tables             = {              },
            .replication_class  = ReplicationClass::SimpleStrategy,
            .replication_factor = 1,
            .do_durable_writes  = true,
        };
        if (auto res = apply_keyspace_options(ks_storage, create.options); res.error != Error::None) {
            co_return Result<Keyspace*>{nullptr, res.error, res.message};
        }

        KeyspaceStorage& ks_storage_ref = push_back(schema.storage.keyspaces, move(ks_storage));

        co_await blob::resize(
            schema.keyspaces_blob, offset_bytes + sizeof(ks_storage_ref.header) + ks_storage_ref.name.length
        );

        co_await blob::tupdate(schema.keyspaces_blob, &ks_storage_ref.header, &offset_bytes);
        co_await update_str(schema.keyspaces_blob, ks_storage_ref.name, &offset_bytes);

        Keyspace ks{
            .idx       = schema.storage.keyspaces.length - 1,
            .tombstone = ks_storage_ref.header.tombstone,
            .name      = ks_storage_ref.name,
            .tbls      = {},
        };
        co_return Result<Keyspace*>{&push_back(schema.keyspaces, move(ks))};
    }

    coroutine::Task<Result<void>> delete_keyspace(Schema& schema, String8 name) {
        auto ks_res = read_keyspace_impl(schema, name);
        if (ks_res.error == Error::None) {
            const auto& ks = ks_res.value;
            ks->tombstone  = true;

            KeyspaceStorage& ks_storage = schema.storage.keyspaces[ks->idx];
            ks_storage.header.tombstone = true;

            U64 offset = ks_storage.offset_in_blob_bytes + offsetof(KeyspaceHeader, tombstone);
            co_await blob::tupdate(schema.keyspaces_blob, &ks_storage.header.tombstone, &offset);
            co_return Result<void>{};
        }
        co_return Result<void>{ks_res.error, ks_res.message};
    }

    struct PrimaryKeyInfo {
        DynamicArray<U64> partition_col_def_indices;  // column_definitions indices, in key position order
        DynamicArray<U64> clustering_col_def_indices; // column_definitions indices, in key position order
    };

    static Optional<PrimaryKeyInfo> get_primary_key_info(const CreateTable& create) {
        PrimaryKeyInfo info;

        // Inline PRIMARY KEY annotation: `col type PRIMARY KEY` — single partition key, no clustering
        for (U64 col_idx = 0; col_idx < create.column_definitions.length; col_idx++) {
            if (create.column_definitions[col_idx].primary_key) {
                push_back(info.partition_col_def_indices, col_idx);
                return info;
            }
        }

        // Standalone PRIMARY KEY clause: PRIMARY KEY ((pk1[, pk2...]), ck1[, ck2...])
        if (create.primary_key) {
            const auto& pk = *create.primary_key;

            auto find_col_def_idx = [&](String8 col_name) -> Optional<U64> {
                for (U64 i = 0; i < create.column_definitions.length; i++) {
                    if (create.column_definitions[i].name.identifier == col_name) {
                        return i;
                    }
                }
                return {};
            };

            // Partition key columns
            if (type_matches_tag<ColumnName>(pk.partition_key.column_or_columns)) {
                String8 pk_name = get<ColumnName>(pk.partition_key.column_or_columns).identifier;
                if (auto idx = find_col_def_idx(pk_name)) {
                    push_back(info.partition_col_def_indices, *idx);
                }
            } else {
                for (const auto& pk_col : get<DynamicArray<ColumnName>>(pk.partition_key.column_or_columns)) {
                    if (auto idx = find_col_def_idx(pk_col.identifier)) {
                        push_back(info.partition_col_def_indices, *idx);
                    }
                }
            }

            // Clustering key columns
            for (const auto& ck_col : pk.clustering_columns) {
                if (auto idx = find_col_def_idx(ck_col.identifier)) {
                    push_back(info.clustering_col_def_indices, *idx);
                }
            }
        }

        if (info.partition_col_def_indices.length == 0) {
            return {};
        }
        return info;
    }

    static bool type_is_counter(const type::Type& t) {
        return type_matches_tag<type::Basic>(t.value) && get<type::Basic>(t.value) == type::Basic::counter;
    }

    static bool type_contains_counter(const type::Type& t) {
        return visit(t.value, [](const auto& v) -> bool {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                return v == type::Basic::counter;
            } else if constexpr (SameAs<T, type::List>) {
                return type_contains_counter(v.element);
            } else if constexpr (SameAs<T, type::Set>) {
                return type_contains_counter(v.key);
            } else if constexpr (SameAs<T, type::Map>) {
                return type_contains_counter(v.key) || type_contains_counter(v.value);
            } else if constexpr (SameAs<T, type::Vector>) {
                return type_contains_counter(v.element);
            } else if constexpr (SameAs<T, type::Tuple>) {
                for (const auto& e : v.elements) {
                    if (type_contains_counter(e)) {
                        return true;
                    }
                }
                return false;
            } else {
                return false;
            }
        });
    }

    coroutine::Task<Result<Table*>> create_table(Schema& schema, Keyspace& ks, const CreateTable& create, S64 default_ttl_ms, TableExtraOptions extras) {
        assert_true(read_table_impl(schema, ks, create.name.table_name).error == Error::MissingTable, "table already exists");

        U64 inline_pk_count = 0;
        for (U64 i = 0; i < create.column_definitions.length; i++) {
            if (create.column_definitions[i].primary_key) {
                inline_pk_count++;
            }
        }
        if (inline_pk_count > 1) {
            co_return Result<Table*>{nullptr, Error::InvalidOptions, "Multiple PRIMARY KEYs specified"};
        }
        if (inline_pk_count == 1 && create.primary_key) {
            co_return Result<Table*>{nullptr, Error::InvalidOptions, "Multiple PRIMARY KEYs specified"};
        }

        auto pk_info_opt = get_primary_key_info(create);
        if (!pk_info_opt) {
            co_return Result<Table*>{nullptr, Error::MissingPrimaryKey, "table needs a primary key"};
        }
        PrimaryKeyInfo& pk_info = *pk_info_opt;

        // @note Cassandra prohibits counters in PKs, inside collections, and mixed with non-counter
        // regular columns. Rejected at CREATE TABLE so callers see a clear error.
        auto is_pk_def = [&](U64 def_idx) -> bool {
            for (U64 i : pk_info.partition_col_def_indices) {
                if (i == def_idx) {
                    return true;
                }
            }
            for (U64 i : pk_info.clustering_col_def_indices) {
                if (i == def_idx) {
                    return true;
                }
            }
            return false;
        };

        for (U64 i = 0; i < pk_info.partition_col_def_indices.length; i++) {
            const auto& col = create.column_definitions[pk_info.partition_col_def_indices[i]];
            if (type_is_counter(col.type)) {
                co_return Result<Table*>{nullptr, Error::InvalidOptions, "counter type is not supported for PRIMARY KEY"};
            }
        }
        for (U64 i = 0; i < pk_info.clustering_col_def_indices.length; i++) {
            const auto& col = create.column_definitions[pk_info.clustering_col_def_indices[i]];
            if (type_is_counter(col.type)) {
                co_return Result<Table*>{nullptr, Error::InvalidOptions, "counter type is not supported for PRIMARY KEY"};
            }
        }

        for (const auto& col : create.column_definitions) {
            if (!type_is_counter(col.type) && type_contains_counter(col.type)) {
                co_return Result<Table*>{nullptr, Error::InvalidOptions, "counter type is not allowed inside collections"};
            }
        }

        bool any_counter     = false;
        bool any_non_counter = false;
        for (U64 i = 0; i < create.column_definitions.length; i++) {
            if (is_pk_def(i)) {
                continue;
            }
            if (type_is_counter(create.column_definitions[i].type)) {
                any_counter = true;
            } else {
                any_non_counter = true;
            }
        }
        if (any_counter && any_non_counter) {
            co_return Result<Table*>{nullptr, Error::InvalidOptions, "Cannot mix counter and non counter columns in the same table"};
        }

        // Build the per-CK direction map from any CLUSTERING ORDER BY directive in WITH options.
        DynamicArray<Sort> ck_orders;
        resize(ck_orders, pk_info.clustering_col_def_indices.length);
        for (U64 i = 0; i < ck_orders.length; i++) {
            ck_orders[i] = Sort::ASC;
        }
        Error order_err = Error::None;
        for (const auto& opt : create.options.value) {
            if (order_err != Error::None) {
                break;
            }
            visit(opt, [&](const auto& o) {
                using O = RemoveCVRef<decltype(o)>;
                if constexpr (SameAs<O, CreateTable::ClusteringOrder>) {
                    // Directive columns must be a sequential prefix of the table's
                    // clustering columns in the same order; the last directive column may
                    // not extend past the actual CK list and must reference a real CK col.
                    for (U64 i = 0; i < o.column_orders.length; i++) {
                        if (i >= ck_orders.length) {
                            order_err = Error::InvalidOptions;
                            return;
                        }
                        U64     ck_def_idx = pk_info.clustering_col_def_indices[i];
                        String8 ck_name(create.column_definitions[ck_def_idx].name.identifier.c_str, create.column_definitions[ck_def_idx].name.identifier.length);
                        String8 directive_name(o.column_orders[i].column.identifier.c_str, o.column_orders[i].column.identifier.length);
                        if (ck_name != directive_name) {
                            order_err = Error::InvalidOptions;
                            return;
                        }
                        ck_orders[i] = o.column_orders[i].sort;
                    }
                }
            });
        }
        if (order_err != Error::None) {
            co_return Result<Table*>{nullptr, order_err, "CLUSTERING ORDER BY must list a sequential prefix of the table's clustering columns"};
        }

        U64 btree_page = co_await btree::create_paged(
            *schema.tables_blob.pager,
            btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(PartitionEntry)>{}
        );

        U64 offset_bytes = schema.tables_blob.size_bytes;

        TableStorage tbl_storage{
            .offset_in_blob_bytes = offset_bytes,
            .header               = TableHeader{
                                                .tombstone                   = false,
                                                .name_length                 = create.name.table_name.length,
                                                .keyspace_idx                = ks.idx,
                                                .btree_page                  = btree_page,
                                                .default_ttl_ms              = default_ttl_ms,
                                                .gc_grace_seconds            = extras.gc_grace_seconds,
                                                .min_index_interval          = extras.min_index_interval,
                                                .max_index_interval          = extras.max_index_interval,
                                                .memtable_flush_period_in_ms = extras.memtable_flush_period_in_ms,
                                                },
            .name    = AutoString8(create.name.table_name),
            .columns = {},
        };
        TableStorage& tbl_storage_ref = push_back(schema.storage.tables, move(tbl_storage));

        co_await blob::resize(
            schema.tables_blob, offset_bytes + sizeof(tbl_storage_ref.header) + tbl_storage_ref.name.length
        );

        co_await blob::tupdate(schema.tables_blob, &tbl_storage_ref.header, &offset_bytes);
        co_await update_str(schema.tables_blob, tbl_storage_ref.name, &offset_bytes);

        Table tbl{
            .idx                        = schema.storage.tables.length - 1,
            .tombstone                  = tbl_storage_ref.header.tombstone,
            .name                       = tbl_storage_ref.name,
            .cols                       = {},
            .partition_key_col_indices  = {},
            .clustering_key_col_indices = {},
            .static_col_indices         = {},
            .indexes                    = {},
            .btree                      = PartitionBTree{
                                           schema.tables_blob.pager, tbl_storage_ref.header.btree_page,
                                           btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(PartitionEntry)>{}
            },
            .default_ttl_ms              = tbl_storage_ref.header.default_ttl_ms,
            .gc_grace_seconds            = tbl_storage_ref.header.gc_grace_seconds,
            .min_index_interval          = tbl_storage_ref.header.min_index_interval,
            .max_index_interval          = tbl_storage_ref.header.max_index_interval,
            .memtable_flush_period_in_ms = tbl_storage_ref.header.memtable_flush_period_in_ms,
        };
        // @todo avoid this copy
        for (U64 i = 0; i < pk_info.partition_col_def_indices.length; i++) {
            push_back(tbl.partition_key_col_indices, pk_info.partition_col_def_indices[i]);
        }
        for (U64 i = 0; i < pk_info.clustering_col_def_indices.length; i++) {
            push_back(tbl.clustering_key_col_indices, pk_info.clustering_col_def_indices[i]);
        }

        Table& tbl_ref = push_back(ks.tbls, move(tbl));

        for (U64 col_def_idx = 0; col_def_idx < create.column_definitions.length; col_def_idx++) {
            auto& col_def = create.column_definitions[col_def_idx];

            if (read_column_impl(schema, tbl_ref, col_def.name.identifier).error != Error::MissingColumn) {
                // @todo hard delete
                co_await delete_table(schema, ks, create.name.table_name);
                co_return Result<Table*>{nullptr, Error::ColumnNameCollision};
            }

            // Determine key_kind and key_position for this column
            KeyKind kk    = KeyKind::None;
            U16     kp    = 0;
            Sort    order = Sort::ASC;
            for (U64 i = 0; i < pk_info.partition_col_def_indices.length; i++) {
                if (pk_info.partition_col_def_indices[i] == col_def_idx) {
                    kk = KeyKind::PartitionKey;
                    kp = static_cast<U16>(i);
                    break;
                }
            }
            if (kk == KeyKind::None) {
                for (U64 i = 0; i < pk_info.clustering_col_def_indices.length; i++) {
                    if (pk_info.clustering_col_def_indices[i] == col_def_idx) {
                        kk    = KeyKind::ClusteringKey;
                        kp    = static_cast<U16>(i);
                        order = ck_orders[i];
                        break;
                    }
                }
            }

            if (auto res = co_await create_column(schema, tbl_ref, col_def, kk, kp, order); res.error != Error::None) {
                // @todo hard delete
                co_await delete_table(schema, ks, create.name.table_name);
                co_return Result<Table*>{nullptr, res.error, res.message};
            }
        }

        co_return Result<Table*>{&tbl_ref};
    }

    coroutine::Task<Result<void>> set_default_ttl_ms(Schema& schema, Table& tbl, S64 default_ttl_ms) {
        TableStorage& tbl_storage         = schema.storage.tables[tbl.idx];
        tbl_storage.header.default_ttl_ms = default_ttl_ms;
        tbl.default_ttl_ms                = default_ttl_ms;
        U64 offset                        = tbl_storage.offset_in_blob_bytes + offsetof(TableHeader, default_ttl_ms);
        co_await blob::tupdate(schema.tables_blob, &tbl_storage.header.default_ttl_ms, &offset);
        co_return Result<void>{};
    }

    coroutine::Task<Result<void>> set_table_extra_options(Schema& schema, Table& tbl, TableExtraOptions extras) {
        TableStorage& tbl_storage                      = schema.storage.tables[tbl.idx];
        tbl_storage.header.gc_grace_seconds            = extras.gc_grace_seconds;
        tbl_storage.header.min_index_interval          = extras.min_index_interval;
        tbl_storage.header.max_index_interval          = extras.max_index_interval;
        tbl_storage.header.memtable_flush_period_in_ms = extras.memtable_flush_period_in_ms;
        tbl.gc_grace_seconds                           = extras.gc_grace_seconds;
        tbl.min_index_interval                         = extras.min_index_interval;
        tbl.max_index_interval                         = extras.max_index_interval;
        tbl.memtable_flush_period_in_ms                = extras.memtable_flush_period_in_ms;
        // @note these four S32 fields are contiguous in TableHeader; persisted in one write.
        U64 offset = tbl_storage.offset_in_blob_bytes + offsetof(TableHeader, gc_grace_seconds);
        U64 size   = offsetof(TableHeader, memtable_flush_period_in_ms) + sizeof(S32) - offsetof(TableHeader, gc_grace_seconds);
        co_await blob::update(schema.tables_blob, reinterpret_cast<const U8*>(&tbl_storage.header.gc_grace_seconds), size, offset);
        co_return Result<void>{};
    }

    coroutine::Task<Result<void>> delete_table(Schema& schema, Keyspace& ks, String8 name) {
        auto tbl_res = read_table_impl(schema, ks, name);
        if (tbl_res.error == Error::None) {
            const auto& tbl = tbl_res.value;
            tbl->tombstone  = true;

            TableStorage& tbl_storage    = schema.storage.tables[tbl->idx];
            tbl_storage.header.tombstone = true;

            U64 offset = tbl_storage.offset_in_blob_bytes + offsetof(TableHeader, tombstone);
            co_await blob::tupdate(schema.tables_blob, &tbl_storage.header.tombstone, &offset);
            co_return Result<void>{};
        }
        co_return Result<void>{tbl_res.error, tbl_res.message};
    }

    coroutine::Task<Result<Column*>> create_column(Schema& schema, Table& tbl, const ColumnDefinition& create, KeyKind key_kind, U16 key_position, Sort clustering_order) {
        if (create._static) {
            if (tbl.clustering_key_col_indices.length == 0) {
                co_return Result<Column*>{nullptr, Error::InvalidOptions, "static columns require at least one clustering column"};
            }
            if (key_kind != KeyKind::None) {
                co_return Result<Column*>{nullptr, Error::InvalidOptions, "primary key columns cannot be static"};
            }
        }
        assert_true_not_implemented(!create.mask, "column MASKED WITH is not implemented");
        assert_true(read_column_impl(schema, tbl, create.name.identifier).error == Error::MissingColumn, "column already exists");

        U64 prev_type_count = schema.storage.type_entries.length;
        U32 type_id         = register_type(schema, create.type);

        // Persist any newly registered type entries.
        if (schema.storage.type_entries.length > prev_type_count) {
            U64 new_types_offset = prev_type_count * sizeof(TypeRegistryEntry);
            co_await blob::resize(schema.types_blob, schema.storage.type_entries.length * sizeof(TypeRegistryEntry));
            for (U64 i = prev_type_count; i < schema.storage.type_entries.length; i++) {
                co_await blob::update(schema.types_blob, reinterpret_cast<const U8*>(&schema.storage.type_entries[i]), sizeof(TypeRegistryEntry), new_types_offset);
                new_types_offset += sizeof(TypeRegistryEntry);
            }
        }

        U64 offset_bytes = schema.columns_blob.size_bytes;

        ColumnStorage col_storage{
            .offset_in_blob_bytes = offset_bytes,
            .header               = ColumnHeader{
                                                 .tombstone        = false,
                                                 .is_static        = create._static,
                                                 .name_length      = create.name.identifier.length,
                                                 .type_id          = type_id,
                                                 .table_idx        = tbl.idx,
                                                 .key_kind         = key_kind,
                                                 .key_position     = key_position,
                                                 .clustering_order = clustering_order,
                                                 },
            .name = AutoString8(create.name.identifier),
        };
        ColumnStorage& col_storage_ref = push_back(schema.storage.columns, move(col_storage));

        co_await blob::resize(
            schema.columns_blob, offset_bytes + sizeof(col_storage_ref.header) + col_storage_ref.name.length
        );

        co_await blob::tupdate(schema.columns_blob, &col_storage_ref.header, &offset_bytes);
        co_await update_str(schema.columns_blob, col_storage_ref.name, &offset_bytes);

        U64    new_col_idx = tbl.cols.length;
        Column col{
            .tombstone        = col_storage_ref.header.tombstone,
            .is_static        = col_storage_ref.header.is_static,
            .name             = col_storage_ref.name,
            .type             = unpack_type(schema, col_storage_ref.header.type_id),
            .key_kind         = key_kind,
            .key_position     = key_position,
            .clustering_order = clustering_order,
        };
        Column* col_ptr = &push_back(tbl.cols, move(col));
        if (create._static) {
            push_back(tbl.static_col_indices, new_col_idx);
        }
        co_return Result<Column*>{col_ptr};
    }

    coroutine::Task<Result<void>> delete_column(Schema& schema, Table& tbl, String8 name) {
        auto col_res = read_column_impl(schema, tbl, name);
        if (col_res.error == Error::None) {
            const auto& col = col_res.value;
            col->tombstone  = true;

            for (auto& col_storage : schema.storage.columns) {
                if (!col_storage.header.tombstone && col_storage.header.table_idx == tbl.idx && String8(col_storage.name.c_str, col_storage.name.length) == name) {
                    col_storage.header.tombstone = true;
                    U64 offset                   = col_storage.offset_in_blob_bytes + offsetof(ColumnHeader, tombstone);
                    co_await blob::tupdate(schema.columns_blob, &col_storage.header.tombstone, &offset);
                    break;
                }
            }
            co_return Result<void>{};
        }
        co_return Result<void>{col_res.error, col_res.message};
    }

    Result<Index*> read_index(Schema& /*schema*/, Table& tbl, String8 name) {
        for (auto& idx : tbl.indexes) {
            if (idx.name == name && !idx.tombstone) {
                return {&idx};
            }
        }
        return {nullptr, Error::MissingIndex};
    }

    coroutine::Task<Result<Index*>> create_index(Schema& schema, Table& tbl, U64 col_idx, String8 index_name, IndexKind kind) {
        U64 btree_page = co_await btree::create_paged(
            *schema.indexes_blob.pager,
            btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<1>{}
        );

        U64 offset_bytes = schema.indexes_blob.size_bytes;

        IndexStorage idx_storage{
            .offset_in_blob_bytes = offset_bytes,
            .header               = IndexHeader{
                                                .tombstone   = false,
                                                .name_length = index_name.length,
                                                .table_idx   = tbl.idx,
                                                .col_idx     = col_idx,
                                                .btree_page  = btree_page,
                                                .kind        = kind,
                                                },
            .name = AutoString8(index_name),
        };
        IndexStorage& idx_storage_ref = push_back(schema.storage.indexes, move(idx_storage));

        co_await blob::resize(
            schema.indexes_blob,
            offset_bytes + sizeof(idx_storage_ref.header) + idx_storage_ref.name.length
        );

        co_await blob::tupdate(schema.indexes_blob, &idx_storage_ref.header, &offset_bytes);
        co_await update_str(schema.indexes_blob, idx_storage_ref.name, &offset_bytes);

        U64   new_idx_storage_i = schema.storage.indexes.length - 1;
        Index idx{
            .idx       = new_idx_storage_i,
            .tombstone = false,
            .name      = idx_storage_ref.name,
            .col_idx   = col_idx,
            .kind      = kind,
            .btree     = IndexBTree{
                                    schema.indexes_blob.pager, btree_page,
                                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<1>{}
            },
        };
        co_return Result<Index*>{&push_back(tbl.indexes, move(idx))};
    }

    coroutine::Task<Result<void>> drop_index(Schema& schema, Table& tbl, String8 name) {
        auto idx_res = read_index(schema, tbl, name);
        if (idx_res.error == Error::None) {
            Index* idx     = idx_res.value;
            idx->tombstone = true;

            IndexStorage& idx_storage    = schema.storage.indexes[idx->idx];
            idx_storage.header.tombstone = true;

            U64 offset = idx_storage.offset_in_blob_bytes + offsetof(IndexHeader, tombstone);
            co_await blob::tupdate(schema.indexes_blob, &idx_storage.header.tombstone, &offset);
            co_return Result<void>{};
        }
        co_return Result<void>{idx_res.error, idx_res.message};
    }
}
