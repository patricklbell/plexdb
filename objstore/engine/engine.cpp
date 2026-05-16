module;
#include <coroutine>
#include <profiling/tracy.hpp>

module objstore.engine;

import plexdb.os;
import plexdb.dynamic.tagged_union;

import objstore.parsers;
import objstore.engine.evaluator;
import objstore.engine.it;

namespace objstore::engine {
    coroutine::Task<> init(Engine& engine, Pager* in_pager) {
        engine.pager = in_pager;
        co_await schema::load(engine.schema, in_pager, in_pager->header.root_page);
    }

    coroutine::Task<void> create_database(Pager& pager) {
        U64 schema_page = co_await schema::create_schema(pager);
        co_await pager::set_root(pager, schema_page);
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
            if (table == "local")     return create_system_local();
            if (table == "peers")     return create_system_peers();
            if (table == "peers_v2")  return create_system_peers_v2();
        }
        if (keyspace == "system_schema") {
            if (table == "keyspaces")        return create_schema_keyspaces(engine.schema);
            if (table == "tables")           return create_schema_tables(engine.schema);
            if (table == "columns")          return create_schema_columns(engine.schema);
            if (table == "views")            return create_schema_views(engine.schema);
            if (table == "indexes")          return create_schema_indexes(engine.schema);
            if (table == "triggers")         return create_schema_triggers(engine.schema);
            if (table == "dropped_columns")  return create_schema_dropped_columns(engine.schema);
            if (table == "types")            return create_schema_types(engine.schema);
            if (table == "functions")        return create_schema_functions(engine.schema);
            if (table == "aggregates")       return create_schema_aggregates(engine.schema);
        }
        return {};
    }

    // ========================================================================
    // execute
    // ========================================================================
    static ExecutionResult create_void_success() {
        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
    }
    // @note msg should be static storage duration
    static ExecutionResult create_server_error(const char* msg) {
        return {.status = ExecutionStatus::ServerError, .message = msg};
    }
    static ExecutionResult create_keyspace_already_exists(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::AlreadyExists,
            .message = "Keyspace already exists",
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult create_keyspace_created(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .message = "CREATED",
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult create_keyspace_not_found(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Keyspace does not exist",
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult create_use_keyspace(const String8& keyspace_name) {
        return {.status = ExecutionStatus::Success, .kind = ResultKind::UseKeyspace, .keyspace = keyspace_name};
    }
    static ExecutionResult create_table_already_exists(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::AlreadyExists,
            .message = "Table already exists",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_table_not_found(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Table does not exist",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_table_created(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .message = "CREATED",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_schema_changed(const String8& keyspace_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .keyspace = keyspace_name,
        };
    }
    static ExecutionResult create_schema_changed(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Success,
            .kind = ResultKind::SchemaChange,
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_insert_column_does_not_match_value_count(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Column count does not match value count",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_insert_into_deleted_column(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Cannot insert into deleted column",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    [[maybe_unused]] static ExecutionResult create_insert_into_unknown_column(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Too many values or unknown column",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_insert_incompatible_literal(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Incompatible literal for column type",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_insert_missing_pks(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Insert is missing a value for the primary keys",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_where_invalid_type(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Invalid type",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }

    // append bytes sync into a DynamicArray buffer — used for write-buffering before async blob writes
    static auto create_buffer_writer(DynamicArray<U8>& buf) {
        return [&buf](const U8* in_value, U64 size) {
            U64 old_len = buf.length;
            resize(buf, old_len + size);
            os::memory_copy(buf.ptr + old_len, in_value, size);
        };
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, const Statement& statement) { ZoneScopedN("engine::execute");
        co_return co_await visit(statement.value, [&engine](const auto& stmt) -> coroutine::Task<ExecutionResult> {
            using T = RemoveCVRef<decltype(stmt)>;

            if constexpr (SameAs<T, CreateKeyspace>) {
                if (is_system_keyspace(stmt.name)) {
                    co_return (stmt.if_not_exists) ? create_void_success() : create_keyspace_already_exists(stmt.name);
                }

                if (auto existing = schema::read_keyspace(engine.schema, stmt.name).value; existing != nullptr) {
                    co_return (stmt.if_not_exists) ? create_void_success() : create_keyspace_already_exists(stmt.name);
                }

                auto ks = (co_await schema::create_keyspace(engine.schema, stmt)).value;
                if (ks == nullptr) {
                    co_return create_server_error("Failed to create keyspace");
                }

                co_return create_keyspace_created(stmt.name);
            } else if constexpr (SameAs<T, CreateTable>) {
                String8 ks_name = static_cast<bool>(stmt.name.keyspace_name) ? String8(*stmt.name.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace CREATE TABLE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                if (auto existing = schema::read_table(engine.schema, *ks, stmt.name.table_name); existing.value != nullptr) {
                    co_return (stmt.if_not_exists) ? create_void_success() : create_table_already_exists(ks_name, stmt.name.table_name);
                }

                auto tbl = (co_await schema::create_table(engine.schema, *ks, stmt)).value;
                if (tbl == nullptr) {
                    co_return create_server_error("Failed to create table");
                }

                co_return create_table_created(ks_name, stmt.name.table_name);
            } else if constexpr (SameAs<T, UseKeyspace>) {
                if (is_system_keyspace(stmt.keyspace)) {
                    engine.current_keyspace = stmt.keyspace;
                    co_return create_use_keyspace(engine.current_keyspace);
                }

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value;
                if (ks == nullptr) co_return create_keyspace_not_found(stmt.keyspace);
                engine.current_keyspace = stmt.keyspace;

                co_return create_use_keyspace(engine.current_keyspace);
            } else if constexpr (SameAs<T, AlterKeyspace>) {
                assert_true_not_implemented(!is_system_keyspace(stmt.keyspace), "system keyspace ALTER KEYSPACE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(stmt.keyspace);
                }

                assert_true_not_implemented(stmt.options.identifier_values.length == 0, "ALTER KEYSPACE WITH options are not implemented");

                co_return create_schema_changed(stmt.keyspace);
            } else if constexpr (SameAs<T, DropKeyspace>) {
                assert_true_not_implemented(!is_system_keyspace(stmt.keyspace), "system keyspace DROP KEYSPACE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(stmt.keyspace);
                }

                co_await schema::delete_keyspace(engine.schema, stmt.keyspace);

                co_return create_schema_changed(stmt.keyspace);
            } else if constexpr (SameAs<T, DropTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace DROP TABLE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(ks_name);
                }

                if ((co_await schema::delete_table(engine.schema, *ks, stmt.table.table_name)).error != schema::Error::None) {
                    if (stmt.if_exists) co_return create_void_success();
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_return create_schema_changed(ks_name, stmt.table.table_name);
            } else if constexpr (SameAs<T, TruncateTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace TRUNCATE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) co_return create_table_not_found(ks_name, stmt.table.table_name);

                co_await btree::truncate(tbl->btree);

                co_return create_void_success();
            } else if constexpr (SameAs<T, Select>) { ZoneScopedN("engine::select");
                String8 ks_name = static_cast<bool>(stmt.from.keyspace_name) ? String8(*stmt.from.keyspace_name) : engine.current_keyspace;

                auto system_vr = try_system_select(engine, ks_name, stmt.from.table_name);
                if (system_vr) {
                    co_return ExecutionResult{
                        .status = ExecutionStatus::Success,
                        .kind = ResultKind::VirtualRows,
                        .keyspace = ks_name,
                        .table = stmt.from.table_name,
                        .virtual_rows = move(system_vr),
                    };
                }
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace SELECT is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.from.table_name).value;
                if (tbl == nullptr) co_return create_table_not_found(ks_name, stmt.from.table_name);

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
                    auto start_it = co_await create_table_eq_it(engine.pager, tbl, hash(*pk_begin));
                    auto stop_it  = create_table_end_it(engine.pager, tbl);
                    co_return ExecutionResult{
                        .status = ExecutionStatus::Success,
                        .kind = ResultKind::Rows,
                        .keyspace = ks_name,
                        .table = stmt.from.table_name,
                        .row_limit_count = 1,
                        .rows = RowRange{
                            .start = move(start_it),
                            .stop  = move(stop_it),
                        },
                        .resolved_table = tbl,
                        .select_col_indices = move(select_col_indices),
                    };
                }

                const auto& pk_col = tbl->cols[tbl->primary_col_idx];
                if (static_cast<bool>(pk_begin) && !io::can_cast_write_evaluated_as_column_value(*pk_begin, pk_col.type)) {
                    co_return create_where_invalid_type(ks->name, tbl->name);
                }
                if (static_cast<bool>(pk_end) && !io::can_cast_write_evaluated_as_column_value(*pk_end, pk_col.type)) {
                    co_return create_where_invalid_type(ks->name, tbl->name);
                }

                U64 hash_begin = 0, hash_end = MAX_U64;
                if (static_cast<bool>(pk_begin)) hash_begin = hash(*pk_begin);
                if (static_cast<bool>(pk_end))   hash_end   = hash(*pk_end);

                if (hash_begin > hash_end) {
                    co_return ExecutionResult{
                        .status = ExecutionStatus::Success,
                        .kind = ResultKind::Rows,
                        .keyspace = ks_name,
                        .table = stmt.from.table_name,
                    };
                }

                RowIterator start_it;
                if (static_cast<bool>(pk_begin)) {
                    if (is_begin_inclusive) start_it = co_await create_table_ge_it(engine.pager, tbl, hash_begin);
                    else                    start_it = co_await create_table_gt_it(engine.pager, tbl, hash_begin);
                } else {
                    start_it = co_await create_table_begin_it(engine.pager, tbl);
                }

                RowIterator stop_it;
                if (static_cast<bool>(pk_end)) {
                    if (is_end_inclusive) stop_it = co_await create_table_le_it(engine.pager, tbl, hash_end);
                    else                  stop_it = co_await create_table_lt_it(engine.pager, tbl, hash_end);
                } else {
                    stop_it = create_table_end_it(engine.pager, tbl);
                }

                co_return ExecutionResult{
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::Rows,
                    .keyspace = ks_name,
                    .table = stmt.from.table_name,
                    .rows = RowRange{.start = move(start_it), .stop = move(stop_it)},
                    .resolved_table = tbl,
                    .select_col_indices = move(select_col_indices),
                };
            } else if constexpr (SameAs<T, Insert>) { ZoneScopedN("engine::insert");
                assert_true_not_implemented(stmt.using_parameters.length == 0, "INSERT USING TIMESTAMP/TTL is not implemented");
                assert_true(static_cast<bool>(stmt.insert_clause), "missing insert clause, this should never happen");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace INSERT is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) co_return create_table_not_found(ks_name, stmt.table.table_name);

                co_return co_await visit(stmt.insert_clause, [&engine, ks, tbl, &stmt](const auto& v) -> coroutine::Task<ExecutionResult> {
                    using T = Decay<decltype(v)>;

                    if constexpr (SameAs<T, Insert::NamesValues>) {
                        if (v.names.length != v.values.length) co_return create_insert_column_does_not_match_value_count(ks->name, tbl->name);

                        auto try_get_names_idx = [&v](const String8& q) -> Optional<U64> {
                            for (U64 idx = 0; idx < v.names.length; idx++) {
                                if (v.names[idx].identifier == q) return idx;
                            }
                            return {};
                        };

                        // validate columns
                        for (const auto& col : tbl->cols) {
                            auto names_idx_opt = try_get_names_idx(col.name);
                            if (names_idx_opt) {
                                if (col.tombstone) co_return create_insert_into_deleted_column(ks->name, tbl->name);
                                const auto& eval = evaluate(v.values[*names_idx_opt]);
                                if (!io::can_cast_write_evaluated_as_column_value(eval, col.type))
                                    co_return create_insert_incompatible_literal(ks->name, tbl->name);
                            }
                        }

                        const auto& pk_col = tbl->cols[tbl->primary_col_idx];
                        auto pk_idx_opt = try_get_names_idx(pk_col.name);
                        if (!pk_idx_opt) co_return create_insert_missing_pks(ks->name, tbl->name);

                        assert_true_not_implemented(!stmt.if_not_exists, "INSERT IF NOT EXISTS is not implemented");

                        // collect serialized row bytes into buffer (sync)
                        DynamicArray<U8> row_buffer;
                        auto write = create_buffer_writer(row_buffer);

                        io::write_column_mask(
                            write,
                            [&](U64 col_idx) { return static_cast<bool>(try_get_names_idx(tbl->cols[col_idx].name)); },
                            tbl->cols.length
                        );
                        for (const auto& col : tbl->cols) {
                            auto names_idx_opt = try_get_names_idx(col.name);
                            if (names_idx_opt) {
                                const auto& eval = evaluate(v.values[*names_idx_opt]);
                                io::cast_write_evaluated_as_column_value(write, eval, col.type);
                            }
                        }

                        // write buffer to new blob (async)
                        U64 row_page = co_await blob::create_paged_dynamic(*engine.pager);
                        blob::BlobDynamicPaged row_blob;
                        co_await blob::load(row_blob, engine.pager, row_page);
                        co_await blob::insert(row_blob, row_buffer.ptr, row_buffer.length);

                        // insert PK → row_page into btree
                        const auto& pk_eval = evaluate(v.values[*pk_idx_opt]);
                        U64 pk_key = hash(pk_eval);
                        co_await btree::tinsert(tbl->btree, pk_key, row_page);

                        co_return create_void_success();
                    } else if constexpr (SameAs<T, Insert::JsonClause>) {
                        assert_not_implemented("inserting json is not implemented");
                        co_return ExecutionResult{};
                    } else {
                        static_assert(!SameAs<T,T>, "missing type case");
                    }
                });
            } else if constexpr (SameAs<T, Update>) { ZoneScopedN("engine::update");
                assert_true_not_implemented(stmt.using_parameters.length == 0, "UPDATE USING TIMESTAMP/TTL is not implemented");
                assert_true_not_implemented(!stmt.if_, "UPDATE IF is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace UPDATE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) co_return create_table_not_found(ks_name, stmt.table.table_name);

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
                auto existing_page_opt = co_await btree::tfind<U64>(tbl->btree, pk_key);

                auto col_idx_for = [&](String8 name) -> Optional<U64> {
                    for (U64 i = 0; i < tbl->cols.length; i++) {
                        if (tbl->cols[i].name == name) return i;
                    }
                    return {};
                };

                if (existing_page_opt) {
                    // read existing row into col_values
                    DynamicArray<ColumnValue> col_values;
                    DynamicArray<bool> col_present;
                    resize(col_values, tbl->cols.length);
                    resize(col_present, tbl->cols.length);
                    {
                        ColumnIterator col_it;
                        co_await load(col_it, engine.pager, tbl, *existing_page_opt);
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
                                    // round-trip through in-memory buffer to get ColumnValue
                                    DynamicArray<U8> tmp_buffer;
                                    auto w = create_buffer_writer(tmp_buffer);
                                    io::cast_write_evaluated_as_column_value(w, eval, tbl->cols[idx].type);
                                    U64 read_offset = 0;
                                    auto r = [&tmp_buffer, &read_offset](U8* out_value, U64 size) {
                                        os::memory_copy(out_value, tmp_buffer.ptr + read_offset, size);
                                        read_offset += size;
                                    };
                                    col_values[idx] = io::read_column_value(r, tbl->cols[idx].type);
                                }
                            }
                        }
                    }

                    // write updated row to new blob
                    DynamicArray<U8> row_buffer;
                    auto write = create_buffer_writer(row_buffer);
                    io::write_column_mask(write, [&col_present](U64 idx) { return idx < col_present.length && col_present[idx]; }, tbl->cols.length);
                    for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                        if (col_present[ci]) {
                            io::write_column_value(write, col_values[ci], tbl->cols[ci].type);
                        }
                    }

                    co_await btree::remove(tbl->btree, pk_key);
                    U64 new_row_page = co_await blob::create_paged_dynamic(*engine.pager);
                    blob::BlobDynamicPaged row_blob;
                    co_await blob::load(row_blob, engine.pager, new_row_page);
                    co_await blob::insert(row_blob, row_buffer.ptr, row_buffer.length);
                    co_await btree::tinsert(tbl->btree, pk_key, new_row_page);
                } else {
                    // insert new row with only assigned columns + PK
                    DynamicArray<bool> col_present;
                    resize(col_present, tbl->cols.length);

                    col_present[tbl->primary_col_idx] = true;

                    DynamicArray<Evaluated> assign_evals;
                    resize(assign_evals, tbl->cols.length);
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

                    DynamicArray<U8> row_buffer;
                    auto write = create_buffer_writer(row_buffer);
                    io::write_column_mask(write, [&col_present](U64 idx) { return idx < col_present.length && col_present[idx]; }, tbl->cols.length);
                    for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                        if (col_present[ci]) {
                            io::cast_write_evaluated_as_column_value(write, assign_evals[ci], tbl->cols[ci].type);
                        }
                    }

                    U64 row_page = co_await blob::create_paged_dynamic(*engine.pager);
                    blob::BlobDynamicPaged row_blob;
                    co_await blob::load(row_blob, engine.pager, row_page);
                    co_await blob::insert(row_blob, row_buffer.ptr, row_buffer.length);
                    co_await btree::tinsert(tbl->btree, pk_key, row_page);
                }

                co_return create_void_success();
            } else if constexpr (SameAs<T, Delete>) { ZoneScopedN("engine::delete");
                assert_true_not_implemented(stmt.using_parameters.length == 0, "DELETE USING TIMESTAMP is not implemented");
                assert_true_not_implemented(!stmt.if_, "DELETE IF is not implemented");
                assert_true_not_implemented(stmt.selections.length == 0, "column-level DELETE is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace DELETE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) co_return create_table_not_found(ks_name, stmt.table.table_name);

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

                if (auto opt_row_page = co_await btree::tfind<U64>(tbl->btree, pk_key); static_cast<bool>(opt_row_page)) {
                    blob::BlobDynamicPaged row_blob;
                    co_await blob::load(row_blob, engine.pager, *opt_row_page);
                    co_await blob::remove(row_blob);
                    co_await btree::remove(tbl->btree, pk_key);
                }

                co_return create_void_success();
            } else if constexpr (SameAs<T, AlterTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace ALTER TABLE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_return co_await visit(stmt.alter_table_instruction, [&engine, tbl, &ks_name, &stmt](const auto& instr) -> coroutine::Task<ExecutionResult> {
                    using I = RemoveCVRef<decltype(instr)>;
                    if constexpr (SameAs<I, AlterTable::AddColumnInstruction>) {
                        for (const auto& col_def : instr.column_definitions) {
                            auto existing = schema::read_column(engine.schema, *tbl, col_def.name.identifier);
                            if (existing.error == schema::Error::None) {
                                if (instr.if_not_exists) continue;
                                co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Column already exists"};
                            }
                            if (auto res = co_await schema::create_column(engine.schema, *tbl, col_def); res.error != schema::Error::None) {
                                co_return create_server_error("Failed to add column");
                            }
                        }
                        co_return create_schema_changed(ks_name, stmt.table.table_name);
                    } else if constexpr (SameAs<I, AlterTable::DropColumnInstruction>) {
                        for (const auto& col_name : instr.columns) {
                            auto col_res = schema::read_column(engine.schema, *tbl, col_name.identifier);
                            if (col_res.error != schema::Error::None) {
                                if (instr.if_exists) continue;
                                co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Column does not exist"};
                            }
                            co_await schema::delete_column(engine.schema, *tbl, col_name.identifier);
                        }
                        co_return create_schema_changed(ks_name, stmt.table.table_name);
                    } else if constexpr (SameAs<I, AlterTable::RenameColumnInstruction>) {
                        assert_true_not_implemented(false, "ALTER TABLE RENAME is not implemented");
                        co_return ExecutionResult{};
                    } else if constexpr (SameAs<I, AlterTable::AlterColumnInstruction>) {
                        assert_true_not_implemented(false, "ALTER TABLE ALTER is not implemented");
                        co_return ExecutionResult{};
                    } else if constexpr (SameAs<I, Options>) {
                        assert_true_not_implemented(false, "ALTER TABLE ... WITH options are not implemented");
                        co_return ExecutionResult{};
                    } else {
                        static_assert(!SameAs<I,I>);
                        co_return ExecutionResult{};
                    }
                });
            } else if constexpr (SameAs<T, Batch>) {
                assert_not_implemented("BATCH is not implemented");
                co_return ExecutionResult{};
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
        auto ks = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) return;
        auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
        if (tbl == nullptr) return;

        if (!type_matches_tag<Insert::NamesValues>(stmt.insert_clause)) return;
        const auto& nv = get<Insert::NamesValues>(stmt.insert_clause);

        for (U64 i = 0; i < nv.values.length; i++) {
            if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                String8 col_name = nv.names[i].identifier;
                Type col_type = create_basic(BasicType::text);
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
        if (bound_values.length == 0) return;

        visit(stmt.value, [&](auto& s) {
            using T = RemoveCVRef<decltype(s)>;
            if constexpr (SameAs<T, Insert>) {
                if (!type_matches_tag<Insert::NamesValues>(s.insert_clause)) return;

                auto& nv = get<Insert::NamesValues>(s.insert_clause);
                U64 bind_idx = 0;
                for (U64 i = 0; i < nv.values.length && bind_idx < bound_values.length; i++) {
                    if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                        nv.values[i].value = move(bound_values[bind_idx]);
                        bind_idx++;
                    }
                }
            }
        });
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, Statement& statement, DynamicArray<Constant>&& bound_values) {
        bind_values_to_statement(statement, bound_values);
        co_return co_await execute(engine, statement);
    }

    // ========================================================================
    // prepared statements
    // ========================================================================
    PrepareResult prepare(Engine& engine, String8 query) { ZoneScopedN("engine::prepare");
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

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) return;
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
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

    coroutine::Task<ExecutionResult> execute(Engine& engine, U64 prepared_id, DynamicArray<Constant>&& bound_values) { ZoneScopedN("engine::execute_prepared");
        auto* entry = find(engine.prepared_cache, prepared_id);
        if (entry == nullptr) {
            co_return ExecutionResult{ .status = ExecutionStatus::Invalid, .message = "Prepared statement not found" };
        }

        auto cql_opt = parsers::cql::parse(String8(entry->query_string));
        if (!cql_opt) {
            co_return ExecutionResult{ .status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query" };
        }

        co_return co_await execute(engine, *cql_opt, move(bound_values));
    }
}
