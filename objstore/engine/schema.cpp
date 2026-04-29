module;
#include "macros.h"

module objstore.engine.schema;

import plexdb.threads;
import plexdb.tagged_union;
import plexdb.os.dynamic_tagged_union;

using namespace plexdb;

namespace objstore::schema {
    // @note str length must be set to decide read length
    void get_str(blob::BlobDynamicPaged& blob, const AutoString8& str, U64* offset) {
        blob::get(blob, reinterpret_cast<U8*>(str.c_str), str.length, *offset);
        *offset += str.length;
    };

    void update_str(blob::BlobDynamicPaged& blob, String8 str, U64* offset) {
        blob::update(blob, reinterpret_cast<const U8*>(str.data), str.length, *offset);
        *offset += str.length;
    };

    // @todo small schema storage optimization
    Schema::Schema(Pager* in_pager, U64 page) {
        this->schema_blob = blob::BlobStaticPaged(in_pager, page, sizeof(SchemaHeader));

        SchemaHeader schema_header{};
        blob::tget(this->schema_blob, &schema_header);

        this->keyspaces_blob = blob::BlobDynamicPaged(in_pager, schema_header.keyspaces_page);
        this->tables_blob = blob::BlobDynamicPaged(in_pager, schema_header.tables_page);
        this->columns_blob = blob::BlobDynamicPaged(in_pager, schema_header.columns_page);

        //
        // allocate storage and cache blob contents in memory
        //
        for (U64 keyspace_offset_bytes = 0; keyspace_offset_bytes < keyspaces_blob.size_bytes;) {
            KeyspaceStorage ks_storage{
                .offset_in_blob_bytes = keyspace_offset_bytes,
                .header = {},
                .name = {},
                .tables = {},
                .replication_class = ReplicationClass::Unknown,
                .replication_factor = 0,
                .do_durable_writes = false,
            };

            blob::tget(this->keyspaces_blob, &ks_storage.header, &keyspace_offset_bytes);

            ks_storage.name = AutoString8{ks_storage.header.name_length};
            get_str(this->keyspaces_blob, ks_storage.name, &keyspace_offset_bytes);

            push_back(this->storage.keyspaces, move(ks_storage));
        }

        for (U64 table_offset_bytes = 0; table_offset_bytes < tables_blob.size_bytes;) {
            TableStorage tbl_storage{
                .offset_in_blob_bytes = table_offset_bytes,
                .header = {},
                .name = {},
                .columns = {},
            };

            blob::tget(this->tables_blob, &tbl_storage.header, &table_offset_bytes);

            tbl_storage.name = AutoString8{tbl_storage.header.name_length};
            get_str(this->tables_blob, tbl_storage.name, &table_offset_bytes);

            assert_true(tbl_storage.header.keyspace_idx < this->storage.keyspaces.length, "invalid keyspace idx");
            KeyspaceStorage& ks_storage = this->storage.keyspaces[tbl_storage.header.keyspace_idx];
            push_back(ks_storage.tables, this->storage.tables.length);

            push_back(this->storage.tables, move(tbl_storage));
        }

        for (U64 column_offset_bytes = 0; column_offset_bytes < columns_blob.size_bytes;) {
            ColumnStorage col_storage{
                .offset_in_blob_bytes = column_offset_bytes,
                .header = {},
                .name = {},
            };

            blob::tget(this->columns_blob, &col_storage.header, &column_offset_bytes);

            col_storage.name = AutoString8{col_storage.header.name_length};
            get_str(this->columns_blob, col_storage.name, &column_offset_bytes);

            assert_true(col_storage.header.table_idx < this->storage.tables.length, "invalid table idx");
            TableStorage& tbl_storage = this->storage.tables[col_storage.header.table_idx];
            push_back(tbl_storage.columns, this->storage.columns.length);

            push_back(this->storage.columns, move(col_storage));
        }


        //
        // resolves indices and contruct views
        //
        reserve(this->keyspaces, this->storage.keyspaces.length);
        for (U64 ks_idx = 0; ks_idx < this->storage.keyspaces.length; ks_idx++) {
            const KeyspaceStorage& ks_storage = this->storage.keyspaces[ks_idx];
            Keyspace ks {
                .idx = ks_idx,
                .tombstone = ks_storage.header.tombstone,
                .name = ks_storage.name,
                .tbls = {},
            };
            for (const auto& tbl_idx: ks_storage.tables) {
                const TableStorage& tbl_storage = this->storage.tables[tbl_idx];

                Table tbl {
                    .idx = tbl_idx,
                    .tombstone = tbl_storage.header.tombstone,
                    .name = tbl_storage.name,
                    .cols = {},
                    .primary_col_idx = 0,
                    .btree = btree::BTreePaged(in_pager, tbl_storage.header.btree_page),
                };

                reserve(tbl.cols, tbl_storage.columns.length);
                for (const auto& col_idx: tbl_storage.columns) {
                    const ColumnStorage& col_storage = this->storage.columns[col_idx];

                    Column col {
                        .tombstone = col_storage.header.tombstone,
                        .name = col_storage.name,
                        .type = col_storage.header.type,
                    };

                    push_back(tbl.cols, move(col));
                }
                push_back(ks.tbls, move(tbl));
            }
            push_back(this->keyspaces, move(ks));
        }
    }

    U64 create_schema(Pager& pager) {
        U64 schema_page = blob::create_paged_static(pager, sizeof(SchemaHeader));

        U64 keyspaces_page = blob::create_paged_dynamic(pager, 0);
        U64 tables_page    = blob::create_paged_dynamic(pager, 0);
        U64 columns_page   = blob::create_paged_dynamic(pager, 0);

        SchemaHeader header{
            .keyspaces_page = keyspaces_page,
            .tables_page = tables_page,
            .columns_page = columns_page,
        };

        blob::BlobStaticPaged schema_blob(&pager, schema_page, sizeof(SchemaHeader));
        blob::tupdate(schema_blob, &header);

        return schema_page;
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

    [[nodiscard("option may be invalid")]]
    static Result<void> apply_keyspace_replication_option(KeyspaceStorage& storage, const OptionValue& option) {
        if (!type_matches_tag<MapLiteral>(option)) {
            return {Error::InvalidOptions, "replication option is not a map literal"};
        }

        const auto& map = get<MapLiteral>(option);
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
                if (!type_matches_tag<Constant>(value_term.value)) {
                    return {Error::InvalidOptions, "replication factor should be an integer"};
                }
                const auto& value_constant = get<Constant>(value_term.value);
                if (!type_matches_tag<S64>(value_constant.value)) {
                    return {Error::InvalidOptions, "replication factor should be an integer"};
                }
                const auto& value = get<S64>(value_constant.value);

                if (value <= 0) {
                    return {Error::InvalidOptions, "replication factor must be strictly positive"};
                }

                storage.replication_factor = value;
            } else {
                // @note per-datacenter replication factors (e.g. 'datacenter1': '3') are accepted but ignored
                // @todo implement per-datacenter replication factor storage
            }
        }

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
        storage.replication_class = ReplicationClass::SimpleStrategy;
        storage.replication_factor = 1;
        storage.do_durable_writes = true;

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

        // @todo should we error if not replication class is provided or continue being more permissive?
        return {Error::None};
    }

    Result<Keyspace*> create_keyspace(Schema& schema, const CreateKeyspace& create) {
        assert_true(read_keyspace_impl(schema, create.name).error == Error::MissingKeyspace, "keyspace already exists");

        U64 offset_bytes = schema.keyspaces_blob.size_bytes;

        KeyspaceStorage ks_storage {
            .offset_in_blob_bytes = offset_bytes,
            .header = KeyspaceHeader{
                .tombstone = false,
                .name_length = create.name.length,
            },
            .name = AutoString8(create.name),
            .tables = {},
            .replication_class = ReplicationClass::SimpleStrategy,
            .replication_factor = 1,
            .do_durable_writes = true,
        };
        if (auto res = apply_keyspace_options(ks_storage, create.options); res.error != Error::None) {
            return {nullptr, res.error, res.message};
        }

        KeyspaceStorage& ks_storage_ref = push_back(schema.storage.keyspaces, move(ks_storage));

        blob::resize(
            schema.keyspaces_blob, offset_bytes +
            // fixed
            sizeof(ks_storage_ref.header) +
            // variable length
            ks_storage_ref.name.length
        );

        blob::tupdate(schema.keyspaces_blob, &ks_storage_ref.header, &offset_bytes);
        update_str(schema.keyspaces_blob, ks_storage_ref.name, &offset_bytes);

        Keyspace ks {
            .idx = schema.storage.keyspaces.length-1,
            .tombstone = ks_storage_ref.header.tombstone,
            .name = ks_storage_ref.name,
            .tbls = {},
        };
        return {&push_back(schema.keyspaces, move(ks))};
    }
    Result<Keyspace*> read_keyspace(Schema& schema, String8 name) {
        return read_keyspace_impl(schema, name);
    }
    Result<void> delete_keyspace(Schema& schema, String8 name) {
        auto ks_res = read_keyspace_impl(schema, name);
        if (ks_res.error == Error::None) {
            const auto& ks = ks_res.value;
            ks->tombstone = true;

            KeyspaceStorage& ks_storage = schema.storage.keyspaces[ks->idx];
            ks_storage.header.tombstone = true;

            U64 offset = ks_storage.offset_in_blob_bytes + offsetof(KeyspaceHeader, tombstone);
            tupdate(schema.keyspaces_blob, &ks_storage.header.tombstone, &offset);
            return {};
        }
        return {ks_res.error, ks_res.message};
    }

    static Optional<U64> get_primary_key_col_idx(const CreateTable& create) {
        bool has_primary_key = false;
        U64 primary_col_idx = 0;
        for (U64 col_idx = 0; col_idx < create.column_definitions.length; col_idx++) {
            const auto& col_def = create.column_definitions[col_idx];

            // @todo error code/message
            if (has_primary_key && col_def.primary_key) {
                return {};
            }

            if (col_def.primary_key) {
                primary_col_idx = col_idx;
                has_primary_key = true;
            }
        }
        if (has_primary_key) {
            return primary_col_idx;
        }

        if (create.primary_key) {
            auto& pk = *create.primary_key;
            String8 pk_col_name;
            if (type_matches_tag<ColumnName>(pk.partition_key.column_or_columns)) {
                pk_col_name = get<ColumnName>(pk.partition_key.column_or_columns).identifier;
            } else {
                auto& cols = get<DynamicArray<ColumnName>>(pk.partition_key.column_or_columns);
                assert_true_not_implemented(cols.length == 1, "composite partition key in standalone PRIMARY KEY");
                if (cols.length > 0) pk_col_name = cols[0].identifier;
            }

            for (U64 col_idx = 0; col_idx < create.column_definitions.length; col_idx++) {
                if (create.column_definitions[col_idx].name.identifier == pk_col_name) {
                    return col_idx;
                }
            }
        }

        return {};
    }

    Result<Table*> create_table(Schema& schema, Keyspace& ks, const CreateTable& create) {
        assert_true_not_implemented(create.options.value.length == 0, "CREATE TABLE WITH options are not implemented");
        assert_true(read_table_impl(schema, ks, create.name.table_name).error == Error::MissingTable, "table already exists");

        auto primary_key_col_idx_opt = get_primary_key_col_idx(create);
        if (!primary_key_col_idx_opt) // @todo error code/message
            return {nullptr, Error::MissingPrimaryKey, "table needs a primary key"};
        U64 primary_key_col_idx = *primary_key_col_idx_opt;

        U64 btree_page = btree::create_paged(*schema.tables_blob.pager, sizeof(U64));

        U64 offset_bytes = schema.tables_blob.size_bytes;

        TableStorage tbl_storage {
            .offset_in_blob_bytes = offset_bytes,
            .header = TableHeader{
                .tombstone = false,
                .name_length = create.name.table_name.length,
                .keyspace_idx = ks.idx,
                .btree_page = btree_page,
            },
            .name = AutoString8(create.name.table_name),
            .columns = {},
        };
        TableStorage& tbl_storage_ref = push_back(schema.storage.tables, move(tbl_storage));

        blob::resize(
            schema.tables_blob, offset_bytes +
            // fixed
            sizeof(tbl_storage_ref.header) +
            // variable length
            tbl_storage_ref.name.length
        );

        blob::tupdate(schema.tables_blob, &tbl_storage_ref.header, &offset_bytes);
        update_str(schema.tables_blob, tbl_storage_ref.name, &offset_bytes);

        Table tbl {
            .idx = schema.storage.tables.length-1,
            .tombstone = tbl_storage_ref.header.tombstone,
            .name = tbl_storage_ref.name,
            .cols = {},
            .primary_col_idx = primary_key_col_idx,
            .btree = btree::BTreePaged(schema.tables_blob.pager, tbl_storage_ref.header.btree_page),
        };

        Table& tbl_ref = push_back(ks.tbls, move(tbl));

        for (U64 col_def_idx = 0; col_def_idx < create.column_definitions.length; col_def_idx++) {
            auto& col_def = create.column_definitions[col_def_idx];

            if (read_column_impl(schema, tbl, col_def.name.identifier).error != Error::MissingColumn) {
                // @todo hard delete
                delete_table(schema, ks, create.name.table_name);
                return {nullptr, Error::ColumnNameCollision};
            }

            if (auto res = create_column(schema, tbl_ref, col_def); res.error != Error::None)  {
                // @todo hard delete
                delete_table(schema, ks, create.name.table_name);
                return {nullptr, res.error, res.message};
            }
        }

        return {&tbl_ref};
    }
    Result<Table*> read_table(Schema& schema, Keyspace& ks, String8 name) {
        return read_table_impl(schema, ks, name);
    }
    Result<void> delete_table(Schema& schema, Keyspace& ks, String8 name) {
        auto tbl_res = read_table_impl(schema, ks, name);
        if (tbl_res.error != Error::None) {
            const auto& tbl = tbl_res.value;
            tbl->tombstone = true;

            TableStorage& tbl_storage = schema.storage.tables[tbl->idx];
            tbl_storage.header.tombstone = true;

            U64 offset = tbl_storage.offset_in_blob_bytes + offsetof(TableHeader, tombstone);
            tupdate(schema.tables_blob, &tbl_storage.header.tombstone, &offset);
            return {};
        }
        return {tbl_res.error, tbl_res.message};
    }

    Result<Column*> create_column(Schema& schema, Table& tbl, const ColumnDefinition& create) {
        // @todo implement proper static column semantics (shared across clustering rows within a partition)
        assert_true_not_implemented(!create._static, "static column storage is not implemented");
        assert_true_not_implemented(!create.mask, "column MASKED WITH is not implemented");
        assert_true(read_column_impl(schema, tbl, create.name.identifier).error == Error::MissingColumn, "column already exists");

        U64 offset_bytes = schema.columns_blob.size_bytes;

        ColumnStorage col_storage {
            .offset_in_blob_bytes = offset_bytes,
            .header = ColumnHeader{
                .tombstone = false,
                .name_length = create.name.identifier.length,
                .type = create.type,
                .table_idx = tbl.idx,
            },
            .name = AutoString8(create.name.identifier),
        };
        ColumnStorage& col_storage_ref = push_back(schema.storage.columns, move(col_storage));

        blob::resize(
            schema.columns_blob, offset_bytes +
            // fixed
            sizeof(col_storage_ref.header) +
            // variable length
            col_storage_ref.name.length
        );

        blob::tupdate(schema.columns_blob, &col_storage_ref.header, &offset_bytes);
        update_str(schema.columns_blob, col_storage_ref.name, &offset_bytes);

        Column col {
            .tombstone = col_storage_ref.header.tombstone,
            .name = col_storage_ref.name,
            .type = col_storage_ref.header.type,
        };
        return {&push_back(tbl.cols, move(col))};
    }
    Result<Column*> read_column(Schema& schema, Table& tbl, String8 name) {
        return read_column_impl(schema, tbl, name);
    }
    Result<void> delete_column(Schema& schema, Table& tbl, String8 name) {
        auto col_res = read_column_impl(schema, tbl, name);
        if (col_res.error != Error::None) {
            const auto& col = col_res.value;
            col->tombstone = true;

            for (auto& col_storage : schema.storage.columns) {
                if (!col_storage.header.tombstone &&
                    col_storage.header.table_idx == tbl.idx &&
                    String8(col_storage.name.c_str, col_storage.name.length) == name) {
                    col_storage.header.tombstone = true;
                    U64 offset = col_storage.offset_in_blob_bytes + offsetof(ColumnHeader, tombstone);
                    tupdate(schema.columns_blob, &col_storage.header.tombstone, &offset);
                    break;
                }
            }
            return {};
        }
        return {col_res.error, col_res.message};
    }
}
