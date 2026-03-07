module objstore.engine;

import plexdb.os;

namespace objstore::engine {
    Engine::Engine(Pager* in_pager) : schema(in_pager, in_pager->header.root_page), pager(in_pager) {}

    void create_database(Pager& pager) {
        U64 schema_page = schema::create_schema(pager);
        pager::set_root(pager, schema_page);
    }

    // ========================================================================
    // column iterator
    // ========================================================================
    ColumnIterator columns_begin(Pager* pager, const schema::Table* table, U64 row_page) {
        return ColumnIterator{
            .pager = pager,
            .table = table,
            .row_page = row_page,
            .col_idx = 0,
            .row_offset_bytes = 0,
        };
    }

    ColumnIterator columns_end(const schema::Table* table) {
        return ColumnIterator{
            .pager = nullptr,
            .table = table,
            .row_page = 0,
            .col_idx = table->cols.length,
            .row_offset_bytes = 0,
        };
    }

    const schema::Column& column(const ColumnIterator& it) {
        return it.table->cols[it.col_idx];
    }

    U64 column_count(const ColumnIterator& it) {
        return it.table->cols.length;
    }

    dtype::ReadValue read_value(ColumnIterator& it) {
        blob::BlobDynamicPaged row_blob(it.pager, it.row_page);
        U64 offset = it.row_offset_bytes;
        auto read = [&offset, &row_blob](U8* out_value, U64 size) {
            blob::get(row_blob, out_value, size, offset);
            offset += size;
        };
        return dtype::read(read, it.table->cols[it.col_idx].dtype);
    }

    ColumnIterator& ColumnIterator::operator++() {
        blob::BlobDynamicPaged row_blob(pager, row_page);
        U64 offset = row_offset_bytes;
        auto skip = [&offset, &row_blob](U8* out_value, U64 size) {
            blob::get(row_blob, out_value, size, offset);
            offset += size;
        };
        dtype::read(skip, table->cols[col_idx].dtype);
        row_offset_bytes = offset;
        ++col_idx;
        return *this;
    }

    // ========================================================================
    // row iterator
    // ========================================================================
    U64 row_page(RowIterator& it) {
        return *it.btree_it;
    }

    ColumnIterator columns_begin(RowIterator& it) {
        return columns_begin(it.pager, it.table, row_page(it));
    }

    ColumnIterator columns_end(const RowIterator& it) {
        return columns_end(it.table);
    }

    RowIterator& RowIterator::operator++() {
        ++btree_it;
        ++row_idx;
        return *this;
    }

    // ========================================================================
    // execute
    // ========================================================================
    ExecutionResult execute(Engine& engine, const Statement& statement) {
        return visit(statement.value, [&](const auto& stmt) -> ExecutionResult {
            using T = RemoveCVRef<decltype(stmt)>;

            if constexpr (SameAs<T, CreateKeyspace>) {
                if (!stmt.if_not_exists) {
                    auto existing = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                    if (existing != nullptr) {
                        return {
                            .status = ExecutionStatus::AlreadyExists,
                            .message = "Keyspace already exists",
                            .keyspace = stmt.keyspace_name,
                        };
                    }
                }

                auto ks = schema::create_keyspace(engine.schema, stmt);
                if (ks == nullptr) {
                    return {.status = ExecutionStatus::ServerError, .message = "Failed to create keyspace"};
                }
    
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .message = "CREATED",
                    .keyspace = stmt.keyspace_name,
                };
            } else if constexpr (SameAs<T, CreateTable>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }

                if (!stmt.if_not_exists) {
                    auto existing = schema::read_table(engine.schema, *ks, stmt.table_name);
                    if (existing != nullptr) {
                        return {
                            .status = ExecutionStatus::AlreadyExists,
                            .message = "Table already exists",
                            .keyspace = stmt.keyspace_name,
                            .table = stmt.table_name,
                        };
                    }
                }
    
                auto tbl = schema::create_table(engine.schema, *ks, stmt);
                if (tbl == nullptr) {
                    return {.status = ExecutionStatus::ServerError, .message = "Failed to create table"};
                }
                
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .message = "CREATED",
                    .keyspace = stmt.keyspace_name,
                    .table = stmt.table_name,
                };
            } else if constexpr (SameAs<T, InsertInto>) {
                // @todo implement these features
                assert_true(stmt.column_names.length == 0, "INSERT with column names not implemented");
                assert_true(stmt.timestamp == -1, "INSERT with USING TIMESTAMP not implemented");
                assert_true(stmt.ttl == -1, "INSERT with USING TTL not implemented");
                assert_true(!stmt.if_not_exists, "INSERT IF NOT EXISTS not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
    
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }
    
                if (tbl->cols.length != stmt.values.length) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Column count mismatch",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }

                for (U64 idx = 0; idx < tbl->cols.length; idx++) {
                    const auto& col = tbl->cols[idx];
                    const auto& value = stmt.values[idx];

                    if (!dtype::can_write(value, col.dtype)) {
                        return {
                            .status = ExecutionStatus::Invalid,
                            .message = "Value does not match its column's type",
                            .keyspace = stmt.keyspace_name,
                            .table = stmt.table_name,
                        };
                    }
                }
    
                // @todo static blobs/in-tree for fixed column set
                U64 row_page = blob::create_paged_dynamic(*engine.pager);
                blob::BlobDynamicPaged row_blob(engine.pager, row_page);
    
                U64 row_offset_bytes = 0;
                auto write = [&row_offset_bytes,&row_blob](const U8* in_value, U64 size) {
                    blob::insert(row_blob, in_value, size, row_offset_bytes);
                    row_offset_bytes += size;
                };

                for (U64 idx = 0; idx < tbl->cols.length; idx++) {
                    const auto& col = tbl->cols[idx];
                    const auto& value = stmt.values[idx];
    
                    dtype::write(write, value, col.dtype);
                }
                
                // @todo uniqueness check
                tinsert(tbl->btree, dtype::hash(stmt.values[tbl->primary_col_idx]), row_page);
                
                return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
            } else if constexpr (SameAs<T, SelectFrom>) {
                // @todo implement these features
                assert_true(stmt.column_names.cap == 0, "SELECT with column names not implemented");
                assert_true(stmt.where.cap == 0, "SELECT with WHERE clause not implemented");
                assert_true(stmt.limit == -1, "SELECT with LIMIT not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
    
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }

                RowRange rows{
                    .begin_it = RowIterator{
                        .pager = engine.pager,
                        .table = tbl,
                        .btree_it = btree::tbegin<U64>(tbl->btree),
                        .btree_end = btree::tend<U64>(tbl->btree),
                        .row_idx = 0,
                    },
                    .end_it = RowIterator{
                        .pager = engine.pager,
                        .table = tbl,
                        .btree_it = btree::tend<U64>(tbl->btree),
                        .btree_end = btree::tend<U64>(tbl->btree),
                        .row_idx = 0,
                    },
                };

                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::Rows,
                    .keyspace = stmt.keyspace_name,
                    .table = stmt.table_name,
                    .rows = move(rows),
                };
            } else if constexpr (SameAs<T, UseKeyspace>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                engine.current_keyspace = stmt.keyspace_name;
                return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
            } else if constexpr (SameAs<T, AlterKeyspace>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    if (stmt.if_exists) {
                        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
                    }
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .keyspace = stmt.keyspace_name,
                };
            } else if constexpr (SameAs<T, DropKeyspace>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    if (stmt.if_exists) {
                        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
                    }
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                for (auto& tbl : ks->tbls) {
                    if (!tbl.tombstone) {
                        schema::delete_table(engine.schema, *ks, tbl.name);
                    }
                }
                schema::delete_keyspace(engine.schema, stmt.keyspace_name);
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .keyspace = stmt.keyspace_name,
                };
            } else if constexpr (SameAs<T, AlterTable>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    if (stmt.if_exists) {
                        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
                    }
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }
                if (stmt.op == AlterTableOp::add_column) {
                    for (U64 i = 0; i < stmt.columns.length; i++) {
                        const auto& col = stmt.columns[i];
                        CreateColumn create{.name = col.name, .dtype = col.dtype, .is_primary_key = false};
                        if (schema::create_column(engine.schema, *tbl, create) == nullptr) {
                            return {
                                .status = ExecutionStatus::Invalid,
                                .message = "Column already exists",
                                .keyspace = stmt.keyspace_name,
                                .table = stmt.table_name,
                            };
                        }
                    }
                } else if (stmt.op == AlterTableOp::drop_column) {
                    for (U64 i = 0; i < stmt.columns.length; i++) {
                        if (!schema::delete_column(engine.schema, *tbl, stmt.columns[i].name)) {
                            return {
                                .status = ExecutionStatus::Invalid,
                                .message = "Column does not exist",
                                .keyspace = stmt.keyspace_name,
                                .table = stmt.table_name,
                            };
                        }
                    }
                } else {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "RENAME COLUMN not supported",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .keyspace = stmt.keyspace_name,
                    .table = stmt.table_name,
                };
            } else if constexpr (SameAs<T, DropTable>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                if (!schema::delete_table(engine.schema, *ks, stmt.table_name)) {
                    if (stmt.if_exists) {
                        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
                    }
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .keyspace = stmt.keyspace_name,
                    .table = stmt.table_name,
                };
            } else if constexpr (SameAs<T, TruncateTable>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }
                btree::truncate(tbl->btree);
                return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
            } else if constexpr (SameAs<T, Update>) {
                // @todo implement USING TIMESTAMP, USING TTL, IF EXISTS
                assert_true(stmt.timestamp == -1, "UPDATE with USING TIMESTAMP not implemented");
                assert_true(stmt.ttl == -1, "UPDATE with USING TTL not implemented");
                assert_true(!stmt.if_exists, "UPDATE IF EXISTS not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }

                // find primary key equality in WHERE clause
                const auto& pk_col = tbl->cols[tbl->primary_col_idx];
                const WhereRelation* pk_where = nullptr;
                for (U64 i = 0; i < stmt.where.cap; i++) {
                    if (stmt.where[i].column_name == pk_col.name && stmt.where[i].op == ComparisonOp::eq) {
                        pk_where = &stmt.where[i];
                        break;
                    }
                }
                if (pk_where == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "UPDATE requires primary key equality in WHERE clause",
                    };
                }

                U64 pk_hash = dtype::hash(pk_where->value);
                auto row_page_opt = btree::tfind<U64>(tbl->btree, pk_hash);
                if (!row_page_opt) {
                    return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
                }
                U64 row_page = *row_page_opt;

                blob::BlobDynamicPaged row_blob(engine.pager, row_page);

                // read all column values
                DynamicArray<dtype::ReadValue> read_values{};
                U64 read_offset = 0;
                auto read_fn = [&](U8* out, U64 size) {
                    blob::get(row_blob, out, size, read_offset);
                    read_offset += size;
                };
                for (const auto& col : tbl->cols) {
                    push_back(read_values, dtype::read(read_fn, col.dtype));
                }

                // rewrite row applying assignments
                blob::resize(row_blob, 0);
                U64 write_offset = 0;
                auto write_fn = [&](const U8* in, U64 size) {
                    blob::insert(row_blob, in, size, write_offset);
                    write_offset += size;
                };
                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                    const auto& col = tbl->cols[ci];

                    const dtype::WriteValue* updated = nullptr;
                    for (U64 ai = 0; ai < stmt.assignments.cap; ai++) {
                        if (stmt.assignments[ai].column_name == col.name) {
                            updated = &stmt.assignments[ai].value;
                            break;
                        }
                    }
                    if (updated != nullptr) {
                        dtype::write(write_fn, *updated, col.dtype);
                    } else {
                        dtype::write_from_read(write_fn, read_values[ci], col.dtype);
                    }
                }

                return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
            } else if constexpr (SameAs<T, Delete>) {
                // @todo implement USING TIMESTAMP
                assert_true(stmt.timestamp == -1, "DELETE with USING TIMESTAMP not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }

                // find primary key equality in WHERE clause
                const auto& pk_col = tbl->cols[tbl->primary_col_idx];
                const WhereRelation* pk_where = nullptr;
                for (U64 i = 0; i < stmt.where.cap; i++) {
                    if (stmt.where[i].column_name == pk_col.name && stmt.where[i].op == ComparisonOp::eq) {
                        pk_where = &stmt.where[i];
                        break;
                    }
                }
                if (pk_where == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "DELETE requires primary key equality in WHERE clause",
                    };
                }

                U64 pk_hash = dtype::hash(pk_where->value);

                if (stmt.column_names.cap == 0) {
                    // delete entire row
                    btree::remove(tbl->btree, pk_hash);
                } else {
                    // delete specific columns: set them to default
                    auto row_page_opt = btree::tfind<U64>(tbl->btree, pk_hash);
                    if (!row_page_opt) {
                        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
                    }
                    U64 row_page = *row_page_opt;
                    blob::BlobDynamicPaged row_blob(engine.pager, row_page);

                    DynamicArray<dtype::ReadValue> read_values{};
                    U64 read_offset = 0;
                    auto read_fn = [&](U8* out, U64 size) {
                        blob::get(row_blob, out, size, read_offset);
                        read_offset += size;
                    };
                    for (const auto& col : tbl->cols) {
                        push_back(read_values, dtype::read(read_fn, col.dtype));
                    }

                    blob::resize(row_blob, 0);
                    U64 write_offset = 0;
                    auto write_fn = [&](const U8* in, U64 size) {
                        blob::insert(row_blob, in, size, write_offset);
                        write_offset += size;
                    };
                    for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                        const auto& col = tbl->cols[ci];
                        bool is_deleted = false;
                        for (U64 di = 0; di < stmt.column_names.cap; di++) {
                            if (stmt.column_names[di] == col.name) {
                                is_deleted = true;
                                break;
                            }
                        }
                        if (is_deleted) {
                            dtype::write_default(write_fn, col.dtype);
                        } else {
                            dtype::write_from_read(write_fn, read_values[ci], col.dtype);
                        }
                    }
                }

                return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
            } else if constexpr (SameAs<T, Batch>) {
                for (U64 i = 0; i < stmt.statements.length; i++) {
                    ExecutionResult result = visit(stmt.statements[i].value, [&](const auto& sub_stmt) -> ExecutionResult {
                        Statement sub{};
                        sub.value = sub_stmt;
                        return execute(engine, sub);
                    });
                    if (result.status != ExecutionStatus::Success) {
                        return result;
                    }
                }
                return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
            } else {
                static_assert(false, "Unhandled statement type in engine::execute");
            }
        });
    }
}