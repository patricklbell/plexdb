module;
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

module cql.engine;

import plexdb.os;
import plexdb.dynamic.tagged_union;
import plexdb.pager.transaction;

import cql.parsers;
import cql.log;
import cql.engine.evaluator;
import cql.engine.it;
import cql.engine.key;

namespace cql::engine {
    coroutine::Task<> init(Engine& engine, Pager* in_pager) {
        engine.pager = in_pager;

        U64 schema_page = in_pager->header.root_page;
        assert_true(schema_page != MAX_U64, "root page is empty, database is corrupted");
        pager::begin_transaction(*in_pager);
        co_await schema::load(engine.schema, in_pager, schema_page);
        co_await pager::commit_transaction(*in_pager);
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
    static ExecutionResult create_insert_missing_pk(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Insert is missing a value for a primary key",
            .keyspace = keyspace_name,
            .table = table_name,
        };
    }
    static ExecutionResult create_insert_missing_ck(const String8& keyspace_name, const String8& table_name) {
        return {
            .status = ExecutionStatus::Invalid,
            .message = "Insert is missing a value for a clustering key",
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

    // Returns false if the option key is not a recognized Cassandra table property.
    static bool handle_table_option_pair(const OptionPair& opt, Engine& engine) {
        String8 key = opt.first;
        if (key == "comment") {
            // metadata only, no behavioral effect
        } else if (key == "gc_grace_seconds" || key == "read_repair" || key == "speculative_retry" ||
                   key == "additional_write_policy") {
            // multi-node only: tombstone GC coordination, read-repair, speculative execution
            assert_true(engine.single_node, "multi-node table option not supported");
            log::native_info("ignoring multi-node table option (single-node no-op)");
        } else if (key == "default_time_to_live") {
            assert_not_implemented("default_time_to_live is not implemented");
        } else if (key == "bloom_filter_fp_chance" || key == "caching"    || key == "compaction" ||
                   key == "compression"             || key == "crc_check_chance"                 ||
                   key == "memtable_flush_period_in_ms"                                          ||
                   key == "min_index_interval"      || key == "max_index_interval"               ||
                   key == "extensions") {
            // known single-node-relevant tuning options — pending implementation
            assert_true(engine.single_node, "table option not supported in non-single-node mode");
            log::native_info("warning: table option not yet implemented, ignoring");
        } else {
            return false;
        }
        return true;
    }

    // Returns false if any option is unrecognized.
    static bool handle_table_options(const CreateTable::TableOptions& opts, Engine& engine) {
        bool valid = true;
        for (const auto& opt : opts.value) {
            visit(opt, [&engine, &valid](const auto& o) {
                using O = RemoveCVRef<decltype(o)>;
                if constexpr (SameAs<O, CreateTable::CompactStorage>) {
                    // deprecated legacy wire format, no data model meaning
                    assert_true(engine.single_node, "COMPACT STORAGE not supported in non-single-node mode");
                    log::native_info("ignoring COMPACT STORAGE (single-node no-op)");
                } else if constexpr (SameAs<O, CreateTable::ClusteringOrder>) {
                    // affects on-disk clustering key sort direction — needs implementation
                    log::native_info("warning: CLUSTERING ORDER BY not yet implemented, using default ASC");
                } else if constexpr (SameAs<O, OptionPair>) {
                    if (!handle_table_option_pair(o, engine)) valid = false;
                } else {
                    static_assert(!SameAs<O, O>, "unhandled table option variant");
                }
            });
            if (!valid) break;
        }
        return valid;
    }

    static coroutine::Task<ExecutionResult> execute_inside_transaction(Engine& engine, const Statement& statement) {
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

                if (!handle_table_options(stmt.options, engine))
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Unknown table option"};

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

                for (const auto& opt : stmt.options.identifier_values) {
                    String8 key = opt.first;
                    if (key == "replication" || key == "durable_writes") {
                        // multi-node: replication strategy/factor and commit-log durability
                        assert_true(engine.single_node, "ALTER KEYSPACE WITH option not supported in non-single-node mode");
                        log::native_info("ignoring keyspace option (single-node no-op)");
                    } else {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Unknown keyspace option"};
                    }
                }

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
                                if (tbl->partition_key_col_indices.length > 0 &&
                                    cer.column.identifier == tbl->cols[tbl->partition_key_col_indices[0]].name) {
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
                    DynamicArray<U8> pk_bytes = key::serialize_partition_single(*tbl, *pk_begin);
                    auto start_it = co_await create_table_eq_it(engine.pager, tbl, pk_bytes);
                    auto stop_it  = create_table_end_it(engine.pager, tbl);
                    co_return ExecutionResult{
                        .status = ExecutionStatus::Success,
                        .kind = ResultKind::Rows,
                        .keyspace = ks_name,
                        .table = stmt.from.table_name,
                        .rows = RowRange{
                            .start = move(start_it),
                            .stop  = move(stop_it),
                        },
                        .resolved_table = tbl,
                        .select_col_indices = move(select_col_indices),
                    };
                }

                if (tbl->partition_key_col_indices.length > 0) {
                    const auto& pk_col = tbl->cols[tbl->partition_key_col_indices[0]];
                    if (static_cast<bool>(pk_begin) && !io::can_cast_write_evaluated_as_column_value(*pk_begin, pk_col.type)) {
                        co_return create_where_invalid_type(ks->name, tbl->name);
                    }
                    if (static_cast<bool>(pk_end) && !io::can_cast_write_evaluated_as_column_value(*pk_end, pk_col.type)) {
                        co_return create_where_invalid_type(ks->name, tbl->name);
                    }
                }

                RowIterator start_it;
                if (static_cast<bool>(pk_begin)) {
                    DynamicArray<U8> begin_bytes = key::serialize_partition_single(*tbl, *pk_begin);
                    if (is_begin_inclusive) start_it = co_await create_table_ge_it(engine.pager, tbl, begin_bytes);
                    else                    start_it = co_await create_table_gt_it(engine.pager, tbl, begin_bytes);
                } else {
                    start_it = co_await create_table_begin_it(engine.pager, tbl);
                }

                RowIterator stop_it;
                if (static_cast<bool>(pk_end)) {
                    DynamicArray<U8> end_bytes = key::serialize_partition_single(*tbl, *pk_end);
                    if (is_end_inclusive) stop_it = co_await create_table_le_it(engine.pager, tbl, end_bytes);
                    else                  stop_it = co_await create_table_lt_it(engine.pager, tbl, end_bytes);
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
                for (const auto& param : stmt.using_parameters) {
                    if (param.kind == UpdateParameter::Kind::TIMESTAMP) {
                        assert_true(engine.single_node, "INSERT USING TIMESTAMP not supported in non-single-node mode");
                        log::native_info("ignoring INSERT USING TIMESTAMP (single-node no-op)");
                    } else if (param.kind == UpdateParameter::Kind::TTL) {
                        assert_not_implemented("INSERT USING TTL is not implemented");
                    }
                }
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

                        for (U64 pk_ci : tbl->partition_key_col_indices) {
                            if (!try_get_names_idx(tbl->cols[pk_ci].name))
                                co_return create_insert_missing_pk(ks->name, tbl->name);
                        }
                        for (U64 ck_ci : tbl->clustering_key_col_indices) {
                            if (!try_get_names_idx(tbl->cols[ck_ci].name))
                                co_return create_insert_missing_ck(ks->name, tbl->name);
                        }

                        assert_true_not_implemented(!stmt.if_not_exists, "INSERT IF NOT EXISTS is not implemented");

                        auto is_static_col = [tbl](U64 ci) -> bool {
                            for (U64 si : tbl->static_col_indices) if (si == ci) return true;
                            return false;
                        };

                        // collect regular (non-static) row bytes into buffer (sync)
                        DynamicArray<U8> row_buffer;
                        auto write = create_buffer_writer(row_buffer);

                        io::write_column_mask(
                            write,
                            [&](U64 col_idx) {
                                return static_cast<bool>(try_get_names_idx(tbl->cols[col_idx].name))
                                    && !is_static_col(col_idx);
                            },
                            tbl->cols.length
                        );
                        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                            if (!is_static_col(ci)) {
                                auto names_idx_opt = try_get_names_idx(tbl->cols[ci].name);
                                if (names_idx_opt) {
                                    const auto& eval = evaluate(v.values[*names_idx_opt]);
                                    io::cast_write_evaluated_as_column_value(write, eval, tbl->cols[ci].type);
                                }
                            }
                        }

                        // collect static column bytes into separate buffer (if any static cols present in INSERT)
                        DynamicArray<U8> static_buffer;
                        bool any_static_in_insert = false;
                        for (U64 si : tbl->static_col_indices) {
                            if (try_get_names_idx(tbl->cols[si].name)) {
                                any_static_in_insert = true;
                                break;
                            }
                        }
                        if (any_static_in_insert) {
                            auto write_static = create_buffer_writer(static_buffer);
                            io::write_column_mask(
                                write_static,
                                [&](U64 col_idx) {
                                    return is_static_col(col_idx)
                                        && static_cast<bool>(try_get_names_idx(tbl->cols[col_idx].name));
                                },
                                tbl->cols.length
                            );
                            for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                if (is_static_col(ci)) {
                                    auto names_idx_opt = try_get_names_idx(tbl->cols[ci].name);
                                    if (names_idx_opt) {
                                        const auto& eval = evaluate(v.values[*names_idx_opt]);
                                        io::cast_write_evaluated_as_column_value(write_static, eval, tbl->cols[ci].type);
                                    }
                                }
                            }
                        }

                        // write regular row buffer to new blob (async)
                        U64 row_page = co_await blob::create_paged_dynamic(*engine.pager);
                        blob::BlobDynamicPaged row_blob;
                        co_await blob::load(row_blob, engine.pager, row_page);
                        co_await blob::insert(row_blob, row_buffer.ptr, row_buffer.length);

                        // build partition key and insert into btree(s)
                        DynamicArray<Evaluated> partition_evals;
                        for (U64 pk_ci : tbl->partition_key_col_indices) {
                            push_back(partition_evals, evaluate(v.values[*try_get_names_idx(tbl->cols[pk_ci].name)]));
                        }
                        DynamicArray<U8> pk_bytes = key::serialize_partition(*tbl, partition_evals);

                        if (schema::has_clustering_keys(*tbl)) {
                            // look up or create the partition entry
                            auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);
                            schema::PartitionEntry entry;
                            bool new_partition;
                            if (entry_opt) {
                                entry = *entry_opt;
                                new_partition = false;
                            } else {
                                entry.data_page = co_await btree::create_paged(
                                    *engine.pager,
                                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}
                                );
                                entry.static_page = 0;
                                new_partition = true;
                            }

                            // create or replace static blob when static columns are present in this INSERT
                            if (any_static_in_insert) {
                                if (entry.static_page == 0) {
                                    entry.static_page = co_await blob::create_paged_dynamic(*engine.pager);
                                    blob::BlobDynamicPaged static_blob;
                                    co_await blob::load(static_blob, engine.pager, entry.static_page);
                                    co_await blob::insert(static_blob, static_buffer.ptr, static_buffer.length);
                                } else {
                                    blob::BlobDynamicPaged static_blob;
                                    co_await blob::load(static_blob, engine.pager, entry.static_page);
                                    co_await blob::remove(static_blob);
                                    entry.static_page = co_await blob::create_paged_dynamic(*engine.pager);
                                    co_await blob::load(static_blob, engine.pager, entry.static_page);
                                    co_await blob::insert(static_blob, static_buffer.ptr, static_buffer.length);
                                }
                            }

                            // persist the partition entry (insert new or update existing static_page)
                            if (new_partition) {
                                co_await btree::tinsert(tbl->btree, pk_bytes, entry);
                            } else if (any_static_in_insert) {
                                co_await btree::tupdate(tbl->btree, pk_bytes, entry);
                            }

                            schema::ClusteringBTree clustering_btree{
                                engine.pager, entry.data_page,
                                btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}
                            };
                            DynamicArray<Evaluated> clustering_evals;
                            for (U64 ck_ci : tbl->clustering_key_col_indices) {
                                push_back(clustering_evals, evaluate(v.values[*try_get_names_idx(tbl->cols[ck_ci].name)]));
                            }
                            DynamicArray<U8> ck_bytes = key::serialize_clustering(*tbl, clustering_evals);
                            co_await btree::tinsert(clustering_btree, ck_bytes, row_page);
                        } else {
                            co_await btree::tinsert(tbl->btree, pk_bytes, schema::PartitionEntry{row_page, 0});
                        }

                        co_return create_void_success();
                    } else if constexpr (SameAs<T, Insert::JsonClause>) {
                        assert_not_implemented("inserting json is not implemented");
                        co_return ExecutionResult{};
                    } else {
                        static_assert(!SameAs<T,T>, "missing type case");
                    }
                });
            } else if constexpr (SameAs<T, Update>) { ZoneScopedN("engine::update");
                for (const auto& param : stmt.using_parameters) {
                    if (param.kind == UpdateParameter::Kind::TIMESTAMP) {
                        assert_true(engine.single_node, "UPDATE USING TIMESTAMP not supported in non-single-node mode");
                        log::native_info("ignoring UPDATE USING TIMESTAMP (single-node no-op)");
                    } else if (param.kind == UpdateParameter::Kind::TTL) {
                        assert_not_implemented("UPDATE USING TTL is not implemented");
                    }
                }
                assert_true_not_implemented(!stmt.if_, "UPDATE IF is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace UPDATE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) co_return create_table_not_found(ks_name, stmt.table.table_name);

                assert_true_not_implemented(tbl->clustering_key_col_indices.length == 0, "UPDATE on table with clustering key is not implemented");

                // find partition key from WHERE clause
                Optional<Evaluated> pk_val;
                for (const auto& rel : stmt.where.relations) {
                    if (type_matches_tag<WhereClause::ColumnExpressionRelation>(rel.value)) {
                        auto& cer = get<WhereClause::ColumnExpressionRelation>(rel.value);
                        if (tbl->partition_key_col_indices.length > 0 &&
                            cer.column.identifier == tbl->cols[tbl->partition_key_col_indices[0]].name &&
                            cer.operator_ == Operator::eq) {
                            pk_val = evaluate(cer.value);
                        }
                    }
                }
                assert_true(static_cast<bool>(pk_val), "UPDATE requires partition key equality in WHERE clause");

                DynamicArray<U8> pk_bytes = key::serialize_partition_single(*tbl, *pk_val);
                auto existing_entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);

                auto col_idx_for = [&](String8 name) -> Optional<U64> {
                    for (U64 i = 0; i < tbl->cols.length; i++) {
                        if (tbl->cols[i].name == name) return i;
                    }
                    return {};
                };

                if (existing_entry_opt) {
                    auto existing_entry = *existing_entry_opt;
                    // read existing row into col_values
                    DynamicArray<ColumnValue> col_values;
                    DynamicArray<bool> col_present;
                    resize(col_values, tbl->cols.length);
                    resize(col_present, tbl->cols.length);
                    {
                        ColumnIterator col_it;
                        co_await load(col_it, engine.pager, tbl, existing_entry.data_page, existing_entry.static_page);
                        ColumnIterator col_end{};
                        U64 ci = 0;
                        while (col_it != col_end && ci < tbl->cols.length) {
                            ColumnValue val = co_await col_it.deref();
                            col_present[ci] = !type_matches_tag<Null>(val);
                            col_values[ci] = move(val);
                            co_await col_it.advance();
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
                                    DynamicArray<U8> tmp_buffer;
                                    auto w = create_buffer_writer(tmp_buffer);
                                    io::cast_write_evaluated_as_column_value(w, eval, tbl->cols[idx].type);
                                    U64 read_offset = 0;
                                    auto r = [&tmp_buffer, &read_offset](U8* out_value, U64 size) -> coroutine::Task<void> {
                                        if (out_value != nullptr)
                                            os::memory_copy(out_value, tmp_buffer.ptr + read_offset, size);
                                        read_offset += size;
                                        co_return;
                                    };
                                    col_values[idx] = co_await io::read_column_value(r, tbl->cols[idx].type);
                                }
                            }
                        } else if (type_matches_tag<ColumnValue>(eval.value)) {
                            ColumnValue& cv = get<ColumnValue>(eval.value);
                            if (type_matches_tag<Null>(cv)) {
                                col_present[idx] = false;
                            } else if (io::can_write_column_value(cv, tbl->cols[idx].type)) {
                                col_present[idx] = true;
                                col_values[idx] = move(cv);
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

                    co_await btree::remove(tbl->btree, pk_bytes);
                    U64 new_row_page = co_await blob::create_paged_dynamic(*engine.pager);
                    blob::BlobDynamicPaged row_blob;
                    co_await blob::load(row_blob, engine.pager, new_row_page);
                    co_await blob::insert(row_blob, row_buffer.ptr, row_buffer.length);
                    co_await btree::tinsert(tbl->btree, pk_bytes, schema::PartitionEntry{new_row_page, 0});
                } else {
                    // insert new row with only assigned columns + PK
                    DynamicArray<bool> col_present;
                    resize(col_present, tbl->cols.length);

                    for (U64 pk_ci : tbl->partition_key_col_indices) col_present[pk_ci] = true;

                    DynamicArray<Evaluated> assign_evals;
                    resize(assign_evals, tbl->cols.length);
                    for (U64 pk_ci : tbl->partition_key_col_indices)
                        assign_evals[pk_ci] = Evaluated{get<Constant>(pk_val->value)};

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
                    co_await btree::tinsert(tbl->btree, pk_bytes, schema::PartitionEntry{row_page, 0});
                }

                co_return create_void_success();
            } else if constexpr (SameAs<T, Delete>) { ZoneScopedN("engine::delete");
                for (const auto& param : stmt.using_parameters) {
                    if (param.kind == UpdateParameter::Kind::TIMESTAMP) {
                        assert_true(engine.single_node, "DELETE USING TIMESTAMP not supported in non-single-node mode");
                        log::native_info("ignoring DELETE USING TIMESTAMP (single-node no-op)");
                    } else if (param.kind == UpdateParameter::Kind::TTL) {
                        assert_not_implemented("DELETE USING TTL is not implemented");
                    }
                }
                assert_true_not_implemented(!stmt.if_, "DELETE IF is not implemented");
                assert_true_not_implemented(stmt.selections.length == 0, "column-level DELETE is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : engine.current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace DELETE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) co_return create_keyspace_not_found(ks_name);

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) co_return create_table_not_found(ks_name, stmt.table.table_name);

                assert_true_not_implemented(tbl->clustering_key_col_indices.length == 0, "DELETE on table with clustering key is not implemented");

                Optional<Evaluated> pk_val;
                for (const auto& rel : stmt.where.relations) {
                    if (type_matches_tag<WhereClause::ColumnExpressionRelation>(rel.value)) {
                        auto& cer = get<WhereClause::ColumnExpressionRelation>(rel.value);
                        if (tbl->partition_key_col_indices.length > 0 &&
                            cer.column.identifier == tbl->cols[tbl->partition_key_col_indices[0]].name &&
                            cer.operator_ == Operator::eq) {
                            pk_val = evaluate(cer.value);
                        }
                    }
                }
                assert_true_not_implemented(static_cast<bool>(pk_val), "DELETE for non-partition key equality in WHERE clause is not implemented");

                DynamicArray<U8> pk_bytes = key::serialize_partition_single(*tbl, *pk_val);
                if (auto opt_entry = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes); static_cast<bool>(opt_entry)) {
                    blob::BlobDynamicPaged row_blob;
                    co_await blob::load(row_blob, engine.pager, opt_entry->data_page);
                    co_await blob::remove(row_blob);
                    co_await btree::remove(tbl->btree, pk_bytes);
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
                        for (const auto& opt : instr.identifier_values) {
                            if (!handle_table_option_pair(opt, engine))
                                co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Unknown table option"};
                        }
                        co_return create_schema_changed(ks_name, stmt.table.table_name);
                    } else {
                        static_assert(!SameAs<I,I>);
                        co_return ExecutionResult{};
                    }
                });
            } else if constexpr (SameAs<T, Batch>) {
                assert_not_implemented("BATCH is not implemented");
                co_return ExecutionResult{};
            } else {
                static_assert(false, "Unhandled statement type in engine::execute_inside_transaction");
            }
        });
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, const Statement& statement) {
        pager::Transaction tx{engine.pager};
        co_await tx.begin();
        auto result = co_await execute_inside_transaction(engine, statement);
        co_await tx.commit();
        co_return result;
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

    static void bind_values_to_statement(Statement& stmt, DynamicArray<Term>& bound_values) {
        if (bound_values.length == 0) return;

        visit(stmt.value, [&](auto& s) {
            using T = RemoveCVRef<decltype(s)>;
            if constexpr (SameAs<T, Insert>) {
                if (!type_matches_tag<Insert::NamesValues>(s.insert_clause)) return;

                auto& nv = get<Insert::NamesValues>(s.insert_clause);
                U64 bind_idx = 0;
                for (U64 i = 0; i < nv.values.length && bind_idx < bound_values.length; i++) {
                    if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                        nv.values[i] = move(bound_values[bind_idx]);
                        bind_idx++;
                    }
                }
            }
        });
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, Statement& statement, DynamicArray<Term>&& bound_values) {
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

        auto cql_opt = parsers::parse(query);
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
                    if (tbl->partition_key_col_indices.length > 0 &&
                        tbl->cols[tbl->partition_key_col_indices[0]].name == entry.bind_variables[i].name) {
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

    coroutine::Task<ExecutionResult> execute(Engine& engine, U64 prepared_id, DynamicArray<Term>&& bound_values) {
        auto* entry = find(engine.prepared_cache, prepared_id);
        if (entry == nullptr) {
            co_return ExecutionResult{ .status = ExecutionStatus::Invalid, .message = "Prepared statement not found" };
        }

        auto cql_opt = parsers::parse(String8(entry->query_string));
        if (!cql_opt) {
            co_return ExecutionResult{ .status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query" };
        }

        co_return co_await execute(engine, *cql_opt, move(bound_values));
    }
}
