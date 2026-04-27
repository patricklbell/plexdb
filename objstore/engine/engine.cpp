module objstore.engine;

import plexdb.os;
import plexdb.os.dynamic_tagged_union;

import objstore.parsers;
import objstore.engine.evaluator;
import objstore.engine.it;

namespace objstore::engine {
    Engine::Engine(Pager* in_pager) : pager(in_pager), schema(in_pager, in_pager->header.root_page) {}

    void create_database(Pager& pager) {
        U64 schema_page = schema::create_schema(pager);
        pager::set_root(pager, schema_page);
    }

    // ========================================================================
    // system virtual tables
    // ========================================================================
    bool is_system_keyspace(String8 ks) {
        return ks == "system" || ks == "system_schema" ||
               ks == "system_virtual_schema" || ks == "system_auth" ||
               ks == "system_distributed" || ks == "system_traces";
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
    [[maybe_unused]] static ExecutionResult make_insert_into_unknown_column(const String8& keyspace_name, const String8& table_name) {
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
    static ExecutionResult make_insert_missing_pks(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Insert is missing a value for the primary keys",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult make_where_invalid_type(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Invalid type",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }

    ExecutionResult execute(Engine& engine, const Statement& statement) {
        return visit(statement.value, [&](const auto& stmt) -> ExecutionResult {
            using T = RemoveCVRef<decltype(stmt)>;

            if constexpr (SameAs<T, CreateKeyspace>) {
                if (is_system_keyspace(stmt.name)) {
                    return (stmt.if_not_exists) ? make_void_success() : make_keyspace_already_exists(stmt.name);
                }

                if (auto existing = schema::read_keyspace(engine.schema, stmt.name).value; existing != nullptr) { // @todo propagate error
                    return (stmt.if_not_exists) ? make_void_success() : make_keyspace_already_exists(stmt.name);
                }

                auto ks = schema::create_keyspace(engine.schema, stmt).value; // @todo propagate error
                if (ks == nullptr) {
                    return make_server_error("Failed to create keyspace");
                }

                return make_keyspace_created(stmt.name);
            } else if constexpr (SameAs<T, CreateTable>) {
                String8 ks_name = static_cast<bool>(stmt.name.keyspace_name) ? String8(*stmt.name.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name).value; // @todo propagate error
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                if (auto existing = schema::read_table(engine.schema, *ks, stmt.name.table_name); existing.value != nullptr) { // @todo propagate error
                    return (stmt.if_not_exists) ? make_void_success() : make_table_already_exists(ks_name, stmt.name.table_name);
                }

                auto tbl = schema::create_table(engine.schema, *ks, stmt).value; // @todo propagate error
                if (tbl == nullptr) {
                    return make_server_error("Failed to create table");
                }

                return make_table_created(ks_name, stmt.name.table_name);
            } else if constexpr (SameAs<T, UseKeyspace>) {
                if (is_system_keyspace(stmt.keyspace)) {
                    engine.current_keyspace = stmt.keyspace;
                    return make_use_keyspace(engine.current_keyspace);
                }

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value; // @todo propagate error
                if (ks == nullptr) return make_keyspace_not_found(stmt.keyspace);
                engine.current_keyspace = stmt.keyspace;

                return make_use_keyspace(engine.current_keyspace);
            } else if constexpr (SameAs<T, AlterKeyspace>) {
                assert_true_not_implemented(!is_system_keyspace(stmt.keyspace));

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value; // @todo propagate error
                if (ks == nullptr) {
                    return (stmt.if_exists) ? make_void_success() :  make_keyspace_not_found(stmt.keyspace);
                }

                assert_true_not_implemented(stmt.options.identifier_values.length == 0);

                return make_schema_changed(stmt.keyspace);
            } else if constexpr (SameAs<T, DropKeyspace>) {
                assert_true_not_implemented(!is_system_keyspace(stmt.keyspace));

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value; // @todo propagate error
                if (ks == nullptr) {
                    return (stmt.if_exists) ? make_void_success() : make_keyspace_not_found(stmt.keyspace);
                }

                schema::delete_keyspace(engine.schema, stmt.keyspace);

                return make_schema_changed(stmt.keyspace);
            } else if constexpr (SameAs<T, DropTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name).value; // @todo propagate error
                if (ks == nullptr) {
                    return (stmt.if_exists) ? make_void_success() : make_keyspace_not_found(ks_name);
                }

                if (schema::delete_table(engine.schema, *ks, stmt.table.table_name).error != schema::Error::None) { ; // @todo propagate error
                    if (stmt.if_exists) return make_void_success();
                    return make_table_not_found(ks_name, stmt.table.table_name);
                }

                return make_schema_changed(ks_name, stmt.table.table_name);
            } else if constexpr (SameAs<T, TruncateTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name).value; // @todo propagate error
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value; // @todo propagate error
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

                auto ks = schema::read_keyspace(engine.schema, ks_name).value; // @todo propagate error
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.from.table_name).value; // @todo propagate error
                if (tbl == nullptr) return make_table_not_found(ks_name, stmt.from.table_name);

                assert_true_not_implemented(!stmt.transform, "SELECT DISTINCT/JSON is not implemented");
                assert_true_not_implemented(!stmt.group_by, "GROUP BY is not implemented");
                assert_true_not_implemented(!stmt.per_partition_limit.value, "PER PARTITION LIMIT is not implemented");
                assert_true_not_implemented(!stmt.order_by, "ORDER BY is not implemented");
                assert_true_not_implemented(!stmt.limit.value, "LIMIT is not implemented");

                bool is_equality = false, is_begin_inclusive = true, is_end_inclusive = true;
                Optional<Evaluated> pk_begin, pk_end;
                if (stmt.where) {
                    assert_true_not_implemented(stmt.where->relations.length == 1, "having multiple where relations is not supported");
                    for (const auto& rel : stmt.where->relations) {
                        visit(rel.value, [&](const auto& value){
                            using T = Decay<decltype(value)>;
                            if constexpr (SameAs<T, WhereClause::ColumnExpressionRelation>) {
                                const auto& cer = value;
                                if (cer.column.identifier == tbl->cols[tbl->primary_col_idx].name) {
                                    switch (cer.operator_) {
                                        case Operator::eq:{
                                            pk_begin = evaluate(cer.value);
                                            is_equality = true;
                                            is_begin_inclusive = true;
                                            is_end_inclusive = true;
                                        }break;
                                        case Operator::lt:{
                                            pk_end = evaluate(cer.value);
                                            is_end_inclusive = false;
                                        }break;
                                        case Operator::le:{
                                            pk_end = evaluate(cer.value);
                                            is_end_inclusive = true;
                                        }break;
                                        case Operator::gt:{
                                            pk_begin = evaluate(cer.value);
                                            is_begin_inclusive = false;
                                        }break;
                                        case Operator::ge:{
                                            pk_begin = evaluate(cer.value);
                                            is_begin_inclusive = true;
                                        }break;
                                        default:{
                                            assert_not_implemented("column expression relation operator not implemented for PK");
                                        }break;
                                    }
                                } else {
                                    assert_not_implemented("non-PK column expression relations are not implemented");
                                }
                            } else if constexpr (SameAs<T, WhereClause::TupleExpressionRelation>) {
                                assert_not_implemented("tuple expression relations are not implemented");
                            } else if constexpr (SameAs<T, WhereClause::TokenRelation>) {
                                assert_not_implemented("token relations are not implemented");
                            } else {
                                static_assert(!SameAs<T,T>, "missing type case");
                            }
                        });
                    }
                }

                // resolve select column indices from SELECT clause
                DynamicArray<U64> select_col_indices;
                if (stmt.select.clauses.length > 0) {
                    for (const auto& sc : stmt.select.clauses) {
                        visit(sc.column.value, [&](const auto& sel) {
                            using ST = RemoveCVRef<decltype(sel)>;
                            if constexpr (SameAs<ST, ColumnName>) {
                                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                    if (tbl->cols[ci].name == sel.identifier) {
                                        push_back(select_col_indices, ci);
                                        break;
                                    }
                                }
                            } else {
                                assert_not_implemented("SELECT clause type (count/function/cast/term) is not implemented");
                            }
                        });
                    }
                }

                if (is_equality) {
                    return {
                        .status = ExecutionStatus::Success,
                        .kind = ResultKind::Rows,
                        .keyspace = ks_name,
                        .table = stmt.from.table_name,
                        .row_limit_count = 1,
                        .rows = RowRange{
                            .start = create_table_eq_it(engine.pager, tbl, hash(*pk_begin)),
                            .stop = create_table_end_it(engine.pager, tbl),
                        },
                        .resolved_table = tbl,
                        .select_col_indices = move(select_col_indices),
                    };
                }

                const auto& pk_col = tbl->cols[tbl->primary_col_idx];
                // @todo better ensure can write evaluated ensures that hash can be compared
                if (static_cast<bool>(pk_begin) && !io::can_cast_write_evaluated_as_column_value(*pk_begin, pk_col.type)) {
                    return make_where_invalid_type(ks->name, tbl->name);
                }
                if (static_cast<bool>(pk_end) && !io::can_cast_write_evaluated_as_column_value(*pk_end, pk_col.type)) {
                    return make_where_invalid_type(ks->name, tbl->name);
                }

                U64 hash_begin = 0, hash_end = MAX_U64;
                if (static_cast<bool>(pk_begin)) {
                    hash_begin = hash(*pk_begin);
                }
                if (static_cast<bool>(pk_end)) {
                    hash_end = hash(*pk_end);
                }
                if (hash_begin > hash_end) {
                    return {
                        .status = ExecutionStatus::Success,
                        .kind = ResultKind::Rows,
                        .keyspace = ks_name,
                        .table = stmt.from.table_name,
                    };
                }

                auto make_start_it = [&]() {
                    if (static_cast<bool>(pk_begin)) {
                        if (is_begin_inclusive) return create_table_ge_it(engine.pager, tbl, hash_begin);
                        else return create_table_gt_it(engine.pager, tbl, hash_begin);
                    }
                    return create_table_begin_it(engine.pager, tbl);
                };
                auto make_stop_it = [&]() {
                    if (static_cast<bool>(pk_end)) {
                        if (is_end_inclusive) return create_table_le_it(engine.pager, tbl, hash_end);
                        else return create_table_lt_it(engine.pager, tbl, hash_end);
                    }
                    return create_table_end_it(engine.pager, tbl);
                };

                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::Rows,
                    .keyspace = ks_name,
                    .table = stmt.from.table_name,
                    .rows = RowRange{.start = make_start_it(), .stop = make_stop_it()},
                    .resolved_table = tbl,
                    .select_col_indices = move(select_col_indices),
                };
            } else if constexpr (SameAs<T, Insert>) {
                assert_true_not_implemented(stmt.using_parameters.length == 0);
                assert_true(static_cast<bool>(stmt.insert_clause), "missing insert clause, this should never happen");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name).value; // @todo propagate error
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value; // @todo propagate error
                if (tbl == nullptr) return make_table_not_found(ks_name, stmt.table.table_name);

                return visit(stmt.insert_clause, [&](const auto& v) -> ExecutionResult {
                    using T = Decay<decltype(v)>;

                    if constexpr (SameAs<T, Insert::NamesValues>) {
                        if (v.names.length != v.values.length) return make_insert_column_does_not_match_value_count(ks->name, tbl->name);

                        // @perf
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

                                // @todo avoid copy here and at other evaluate
                                const auto& eval = evaluate(v.values[*names_idx_opt]);
                                if (!io::can_cast_write_evaluated_as_column_value(eval, col.type))
                                    return make_insert_incompatible_literal(ks->name, tbl->name);
                            }
                        }

                        // check pk is in insert values
                        const auto& pk_col = tbl->cols[tbl->primary_col_idx];
                        auto pk_idx_opt = try_get_names_idx(pk_col.name);
                        if (!pk_idx_opt) {
                            return make_insert_missing_pks(ks->name, tbl->name);
                        }

                        // @todo uniqueness check
                        assert_true_not_implemented(!stmt.if_not_exists);

                        // write new values
                        // @perf avoid copying by allowing separate read/write pages in transactions
                        {
                            // @todo static blobs/in-tree for fixed column set
                            U64 row_page = blob::create_paged_dynamic(*engine.pager);
                            blob::BlobDynamicPaged row_blob(engine.pager, row_page);

                            U64 row_offset_bytes = 0;
                            auto write = [&row_offset_bytes, &row_blob](const U8* in_value, U64 size) {
                                blob::insert(row_blob, in_value, size, row_offset_bytes);
                                row_offset_bytes += size;
                            };

                            // write mask and values
                            // @perf combine loops?
                            io::write_column_mask(
                                write,
                                [&](U64 col_idx) {
                                    return static_cast<bool>(try_get_names_idx(tbl->cols[col_idx].name));
                                },
                                tbl->cols.length
                            );
                            for (const auto& col : tbl->cols) {
                                auto names_idx_opt = try_get_names_idx(col.name);
                                if (names_idx_opt) {
                                    const auto& eval = evaluate(v.values[*names_idx_opt]);
                                    io::cast_write_evaluated_as_column_value(write, eval, col.type);
                                }
                            }

                            // write pk
                            const auto& pk_eval = evaluate(v.values[*pk_idx_opt]);
                            U64 pk_key = hash(pk_eval);
                            tinsert(tbl->btree, pk_key, row_page);
                        }

                        return make_void_success();
                    } else if constexpr (SameAs<T, Insert::JsonClause>) {
                        assert_not_implemented("inserting json is not implemented");
                        return ExecutionResult{};
                    } else {
                        static_assert(!SameAs<T,T>, "missing type case");
                    }
                });
            } else if constexpr (SameAs<T, Update>) {
                assert_true_not_implemented(stmt.using_parameters.length == 0, "UPDATE USING TIMESTAMP/TTL is not implemented");
                assert_true_not_implemented(!stmt.if_, "UPDATE IF is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) return make_table_not_found(ks_name, stmt.table.table_name);

                // find partition key from WHERE clause
                Optional<Evaluated> pk_val;
                for (const auto& rel : stmt.where.relations) {
                    if (type_matches_tag<WhereClause::ColumnExpressionRelation>(rel.value)) {
                        auto& cer = get<WhereClause::ColumnExpressionRelation>(rel.value);
                        if (cer.column.identifier == tbl->cols[tbl->primary_col_idx].name && cer.operator_ == Operator::eq) {
                            pk_val = evaluate(cer.value);
                        }
                    }
                }
                assert_true(static_cast<bool>(pk_val), "UPDATE requires partition key equality in WHERE clause");

                U64 pk_key = hash(*pk_val);
                auto existing_page_opt = btree::tfind<U64>(tbl->btree, pk_key);

                // build column name-to-index mapping for assignments
                auto col_idx_for = [&](String8 name) -> Optional<U64> {
                    for (U64 i = 0; i < tbl->cols.length; i++) {
                        if (tbl->cols[i].name == name) return i;
                    }
                    return {};
                };

                if (existing_page_opt) {
                    // @todo avoid allocation by building and swapping out old with new blob
                    // read existing row
                    DynamicArray<ColumnValue> col_values;
                    DynamicArray<bool> col_present;
                    resize(col_values, tbl->cols.length);
                    resize(col_present, tbl->cols.length);
                    {
                        ColumnIterator col_it{engine.pager, tbl, *existing_page_opt};
                        ColumnIterator col_end{};
                        U64 ci = 0;
                        while (col_it != col_end && ci < tbl->cols.length) {
                            ColumnValue val = *col_it;
                            col_present[ci] = !type_matches_tag<Null>(val);
                            col_values[ci] = move(val);
                            ++col_it;
                            ++ci;
                        }
                    }

                    // apply assignments
                    for (const auto& assign : stmt.assignments) {
                        auto idx_opt = col_idx_for(assign.target.column.identifier);
                        if (!idx_opt) continue;
                        U64 idx = *idx_opt;

                        // evaluate RHS: for now only handle Constant values via pointer cast
                        // @note TermWithIdentifiers has same initial layout as Term
                        assert_true_not_implemented(!assign.target.access, "subscript/field access in UPDATE SET is not implemented");
                        Term rhs_term;
                        if (type_matches_tag<Constant>(assign.value.value)) {
                            rhs_term.value = get<Constant>(assign.value.value);
                        } else if (type_matches_tag<BindMarker>(assign.value.value)) {
                            rhs_term.value = get<BindMarker>(assign.value.value);
                        } else {
                            assert_true_not_implemented(false, "non-constant/non-bind UPDATE assignment is not implemented");
                        }

                        auto eval = evaluate(move(rhs_term));
                        if (type_matches_tag<Constant>(eval.value)) {
                            auto& c = get<Constant>(eval.value);
                            if (type_matches_tag<Null>(c.value)) {
                                col_present[idx] = false;
                            } else {
                                if (io::can_cast_write_evaluated_as_column_value(eval, tbl->cols[idx].type)) {
                                    col_present[idx] = true;
                                    // rewrite the column value
                                    U64 row_page = blob::create_paged_dynamic(*engine.pager);
                                    blob::BlobDynamicPaged tmp_blob(engine.pager, row_page);
                                    U64 tmp_offset = 0;
                                    auto w = [&tmp_offset, &tmp_blob](const U8* in_value, U64 size) {
                                        blob::insert(tmp_blob, in_value, size, tmp_offset);
                                        tmp_offset += size;
                                    };
                                    io::cast_write_evaluated_as_column_value(w, eval, tbl->cols[idx].type);
                                    // read it back as a ColumnValue
                                    tmp_offset = 0;
                                    auto r = [&tmp_offset, &tmp_blob](U8* out_value, U64 size) {
                                        blob::get(tmp_blob, out_value, size, tmp_offset);
                                        tmp_offset += size;
                                    };
                                    col_values[idx] = io::read_column_value(r, tbl->cols[idx].type);
                                }
                            }
                        }
                    }

                    // write updated row
                    btree::remove(tbl->btree, pk_key);
                    U64 new_row_page = blob::create_paged_dynamic(*engine.pager);
                    blob::BlobDynamicPaged row_blob(engine.pager, new_row_page);
                    U64 row_offset = 0;
                    auto write = [&row_offset, &row_blob](const U8* in_value, U64 size) {
                        blob::insert(row_blob, in_value, size, row_offset);
                        row_offset += size;
                    };
                    io::write_column_mask(write, [&col_present](U64 idx) { return idx < col_present.length && col_present[idx]; }, tbl->cols.length);
                    for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                        if (col_present[ci]) {
                            io::write_column_value(write, col_values[ci], tbl->cols[ci].type);
                        }
                    }
                    tinsert(tbl->btree, pk_key, new_row_page);
                } else {
                    // @todo consolidate with insert
                    // insert new row with only assigned columns + PK
                    DynamicArray<bool> col_present;
                    resize(col_present, tbl->cols.length);

                    // PK must be present
                    col_present[tbl->primary_col_idx] = true;

                    // evaluate assignments
                    DynamicArray<Evaluated> assign_evals;
                    resize(assign_evals, tbl->cols.length);

                    // set PK value
                    assign_evals[tbl->primary_col_idx] = Evaluated{get<Constant>(pk_val->value)};

                    for (const auto& assign : stmt.assignments) {
                        auto idx_opt = col_idx_for(assign.target.column.identifier);
                        if (!idx_opt) continue;
                        U64 idx = *idx_opt;
                        assert_true_not_implemented(!assign.target.access, "subscript/field access in UPDATE SET is not implemented");

                        Term rhs_term;
                        if (type_matches_tag<Constant>(assign.value.value)) {
                            rhs_term.value = get<Constant>(assign.value.value);
                        } else if (type_matches_tag<BindMarker>(assign.value.value)) {
                            rhs_term.value = get<BindMarker>(assign.value.value);
                        } else {
                            assert_true_not_implemented(false, "non-constant/non-bind UPDATE assignment is not implemented");
                        }

                        auto eval = evaluate(move(rhs_term));
                        if (type_matches_tag<Constant>(eval.value) && type_matches_tag<Null>(get<Constant>(eval.value).value)) {
                            col_present[idx] = false;
                        } else {
                            col_present[idx] = true;
                            assign_evals[idx] = move(eval);
                        }
                    }

                    U64 row_page = blob::create_paged_dynamic(*engine.pager);
                    blob::BlobDynamicPaged row_blob(engine.pager, row_page);
                    U64 row_offset = 0;
                    auto write = [&row_offset, &row_blob](const U8* in_value, U64 size) {
                        blob::insert(row_blob, in_value, size, row_offset);
                        row_offset += size;
                    };
                    io::write_column_mask(write, [&col_present](U64 idx) { return idx < col_present.length && col_present[idx]; }, tbl->cols.length);
                    for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                        if (col_present[ci]) {
                            io::cast_write_evaluated_as_column_value(write, assign_evals[ci], tbl->cols[ci].type);
                        }
                    }
                    tinsert(tbl->btree, pk_key, row_page);
                }

                return make_void_success();
            } else if constexpr (SameAs<T, Delete>) {
                assert_true_not_implemented(stmt.using_parameters.length == 0, "DELETE USING TIMESTAMP is not implemented");
                assert_true_not_implemented(!stmt.if_, "DELETE IF is not implemented");
                assert_true_not_implemented(stmt.selections.length == 0, "column-level DELETE is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) return make_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) return make_table_not_found(ks_name, stmt.table.table_name);

                // find partition key from WHERE clause
                Optional<Evaluated> pk_val;
                for (const auto& rel : stmt.where.relations) {
                    if (type_matches_tag<WhereClause::ColumnExpressionRelation>(rel.value)) {
                        auto& cer = get<WhereClause::ColumnExpressionRelation>(rel.value);
                        if (cer.column.identifier == tbl->cols[tbl->primary_col_idx].name && cer.operator_ == Operator::eq) {
                            pk_val = evaluate(cer.value);
                        }
                    }
                }
                assert_true_not_implemented(static_cast<bool>(pk_val), "DELETE for non-partition key equality in WHERE clause is not implemented");

                U64 pk_key = hash(*pk_val);

                // @perf @todo combine with where logic and use iterator to delete
                if (auto opt_row_page = btree::tfind<U64>(tbl->btree, pk_key); static_cast<bool>(opt_row_page)) {
                    blob::BlobDynamicPaged row_blob{engine.pager, *opt_row_page};
                    blob::remove(row_blob);
                    btree::remove(tbl->btree, pk_key);
                }

                return make_void_success();
            } else if constexpr (SameAs<T, AlterTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name));

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    return (stmt.if_exists) ? make_void_success() : make_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    return (stmt.if_exists) ? make_void_success() : make_table_not_found(ks_name, stmt.table.table_name);
                }

                return visit(stmt.alter_table_instruction, [&](const auto& instr) -> ExecutionResult {
                    using I = RemoveCVRef<decltype(instr)>;
                    if constexpr (SameAs<I, AlterTable::AddColumnInstruction>) {
                        for (const auto& col_def : instr.column_definitions) {
                            auto existing = schema::read_column(engine.schema, *tbl, col_def.name.identifier);
                            if (existing.error == schema::Error::None) {
                                if (instr.if_not_exists) continue;
                                return {.status = ExecutionStatus::Invalid, .message = "Column already exists"};
                            }
                            if (auto res = schema::create_column(engine.schema, *tbl, col_def); res.error != schema::Error::None) {
                                return make_server_error("Failed to add column");
                            }
                        }
                        return make_schema_changed(ks_name, stmt.table.table_name);
                    } else if constexpr (SameAs<I, AlterTable::DropColumnInstruction>) {
                        for (const auto& col_name : instr.columns) {
                            auto col_res = schema::read_column(engine.schema, *tbl, col_name.identifier);
                            if (col_res.error != schema::Error::None) {
                                if (instr.if_exists) continue;
                                return {.status = ExecutionStatus::Invalid, .message = "Column does not exist"};
                            }
                            schema::delete_column(engine.schema, *tbl, col_name.identifier);
                        }
                        return make_schema_changed(ks_name, stmt.table.table_name);
                    } else if constexpr (SameAs<I, AlterTable::RenameColumnInstruction>) {
                        assert_true_not_implemented(false, "ALTER TABLE RENAME is not implemented");
                        return ExecutionResult{};
                    } else if constexpr (SameAs<I, AlterTable::AlterColumnInstruction>) {
                        assert_true_not_implemented(false, "ALTER TABLE ALTER is not implemented");
                        return ExecutionResult{};
                    } else if constexpr (SameAs<I, Options>) {
                        assert_true_not_implemented(false, "ALTER TABLE ... WITH options are not implemented");
                        return ExecutionResult{};
                    } else {
                        static_assert(!SameAs<I,I>);
                        return ExecutionResult{};
                    }
                });
            } else if constexpr (SameAs<T, Batch>) {
                assert_not_implemented("BATCH is not implemented");
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
        auto ks = schema::read_keyspace(engine.schema, ks_name).value; // @todo propagate error
        if (ks == nullptr) return;
        auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value; // @todo propagate error
        if (tbl == nullptr) return;

        if (!type_matches_tag<Insert::NamesValues>(stmt.insert_clause)) return;
        const auto& nv = get<Insert::NamesValues>(stmt.insert_clause);

        for (U64 i = 0; i < nv.values.length; i++) {
            // @todo nested bind markers
            if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                String8 col_name = nv.names[i].identifier;
                Type col_type = make_basic(BasicType::text);
                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                    if (tbl->cols[ci].name == col_name) {
                        col_type = tbl->cols[ci].type;
                        break;
                    }
                }
                emplace_back(out, BindVariableSpec{.name = AutoString8(col_name), .type = col_type});
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
        if (bound_values.length == 0)
            return;

        visit(stmt.value, [&](auto& s) {
            using T = RemoveCVRef<decltype(s)>;
            if constexpr (SameAs<T, Insert>) {
                if (!type_matches_tag<Insert::NamesValues>(s.insert_clause))
                    return;

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

    ExecutionResult execute(Engine& engine, Statement& statement, DynamicArray<Constant>&& bound_values) {
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

                auto ks = schema::read_keyspace(engine.schema, ks_name).value; // @todo propagate error
                if (ks == nullptr) return;
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value; // @todo propagate error
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

    PreparedEntry* try_get_prepared(Engine& engine, U64 prepared_id) {
        return find(engine.prepared_cache, prepared_id);
    }

    ExecutionResult execute(Engine& engine, U64 prepared_id, DynamicArray<Constant>&& bound_values) {
        auto* entry = find(engine.prepared_cache, prepared_id);
        if (entry == nullptr) {
            return { .status = ExecutionStatus::Invalid, .message = "Prepared statement not found" };
        }

        auto cql_opt = parsers::cql::parse(String8(entry->query_string));
        if (!cql_opt) {
            return { .status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query" };
        }

        return execute(engine, *cql_opt, move(bound_values));
    }
}
