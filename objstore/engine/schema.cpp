module;
#include "cstddef"

module objstore.engine.schema;

import plexdb.threads;

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
            };

            blob::tget(this->keyspaces_blob, &ks_storage.header, &keyspace_offset_bytes);

            ks_storage.name = AutoString8{ks_storage.header.name_length};
            get_str(this->keyspaces_blob, ks_storage.name, &keyspace_offset_bytes);

            push_back(this->storage.keyspaces, move(ks_storage));
        }

        for (U64 table_offset_bytes = 0; table_offset_bytes < tables_blob.size_bytes;) {
            TableStorage tbl_storage{
                .offset_in_blob_bytes = table_offset_bytes,
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
            };

            reserve(ks.tbls, ks.tbls.length);
            for (const auto& tbl_idx: ks_storage.tables) {
                const TableStorage& tbl_storage = this->storage.tables[tbl_idx];

                Table tbl {
                    .idx = tbl_idx,
                    .tombstone = tbl_storage.header.tombstone,
                    .name = tbl_storage.name,
                    .btree = btree::BTreePaged(in_pager, tbl_storage.header.btree_page),
                };

                reserve(tbl.cols, tbl_storage.columns.length);
                for (const auto& col_idx: tbl_storage.columns) {
                    const ColumnStorage& col_storage = this->storage.columns[col_idx];

                    Column col {
                        .tombstone = col_storage.header.tombstone,
                        .name = col_storage.name,
                        .dtype = col_storage.header.dtype,
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

    Keyspace* read_keyspace_impl(Schema& schema, String8 name) {
        for (auto& ks : schema.keyspaces) {
            if (ks.name == name) {
                return &ks;
            }
        }
        return nullptr;
    }
    Table* read_table_impl(Schema& schema, Keyspace& ks, String8 name) {
        for (auto& tbl : ks.tbls) {
            if (tbl.name == name) {
                return &tbl;
            }
        }
        return nullptr;
    }
    Column* read_column_impl(Schema& schema, Table& tbl, String8 name) {
        for (auto& col : tbl.cols) {
            if (col.name == name) {
                return &col;
            }
        }
        return nullptr;
    }

    Keyspace* create_keyspace(Schema& schema, const CreateKeyspace& create) {
        if (Keyspace* ks = read_keyspace_impl(schema, create.keyspace_name); ks != nullptr) {
            if (create.if_not_exists) {
                return ks;
            } else {
                return nullptr;
            }
        }

        U64 offset_bytes = schema.keyspaces_blob.size_bytes;

        KeyspaceStorage ks_storage {
            .offset_in_blob_bytes = offset_bytes,
            .header = KeyspaceHeader{
                .tombstone = false,
                .name_length = create.keyspace_name.length,
            },
            .name = AutoString8(create.keyspace_name),
        };
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
        };
        return &push_back(schema.keyspaces, move(ks));
    }
    Keyspace* read_keyspace(Schema& schema, String8 name) {
        return read_keyspace_impl(schema, name);
    }
    bool delete_keyspace(Schema& schema, String8 name) {
        if (Keyspace* ks = read_keyspace_impl(schema, name); ks != nullptr) {
            ks->tombstone = true;

            KeyspaceStorage& ks_storage = schema.storage.keyspaces[ks->idx];
            ks_storage.header.tombstone = true;

            U64 offset = ks_storage.offset_in_blob_bytes + offsetof(KeyspaceHeader, tombstone);
            tupdate(schema.keyspaces_blob, &ks_storage.header.tombstone, &offset);
            return true;
        }
        return false;
    }

    Table* create_table(Schema& schema, Keyspace& ks, const CreateTable& create) {
        if (Table* tbl = read_table_impl(schema, ks, create.table_name); tbl != nullptr) {
            if (create.if_not_exists) {
                return tbl;
            } else {
                return nullptr;
            }
        }

        // check column schema contains one primary key
        bool has_primary_key = false;
        U64 primary_col_idx = 0;
        for (U64 col_idx = 0; col_idx < create.columns.length; col_idx++) {
            const auto& create_col = create.columns[col_idx];

            // @todo error code/message
            if (has_primary_key && create_col.is_primary_key) {
                return nullptr;
            }

            if (create_col.is_primary_key) {
                primary_col_idx = col_idx;
                has_primary_key = true;
            }
        }
        if (!has_primary_key) {
            return nullptr;
        }
        
        U64 btree_page = btree::create_paged(*schema.tables_blob.pager, sizeof(U64));
        
        U64 offset_bytes = schema.tables_blob.size_bytes;

        TableStorage tbl_storage {
            .offset_in_blob_bytes = offset_bytes,
            .header = TableHeader{
                .tombstone = false,
                .name_length = create.table_name.length,
                .keyspace_idx = ks.idx,
                .btree_page = btree_page,
            },
            .name = AutoString8(create.table_name),
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
            .primary_col_idx = primary_col_idx,
            .btree = btree::BTreePaged(schema.tables_blob.pager, tbl_storage_ref.header.btree_page),
        };
        
        Table& tbl_ref = push_back(ks.tbls, move(tbl));

        for (auto& create_col : create.columns) {
            Column* col_ref = create_column(schema, tbl_ref, create_col);

            if (col_ref == nullptr) {
                assert_not_implemented("handle failure to create column during table creation");
            }
        }

        return &tbl_ref;
    }
    Table* read_table(Schema& schema, Keyspace& ks, String8 name) {
        return read_table_impl(schema, ks, name);
    }
    bool delete_table(Schema& schema, Keyspace& ks, String8 name) {
        if (Table* tbl = read_table_impl(schema, ks, name); tbl != nullptr) {
            tbl->tombstone = true;

            TableStorage& tbl_storage = schema.storage.tables[tbl->idx];
            tbl_storage.header.tombstone = true;

            U64 offset = tbl_storage.offset_in_blob_bytes + offsetof(TableHeader, tombstone);
            tupdate(schema.tables_blob, &tbl_storage.header.tombstone, &offset);
            return true;
        }
        return false;
    }

    Column* create_column(Schema& schema, Table& tbl, const CreateColumn& create) {
        if (Column* col = read_column_impl(schema, tbl, create.name); col != nullptr) {
            return nullptr;
        }

        U64 offset_bytes = schema.columns_blob.size_bytes;

        ColumnStorage col_storage {
            .offset_in_blob_bytes = offset_bytes,
            .header = ColumnHeader{
                .tombstone = false,
                .name_length = create.name.length,
                .dtype = create.dtype,
                .table_idx = tbl.idx,
            },
            .name = AutoString8(create.name),
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
            .dtype = col_storage_ref.header.dtype,
        };
        return &push_back(tbl.cols, move(col));
    }
    Column* read_column(Schema& schema, Table& tbl, String8 name) {
        return read_column_impl(schema, tbl, name);
    }
    bool delete_column(Schema& schema, Table& tbl, String8 name) {
        if (Column* col = read_column_impl(schema, tbl, name); col != nullptr) {
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
            return true;
        }
        return false;
    }
}