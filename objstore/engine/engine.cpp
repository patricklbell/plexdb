module objstore.engine;

import plexdb.os;
import plexdb.os.dynamic_tagged_union;
import objstore.parsers;

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
        DynamicArray<bool> active_cols(table->cols.length);
        blob::BlobDynamicPaged row_blob(pager, row_page);
        U64 offset = 0;
        auto read = [&offset, &row_blob](U8* out_value, U64 size) {
            blob::get(row_blob, out_value, size, offset);
            offset += size;
        };
        types::read_col_mask(read, [&active_cols](U64 idx) { active_cols[idx] = true; });
        return ColumnIterator{
            .pager = pager,
            .table = table,
            .row_page = row_page,
            .col_idx = 0,
            .row_offset_bytes = offset,
            .active_cols = move(active_cols),
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

    types::ReadValue read_value(ColumnIterator& it) {
        if (it.col_idx < it.active_cols.length && !it.active_cols[it.col_idx]) {
            auto null_read = [](U8* out, U64 size) { for (U64 i = 0; i < size; i++) out[i] = 0; };
            return types::read_specific(null_read, it.table->cols[it.col_idx].type);
        }
        blob::BlobDynamicPaged row_blob(it.pager, it.row_page);
        U64 offset = it.row_offset_bytes;
        auto read = [&offset, &row_blob](U8* out_value, U64 size) {
            blob::get(row_blob, out_value, size, offset);
            offset += size;
        };
        return types::read_specific(read, it.table->cols[it.col_idx].type);
    }

    ColumnIterator& ColumnIterator::operator++() {
        if (col_idx < active_cols.length && active_cols[col_idx]) {
            blob::BlobDynamicPaged row_blob(pager, row_page);
            U64 offset = row_offset_bytes;
            auto skip = [&offset, &row_blob](U8* out_value, U64 size) {
                blob::get(row_blob, out_value, size, offset);
                offset += size;
            };
            types::read_specific(skip, table->cols[col_idx].type);
            row_offset_bytes = offset;
        }
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
    // system virtual tables
    // ========================================================================
    bool is_system_keyspace(String8 ks) {
        return ks == "system" || ks == "system_schema" ||
               ks == "system_virtual_schema" || ks == "system_auth" ||
               ks == "system_distributed" || ks == "system_traces";
    }

    VirtualRows make_system_local() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table = "local";

        push_back(vr.columns, VirtualColumn{"key", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"bootstrapped", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"broadcast_address", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"broadcast_port", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"cluster_name", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"cql_version", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"data_center", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"host_id", types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"listen_address", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"listen_port", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"native_protocol_version", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"partitioner", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"rack", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"release_version", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"rpc_address", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"rpc_port", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"schema_version", types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"tokens", types::make_set(types::text)});

        VirtualRow row;
        push_back(row.values, types::ReadValue{"local"_as});
        push_back(row.values, types::ReadValue{"COMPLETED"_as});
        push_back(row.values, types::ReadValue{"127.0.0.1"_as});
        push_back(row.values, types::ReadValue{S32(7000)});
        push_back(row.values, types::ReadValue{"objstore"_as});
        push_back(row.values, types::ReadValue{"3.4.7"_as});
        push_back(row.values, types::ReadValue{"datacenter1"_as});
        push_back(row.values, types::ReadValue{Array<U8,16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}});
        push_back(row.values, types::ReadValue{"127.0.0.1"_as});
        push_back(row.values, types::ReadValue{S32(7000)});
        push_back(row.values, types::ReadValue{"4"_as});
        push_back(row.values, types::ReadValue{"org.apache.cassandra.dht.Murmur3Partitioner"_as});
        push_back(row.values, types::ReadValue{"rack1"_as});
        push_back(row.values, types::ReadValue{"3.11.19"_as}); // @note last version in 3.x, before system_virtual
        push_back(row.values, types::ReadValue{"127.0.0.1"_as});
        push_back(row.values, types::ReadValue{S32(9042)});
        push_back(row.values, types::ReadValue{Array<U8,16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}); 
        push_back(row.values, types::ReadValue{DynamicSet<AutoString8>{{"0"_as}}});
        push_back(vr.rows, move(row));

        return vr;
    }

    VirtualRows make_system_peers() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table = "peers";

        push_back(vr.columns, VirtualColumn{"peer", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"data_center", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"host_id", types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"preferred_ip", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"rack", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"release_version", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"rpc_address", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"schema_version", types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"tokens", types::make_set(types::text)});

        return vr;
    }

    VirtualRows make_system_peers_v2() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table = "peers_v2";

        push_back(vr.columns, VirtualColumn{"peer", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"peer_port", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"data_center", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"host_id", types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"native_address", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"native_port", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"preferred_ip", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"preferred_port", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"rack", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"release_version", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"schema_version", types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"tokens", types::make_native(types::text)});

        return vr;
    }

    // @todo in cassandra the schema is stored in the database directly, this is probably a good idea in future
    Optional<VirtualRows> try_system_select(Engine& engine, String8 keyspace, String8 table) {
        if (keyspace == "system") {
            if (table == "local")     return make_system_local();
            if (table == "peers")     return make_system_peers();
            if (table == "peers_v2")  return make_system_peers_v2();
        }
        if (keyspace == "system_schema") {
            if (table == "keyspaces")        return make_schema_keyspaces(engine.schema);
            if (table == "tables")           return make_schema_tables(engine.schema);
            if (table == "columns")          return make_schema_columns(engine.schema);
            if (table == "views")            return make_schema_views(engine.schema);
            if (table == "indexes")          return make_schema_indexes(engine.schema);
            if (table == "triggers")         return make_schema_triggers(engine.schema);
            if (table == "dropped_columns")  return make_schema_dropped_columns(engine.schema);
            if (table == "types")            return make_schema_types(engine.schema);
            if (table == "functions")        return make_schema_functions(engine.schema);
            if (table == "aggregates")       return make_schema_aggregates(engine.schema);
        }
        return {};
    }

    // ========================================================================
    // execute
    // ========================================================================
    static ExecutionResult make_void_success() {
        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
    }
    // @note msg should be static storage duration
    static ExecutionResult make_server_error(const char* msg) {
        return {.status = ExecutionStatus::ServerError, .message = msg};
    }
    static ExecutionResult make_keyspace_already_exists(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::AlreadyExists,
            .message = "Keyspace already exists",
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult make_keyspace_created(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .message = "CREATED",
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult make_keyspace_not_found(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Keyspace does not exist",
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult make_use_keyspace(const String8& keyspace_name) {
        return {.status = ExecutionStatus::Success, .kind = ResultKind::UseKeyspace, .keyspace = keyspace_name};
    }
    static ExecutionResult make_table_already_exists(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::AlreadyExists,
            .message = "Table already exists",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_table_not_found(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Table does not exist",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_table_created(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .message = "CREATED",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_schema_changed(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult make_schema_changed(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_insert_column_does_not_match_value_count(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Column count does not match value count",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_insert_into_deleted_column(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Cannot insert into deleted column",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_insert_into_unknown_column(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Too many values or unknown column",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_insert_incompatible_literal(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Incompatible literal for column type",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }

    ExecutionResult execute(Engine& engine, const Statement& statement) {
        return visit(statement.value, [&](const auto& stmt) -> ExecutionResult {
            using T = RemoveCVRef<decltype(stmt)>;

            if constexpr (SameAs<T, CreateKeyspace>) {
                if (is_system_keyspace(stmt.name)) {
                    if (stmt.if_not_exists) {
                        return make_void_success();
                    }
                    return make_keyspace_already_exists(stmt.name);
                }

                if (!stmt.if_not_exists) {
                    auto existing = schema::read_keyspace(engine.schema, stmt.name);
                    if (existing != nullptr) return make_keyspace_already_exists(stmt.name);
                }

                auto ks = schema::create_keyspace(engine.schema, stmt);
                if (ks == nullptr) {
                    return make_server_error("Failed to create keyspace");
                }
    
                return make_keyspace_created(stmt.name);
            } else if constexpr (SameAs<T, CreateTable>) {
                String8 ks_name = static_cast<bool>(stmt.name.keyspace_name) ? String8(*stmt.name.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name);
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                if (!stmt.if_not_exists) {
                    auto existing = schema::read_table(engine.schema, *ks, stmt.name.table_name);
                    if (existing != nullptr) return make_table_already_exists(ks_name, stmt.name.table_name);
                }
    
                auto tbl = schema::create_table(engine.schema, *ks, stmt);
                if (tbl == nullptr) {
                    return make_server_error("Failed to create table");
                }
                
                return make_table_created(ks_name, stmt.name.table_name);
            } else if constexpr (SameAs<T, UseKeyspace>) {
                if (is_system_keyspace(stmt.keyspace)) {
                    engine.current_keyspace = stmt.keyspace;
                    return make_use_keyspace(engine.current_keyspace);
                }
                
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace);
                if (ks == nullptr) return make_keyspace_not_found(stmt.keyspace);
                engine.current_keyspace = stmt.keyspace;

                return make_use_keyspace(engine.current_keyspace);
            } else if constexpr (SameAs<T, AlterKeyspace>) {
                assert_true_not_implemented(!is_system_keyspace(stmt.keyspace));

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace);
                if (ks == nullptr) {
                    if (stmt.if_exists) return make_void_success();
                    return make_keyspace_not_found(stmt.keyspace);
                }

                assert_true_not_implemented(stmt.options.identifier_values.length == 0);
                
                return make_schema_changed(stmt.keyspace);
            } else if constexpr (SameAs<T, DropKeyspace>) {
                assert_true_not_implemented(!is_system_keyspace(stmt.keyspace));

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace);
                if (ks == nullptr) {
                    if (stmt.if_exists) return make_void_success();
                    return make_keyspace_not_found(stmt.keyspace);
                }
                
                schema::delete_keyspace(engine.schema, stmt.keyspace);

                return make_schema_changed(stmt.keyspace);
            } else if constexpr (SameAs<T, DropTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name);
                if (ks == nullptr) {
                    if (stmt.if_exists) return make_void_success();
                    return make_keyspace_not_found(ks_name);
                }

                if (!schema::delete_table(engine.schema, *ks, stmt.table.table_name)) {
                    if (stmt.if_exists) return make_void_success();
                    return make_table_not_found(ks_name, stmt.table.table_name);
                }
                
                return make_schema_changed(ks_name, stmt.table.table_name);
            } else if constexpr (SameAs<T, TruncateTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name);
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name);
                if (tbl == nullptr) return make_table_not_found(ks_name, stmt.table.table_name);
                
                btree::truncate(tbl->btree);

                return make_void_success();
            } else if constexpr (SameAs<T, Select>) {
                String8 ks_name = static_cast<bool>(stmt.from.keyspace_name) ? String8(*stmt.from.keyspace_name) : engine.current_keyspace;

                auto system_vr = try_system_select(engine, ks_name, stmt.from.table_name);
                if (system_vr) {
                    return {
                        .status = ExecutionStatus::Success,
                        .kind = ResultKind::VirtualRows,
                        .keyspace = ks_name,
                        .table = stmt.from.table_name,
                        .virtual_rows = move(system_vr),
                    };
                }
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name);
                if (ks == nullptr) return make_keyspace_not_found(ks_name);
    
                auto tbl = schema::read_table(engine.schema, *ks, stmt.from.table_name);
                if (tbl == nullptr) return make_table_not_found(ks_name, stmt.from.table_name);

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
                    .keyspace = ks_name,
                    .table = stmt.from.table_name,
                    .rows = move(rows),
                };
            } else if constexpr (SameAs<T, Insert>) {
                assert_true_not_implemented(stmt.using_parameters.length == 0);
                assert_true(static_cast<bool>(stmt.insert_clause), "missing insert clause, this should never happen");
                
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name);
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name);
                if (tbl == nullptr) return make_table_not_found(ks_name, stmt.table.table_name);

                return visit(stmt.insert_clause, [&](const auto& v) -> ExecutionResult {
                    using T = Decay<decltype(v)>;

                    if constexpr (SameAs<T, Insert::NamesValues>) {
                        if (v.names.length != v.values.length) return make_insert_column_does_not_match_value_count(ks->name, tbl->name);

                        auto try_get_names_idx = [&v](const String8& q) -> Optional<U64> {
                            for (U64 idx = 0; idx < v.names.length ; idx++) {
                                if (v.names[idx].identifier == q)
                                    return idx;
                            }
                            return {};
                        };

                        // check columns and build mapping
                        for (const auto& col : tbl->cols) {
                            auto names_idx_opt = try_get_names_idx(col.name);
                            if (names_idx_opt) {
                                // @todo timestamp
                                if (col.tombstone) {
                                    return make_insert_into_deleted_column(ks->name, tbl->name);
                                }

                                const auto& constant = consteval_term_to_constant(v.values[*names_idx_opt]);
                                if (!types::can_write_constant_value(constant.value, col.type.native.value_dtype))
                                    return make_insert_incompatible_literal(ks->name, tbl->name);
                            }
                        }

                        // @todo uniqueness check
                        assert_true_not_implemented(!stmt.if_not_exists);

                        // write new values
                        // @todo avoid copying by having separate read/write pages help in transactions
                        {
                            // @todo static blobs/in-tree for fixed column set
                            U64 row_page = blob::create_paged_dynamic(*engine.pager);
                            blob::BlobDynamicPaged row_blob(engine.pager, row_page);

                            U64 row_offset_bytes = 0;
                            auto write = [&row_offset_bytes,&row_blob](const U8* in_value, U64 size) {
                                blob::insert(row_blob, in_value, size, row_offset_bytes);
                                row_offset_bytes += size;
                            };

                            types::write_col_mask(write, [&](U64 col_idx) { return static_cast<bool>(try_get_names_idx(tbl->cols[col_idx].name)); }, tbl->cols.length);

                            for (const auto& col : tbl->cols) {
                                auto names_idx_opt = try_get_names_idx(col.name);
                                if (names_idx_opt) {
                                    const auto& constant = consteval_term_to_constant(v.values[*names_idx_opt]);
                                    types::write_constant_value(write, constant.value, col.type.native.value_dtype);
                                }
                            }

                            const auto& pk_col = tbl->cols[tbl->primary_col_idx];
                            auto pk_idx_opt = try_get_names_idx(pk_col.name);
                            assert_true(static_cast<bool>(pk_idx_opt), "primary key column must be provided in INSERT");
                            const auto& pk_constant = consteval_term_to_constant(v.values[*pk_idx_opt]);
                            U64 pk_key = types::hash_constant_value(pk_constant.value);
                            tinsert(tbl->btree, pk_key, row_page);
                        }
                        
                        return make_void_success();
                    } else if constexpr (SameAs<T, Insert::JsonClause>) {
                        assert_not_implemented();
                        return ExecutionResult{};
                    } else {
                        static_assert(!SameAs<T,T>);
                    }
                });
            } else if constexpr (SameAs<T, Update>) {
                assert_not_implemented();
                return ExecutionResult{};
            } else if constexpr (SameAs<T, Delete>) {
                assert_not_implemented();
                return ExecutionResult{};
            } else if constexpr (SameAs<T, AlterTable>) {
                assert_not_implemented();
                return ExecutionResult{};
            } else if constexpr (SameAs<T, Batch>) {
                assert_not_implemented();
                return ExecutionResult{};
            } else {
                static_assert(false, "Unhandled statement type in engine::execute");
            }
        });
    }

    // ========================================================================
    // bind variables
    // ========================================================================
    static void collect_bind_variables_insert(Engine& engine, const Insert& stmt, DynamicArray<BindVariableSpec>& out) {
        String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
        auto ks = schema::read_keyspace(engine.schema, ks_name);
        if (ks == nullptr) return;
        auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name);
        if (tbl == nullptr) return;

        if (!type_matches_tag<Insert::NamesValues>(stmt.insert_clause)) return;
        const auto& nv = get<Insert::NamesValues>(stmt.insert_clause);

        for (U64 i = 0; i < nv.values.length; i++) {
            // @todo nested bind markers
            if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                String8 col_name = nv.names[i].identifier;
                CqlType col_type = types::make_native(types::text);
                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                    if (tbl->cols[ci].name == col_name) {
                        col_type = tbl->cols[ci].type;
                        break;
                    }
                }
                push_back(out, BindVariableSpec{.name = AutoString8(col_name), .type = col_type});
            }
        }
    }

    DynamicArray<BindVariableSpec> collect_bind_variables(Engine& engine, const Statement& statement) {
        DynamicArray<BindVariableSpec> result;
        visit(statement.value, [&](const auto& stmt) {
            using T = RemoveCVRef<decltype(stmt)>;
            if constexpr (SameAs<T, Insert>) {
                collect_bind_variables_insert(engine, stmt, result);
            }
        });
        return result;
    }

    static void bind_values_to_statement(Statement& stmt, DynamicArray<Constant>& bound_values) {
        visit(stmt.value, [&](auto& s) {
            using T = RemoveCVRef<decltype(s)>;
            if constexpr (SameAs<T, Insert>) {
                if (!type_matches_tag<Insert::NamesValues>(s.insert_clause)) return;
                auto& nv = get<Insert::NamesValues>(s.insert_clause);
                U64 bind_idx = 0;
                for (U64 i = 0; i < nv.values.length && bind_idx < bound_values.length; i++) {
                    // @todo nested bind markers
                    if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                        nv.values[i].value = move(bound_values[bind_idx]);
                        bind_idx++;
                    }
                }
            }
        });
    }

    ExecutionResult execute_with_values(Engine& engine, Statement& statement, DynamicArray<Constant>&& bound_values) {
        bind_values_to_statement(statement, bound_values);
        return execute(engine, statement);
    }

    // ========================================================================
    // prepared statements
    // ========================================================================
    PrepareResult prepare(Engine& engine, String8 query) {
        U64 query_hash = hash(query);

        auto* existing = find(engine.prepared_cache, query_hash);
        if (existing != nullptr) {
            return { .status = ExecutionStatus::Success, .id = query_hash, .entry = existing };
        }

        auto cql_opt = parsers::cql::parse(query);
        if (!cql_opt) {
            return { .status = ExecutionStatus::SyntaxError, .message = "Failed to parse CQL" };
        }

        auto& entry = insert(engine.prepared_cache, query_hash);
        entry.query_string = AutoString8(query);
        entry.bind_variables = collect_bind_variables(engine, *cql_opt);

        visit(cql_opt->value, [&](const auto& stmt) {
            using T = RemoveCVRef<decltype(stmt)>;
            if constexpr (SameAs<T, Insert>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                entry.keyspace = AutoString8(ks_name);
                entry.table = AutoString8(stmt.table.table_name);

                auto ks = schema::read_keyspace(engine.schema, ks_name);
                if (ks == nullptr) return;
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name);
                if (tbl == nullptr) return;
                for (U64 i = 0; i < entry.bind_variables.length; i++) {
                    if (tbl->primary_col_idx < tbl->cols.length && tbl->cols[tbl->primary_col_idx].name == entry.bind_variables[i].name) {
                        entry.pk_index = S32(i);
                        break;
                    }
                }
            }
        });

        return { .status = ExecutionStatus::Success, .id = query_hash, .entry = &entry };
    }

    PreparedEntry* find_prepared(Engine& engine, U64 prepared_id) {
        return find(engine.prepared_cache, prepared_id);
    }

    ExecutionResult execute_prepared(Engine& engine, U64 prepared_id, DynamicArray<Constant>&& bound_values) {
        auto* entry = find(engine.prepared_cache, prepared_id);
        if (entry == nullptr) {
            return { .status = ExecutionStatus::Invalid, .message = "Prepared statement not found" };
        }

        auto cql_opt = parsers::cql::parse(String8(entry->query_string));
        if (!cql_opt) {
            return { .status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query" };
        }

        return execute_with_values(engine, *cql_opt, move(bound_values));
    }
}