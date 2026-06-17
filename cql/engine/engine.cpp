module;
#include <coroutine>
#include <cstring>
#include <plexdb/support/tracy/tracy.hpp>

module cql.engine;

import plexdb.os;
import plexdb.btree.types;
import plexdb.dynamic.tagged_union;

import cql.parsers;
import cql.log;
import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.it;
import cql.engine.key;
import cql.engine.planner;

namespace cql::engine {
    coroutine::Task<> init(Engine& engine, Pager* in_pager) {
        engine.pager = in_pager;

        U64 schema_page = in_pager->header.root_page;
        assert_true(schema_page != MAX_U64, "root page is empty, database is corrupted");
        pager::Transaction tx{in_pager};
        co_await tx.begin();
        co_await schema::load(engine.schema, in_pager, schema_page);
        co_await tx.commit();
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
            if (table == "local") {
                return create_system_local(engine.port, engine.schema);
            }
            if (table == "peers") {
                return create_system_peers();
            }
            if (table == "peers_v2") {
                return create_system_peers_v2();
            }
        }
        if (keyspace == "system_schema") {
            if (table == "keyspaces") {
                return create_schema_keyspaces(engine.schema);
            }
            if (table == "tables") {
                return create_schema_tables(engine.schema);
            }
            if (table == "columns") {
                return create_schema_columns(engine.schema);
            }
            if (table == "views") {
                return create_schema_views(engine.schema);
            }
            if (table == "indexes") {
                return create_schema_indexes(engine.schema);
            }
            if (table == "triggers") {
                return create_schema_triggers(engine.schema);
            }
            if (table == "dropped_columns") {
                return create_schema_dropped_columns(engine.schema);
            }
            if (table == "types") {
                return create_schema_types(engine.schema);
            }
            if (table == "functions") {
                return create_schema_functions(engine.schema);
            }
            if (table == "aggregates") {
                return create_schema_aggregates(engine.schema);
            }
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
            .status   = ExecutionStatus::AlreadyExists,
            .message  = "Keyspace already exists",
            .keyspace = AutoString8(keyspace_name),
        };
    }
    static ExecutionResult create_keyspace_created(const String8& keyspace_name) {
        return {
            .status   = ExecutionStatus::Success,
            .kind     = ResultKind::SchemaChange,
            .message  = "CREATED",
            .keyspace = AutoString8(keyspace_name),
        };
    }
    static ExecutionResult create_keyspace_not_found(const String8& keyspace_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.message_storage = "Keyspace '" + r.keyspace + "' does not exist";
        r.message         = String8(r.message_storage.c_str, r.message_storage.length);
        return r;
    }
    static ExecutionResult create_use_keyspace(const String8& keyspace_name) {
        return {.status = ExecutionStatus::Success, .kind = ResultKind::UseKeyspace, .keyspace = AutoString8(keyspace_name)};
    }
    static ExecutionResult create_table_already_exists(const String8& keyspace_name, const String8& table_name) {
        return {
            .status   = ExecutionStatus::AlreadyExists,
            .message  = "Table already exists",
            .keyspace = AutoString8(keyspace_name),
            .table    = AutoString8(table_name),
        };
    }
    static ExecutionResult create_table_not_found(const String8& keyspace_name, const String8& table_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Table '" + r.keyspace + "." + r.table + "' does not exist";
        r.message         = String8(r.message_storage.c_str, r.message_storage.length);
        return r;
    }
    static ExecutionResult create_table_created(const String8& keyspace_name, const String8& table_name) {
        return {
            .status   = ExecutionStatus::Success,
            .kind     = ResultKind::SchemaChange,
            .message  = "CREATED",
            .keyspace = AutoString8(keyspace_name),
            .table    = AutoString8(table_name),
        };
    }
    static ExecutionResult create_schema_changed(const String8& keyspace_name) {
        return {
            .status   = ExecutionStatus::Success,
            .kind     = ResultKind::SchemaChange,
            .keyspace = AutoString8(keyspace_name),
        };
    }
    static ExecutionResult create_schema_changed(const String8& keyspace_name, const String8& table_name) {
        return {
            .status   = ExecutionStatus::Success,
            .kind     = ResultKind::SchemaChange,
            .keyspace = AutoString8(keyspace_name),
            .table    = AutoString8(table_name),
        };
    }
    static ExecutionResult create_insert_column_does_not_match_value_count(const String8& keyspace_name, const String8& table_name) {
        return {
            .status   = ExecutionStatus::Invalid,
            .message  = "Column count does not match value count",
            .keyspace = AutoString8(keyspace_name),
            .table    = AutoString8(table_name),
        };
    }

    static ExecutionResult create_insert_into_unknown_column(const String8& keyspace_name, const String8& table_name, const String8& col_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Undefined column name " + AutoString8(col_name);
        r.message         = String8(r.message_storage.c_str, r.message_storage.length);
        return r;
    }
    static ExecutionResult create_insert_duplicate_column(const String8& keyspace_name, const String8& table_name, const String8& col_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Multiple definitions of identifier " + AutoString8(col_name);
        r.message         = String8(r.message_storage.c_str, r.message_storage.length);
        return r;
    }
    static ExecutionResult create_insert_incompatible_literal(const String8& keyspace_name, const String8& table_name) {
        return {
            .status   = ExecutionStatus::Invalid,
            .message  = "Incompatible literal for column type",
            .keyspace = AutoString8(keyspace_name),
            .table    = AutoString8(table_name),
        };
    }
    static ExecutionResult create_insert_missing_pk(const String8& keyspace_name, const String8& table_name, const String8& col_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Missing mandatory PRIMARY KEY part " + AutoString8(col_name);
        r.message         = String8(r.message_storage.c_str, r.message_storage.length);
        return r;
    }
    static ExecutionResult create_insert_missing_ck(const String8& keyspace_name, const String8& table_name, const String8& col_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Missing mandatory PRIMARY KEY part " + AutoString8(col_name);
        r.message         = String8(r.message_storage.c_str, r.message_storage.length);
        return r;
    }
    static Optional<ExecutionResult> validate_plan(const planner::PlanResult& result) {
        switch (result.error) {
            case planner::PlanError::None:
                return {};
            case planner::PlanError::RequiresAllowFiltering:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "Cannot execute this query as it might involve data filtering and thus may have unpredictable performance. If you want to execute this query despite the performance unpredictability, use ALLOW FILTERING",
                };
            case planner::PlanError::MissingPartitionKey: {
                ExecutionResult r;
                r.status          = ExecutionStatus::Invalid;
                r.message_storage = "Some partition key parts are missing: " + result.context;
                r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::MissingClusteringKey: {
                ExecutionResult r;
                r.status          = ExecutionStatus::Invalid;
                r.message_storage = "Some clustering keys are missing: " + result.context;
                r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::StaticOnlyUpdateWithCK:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "Invalid restrictions on clustering columns since the UPDATE statement modifies only static columns",
                };
            case planner::PlanError::StaticOnlyDeleteWithCK:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "Invalid restrictions on clustering columns since the DELETE statement modifies only static columns",
                };
            case planner::PlanError::RangeDeletionOnSpecificColumns:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "Range deletions are not supported for specific columns",
                };
            case planner::PlanError::OrderByOnNonClusteringColumn: {
                ExecutionResult r;
                r.status          = ExecutionStatus::Invalid;
                r.message_storage = "Order by is currently only supported on the clustered columns of the PRIMARY KEY, got " + result.context;
                r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::ColumnNotFound: {
                ExecutionResult r;
                r.status          = ExecutionStatus::Invalid;
                r.message_storage = "Undefined column name " + result.context;
                r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::TypeMismatch:
                return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Type mismatch for key column"};
            case planner::PlanError::TokenFunctionInMutation:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "The token function cannot be used in WHERE clauses for UPDATE",
                };
            case planner::PlanError::DuplicateColumnInMutation: {
                ExecutionResult r;
                r.status          = ExecutionStatus::Invalid;
                r.message_storage = "Column " + result.context + " is assigned twice in UPDATE";
                r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::NonKeyColumnInMutationWhere: {
                ExecutionResult r;
                r.status          = ExecutionStatus::Invalid;
                r.message_storage = "Non PRIMARY KEY columns found in where clause: " + result.context;
                r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::NonEqInOnPartitionKeyMutation: {
                ExecutionResult r;
                r.status = ExecutionStatus::Invalid;
                if (result.context.length > 0) {
                    r.message_storage = AutoString8("Only EQ and IN relation are supported on the partition key (unless you use the token() function) — ") + result.context + " is not supported";
                } else {
                    r.message_storage = AutoString8("Only EQ and IN relation are supported on the partition key (unless you use the token() function)");
                }
                r.message = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::CounterOperationOnNonCounter: {
                ExecutionResult r;
                r.status          = ExecutionStatus::Invalid;
                r.message_storage = "Cannot apply counter operations on non-counter column " + result.context;
                r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                return r;
            }
            case planner::PlanError::DistinctRestrictionInvalid:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "SELECT DISTINCT with WHERE clause only supports restriction by partition key and/or static columns.",
                };
        }
        return {};
    }

    // append bytes sync into a DynamicArray buffer — used for write-buffering before async blob writes
    static auto create_buffer_writer(DynamicArray<U8>& buf) {
        return [&buf](const U8* in_value, U64 size) {
            U64 old_len = buf.length;
            resize(buf, old_len + size);
            os::memory_copy(buf.ptr + old_len, in_value, size);
        };
    }

    // Silently ignores all recognized and unknown Cassandra table properties (single-node mode).
    static void handle_table_option_pair(const OptionPair& opt, Engine& engine) {
        String8 key = opt.first;
        if (key == "comment") {
            // metadata only, no behavioral effect
        } else if (key == "gc_grace_seconds" || key == "read_repair" || key == "speculative_retry" ||
                   key == "additional_write_policy") {
            // multi-node only: tombstone GC coordination, read-repair, speculative execution
            assert_true(engine.single_node, "multi-node table option not supported");
            log::native_info("ignoring multi-node table option (single-node no-op)");
        } else if (key == "default_time_to_live") {
            // @todo TTL enforcement deferred; storage not yet implemented
            log::native_info("ignoring default_time_to_live (TTL not yet implemented)");
        } else if (key == "bloom_filter_fp_chance" || key == "caching" || key == "compaction" ||
                   key == "compression" || key == "crc_check_chance" ||
                   key == "memtable_flush_period_in_ms" ||
                   key == "min_index_interval" || key == "max_index_interval" ||
                   key == "extensions") {
            // @todo compaction strategy, compression, and bloom filter configuration
            assert_true(engine.single_node, "table option not supported in non-single-node mode");
            log::native_info("warning: table option not yet implemented, ignoring");
        } else {
            log::native_info("warning: unknown table option, ignoring");
        }
    }

    static void handle_table_options(const CreateTable::TableOptions& opts, Engine& engine) {
        for (const auto& opt : opts.value) {
            visit(opt, [&engine](const auto& o) {
                using O = RemoveCVRef<decltype(o)>;
                if constexpr (SameAs<O, CreateTable::CompactStorage>) {
                    // @todo COMPACT STORAGE affects the wire format for Cassandra 2.x compat
                    assert_true(engine.single_node, "COMPACT STORAGE not supported in non-single-node mode");
                    log::native_info("ignoring COMPACT STORAGE (single-node no-op)");
                } else if constexpr (SameAs<O, CreateTable::ClusteringOrder>) {
                    // @note consumed by schema::create_table when assigning per-CK directions.
                } else if constexpr (SameAs<O, OptionPair>) {
                    handle_table_option_pair(o, engine);
                } else {
                    static_assert(!SameAs<O, O>, "unhandled table option variant");
                }
            });
        }
    }

    // ========================================================================
    // mutation helpers
    // ========================================================================

    // Read a row blob (and optional static blob) into parallel col_values/col_present arrays.
    // If page_idx == 0 and static_page_idx == 0, leaves all entries as Null/absent.
    static coroutine::Task<void> read_row_into(Engine& engine, const schema::Table* tbl,
                                               U64 page_idx, U64 static_page_idx,
                                               DynamicArray<ColumnValue>& col_values,
                                               DynamicArray<bool>&        col_present) {
        resize(col_values, tbl->cols.length);
        resize(col_present, tbl->cols.length);
        if (page_idx == 0 && static_page_idx == 0) {
            co_return;
        }
        ColumnIterator col_it;
        co_await load(col_it, engine.pager, tbl, page_idx, static_page_idx);
        ColumnIterator col_end{};
        U64            ci = 0;
        while (col_it != col_end && ci < tbl->cols.length) {
            ColumnValue val = co_await col_it.deref();
            col_present[ci] = !type_matches_tag<Null>(val);
            col_values[ci]  = move(val);
            co_await col_it.advance();
            ++ci;
        }
    }

    // Serialise non-static columns (or all columns for non-clustering tables) from
    // col_values/col_present into a new dynamic-paged blob; returns the new page index.
    static coroutine::Task<U64> write_row_blob(Engine& engine, const schema::Table* tbl,
                                               const DynamicArray<ColumnValue>& col_values,
                                               const DynamicArray<bool>&        col_present) {
        bool             has_ck = schema::has_clustering_keys(*tbl);
        DynamicArray<U8> buf;
        auto             write_fn  = create_buffer_writer(buf);
        auto             write     = io::to_writer(write_fn);
        auto             is_active = [&](U64 idx) {
            return idx < col_present.length && col_present[idx] &&
                   !(has_ck && tbl->cols[idx].is_static);
        };
        io::write_column_mask(write, io::to_checker(is_active), tbl->cols.length);
        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
            if (is_active(ci)) {
                io::write_column_value(write, col_values[ci], tbl->cols[ci].type);
            }
        }
        U64                    page = co_await blob::create_paged_dynamic(*engine.pager);
        blob::BlobDynamicPaged b;
        co_await blob::load(b, engine.pager, page);
        co_await blob::insert(b, buf.ptr, buf.length);
        co_return page;
    }

    // Apply a MutationSpec's updates to col_values/col_present in place.
    static coroutine::Task<void> apply_updates_to_row(const schema::Table*         tbl,
                                                      DynamicArray<ColumnValue>&   col_values,
                                                      DynamicArray<bool>&          col_present,
                                                      const planner::MutationSpec& spec,
                                                      const EvalContext&           ctx) {
        for (const auto& upd : spec.updates) {
            U64              idx  = upd.col_idx;
            const Evaluated& eval = upd.new_value;
            if (type_matches_tag<ColumnValue>(eval.value)) {
                const ColumnValue& cv = get<ColumnValue>(eval.value);
                if (type_matches_tag<Null>(cv)) {
                    col_present[idx] = false;
                } else if (io::can_write_column_value(cv, tbl->cols[idx].type)) {
                    col_present[idx] = true;
                    col_values[idx]  = cv;
                }
            } else if (type_matches_tag<Constant>(eval.value) &&
                       type_matches_tag<Null>(get<Constant>(eval.value).value)) {
                col_present[idx] = false;
            } else if (io::can_cast_write_evaluated_as_column_value(eval, tbl->cols[idx].type)) {
                col_present[idx] = true;
                DynamicArray<U8> tmp;
                auto             w_fn = create_buffer_writer(tmp);
                io::cast_write_evaluated_as_column_value(io::to_writer(w_fn), eval, tbl->cols[idx].type, ctx);
                U64  off  = 0;
                auto r_fn = [&tmp, &off](U8* dst, U64 sz) -> coroutine::Task<void> {
                    if (dst) {
                        os::memory_copy(dst, tmp.ptr + off, sz);
                    }
                    off += sz;
                    co_return;
                };
                col_values[idx] = co_await io::read_column_value(io::to_reader(r_fn), tbl->cols[idx].type);
            }
        }
    }

    // Rewrite the static-column blob for a partition entry.
    // Removes the existing static blob (if any), writes a new one from col_values/col_present,
    // and updates entry.static_page in place.
    static coroutine::Task<void> rewrite_static(Engine& engine, schema::PartitionEntry& entry,
                                                const schema::Table*             tbl,
                                                const DynamicArray<ColumnValue>& col_values,
                                                const DynamicArray<bool>&        col_present) {
        if (entry.static_page != 0) {
            blob::BlobDynamicPaged old_blob;
            co_await blob::load(old_blob, engine.pager, entry.static_page);
            co_await blob::remove(old_blob);
            entry.static_page = 0;
        }

        bool any_static = false;
        for (U64 si : tbl->static_col_indices) {
            if (si < col_present.length && col_present[si]) {
                any_static = true;
                break;
            }
        }
        if (!any_static) {
            co_return;
        }

        DynamicArray<U8> buf;
        auto             write_fn  = create_buffer_writer(buf);
        auto             write     = io::to_writer(write_fn);
        auto             is_active = [&](U64 idx) {
            return idx < col_present.length && col_present[idx] && tbl->cols[idx].is_static;
        };
        io::write_column_mask(write, io::to_checker(is_active), tbl->cols.length);
        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
            if (tbl->cols[ci].is_static && ci < col_present.length && col_present[ci]) {
                io::write_column_value(write, col_values[ci], tbl->cols[ci].type);
            }
        }
        entry.static_page = co_await blob::create_paged_dynamic(*engine.pager);
        blob::BlobDynamicPaged b;
        co_await blob::load(b, engine.pager, entry.static_page);
        co_await blob::insert(b, buf.ptr, buf.length);
    }

    // @note empty old_cv/old_present means "row did not exist"; empty new_cv/new_present means "row was deleted".
    static coroutine::Task<void> update_indexes(Engine& /*engine*/, schema::Table* tbl,
                                                TArrayView<const U8, U16>        pk_bytes,
                                                TArrayView<const U8, U16>        ck_bytes,
                                                const DynamicArray<ColumnValue>& old_cv,
                                                const DynamicArray<bool>&        old_present,
                                                const DynamicArray<ColumnValue>& new_cv,
                                                const DynamicArray<bool>&        new_present) {
        for (auto& idx : tbl->indexes) {
            if (idx.tombstone) {
                continue;
            }
            U64 ci = idx.col_idx;
            if (!type_matches_tag<type::Basic>(tbl->cols[ci].type.value)) {
                continue;
            }
            type::Basic dtype   = get<type::Basic>(tbl->cols[ci].type.value);
            bool        old_has = ci < old_cv.length && old_present[ci] && !type_matches_tag<Null>(old_cv[ci]);
            bool        new_has = ci < new_cv.length && new_present[ci] && !type_matches_tag<Null>(new_cv[ci]);
            if (old_has) {
                DynamicArray<U8> old_key = key::make_full_index_key(
                    key::make_index_prefix_from_cv(old_cv[ci], dtype), pk_bytes, ck_bytes);
                auto kv = TArrayView<const U8, U16>(old_key.ptr, static_cast<U16>(old_key.length));
                co_await btree::remove(idx.btree, kv);
            }
            if (new_has) {
                DynamicArray<U8> new_key = key::make_full_index_key(
                    key::make_index_prefix_from_cv(new_cv[ci], dtype), pk_bytes, ck_bytes);
                auto kv    = TArrayView<const U8, U16>(new_key.ptr, static_cast<U16>(new_key.length));
                U8   dummy = 0;
                co_await btree::tinsert(idx.btree, kv, dummy);
            }
        }
    }

    static coroutine::Task<void> backfill_index(Engine& engine, schema::Table* tbl, schema::Index& idx) {
        if (!type_matches_tag<type::Basic>(tbl->cols[idx.col_idx].type.value)) {
            co_return;
        }
        type::Basic dtype = get<type::Basic>(tbl->cols[idx.col_idx].type.value);
        U64         ci    = idx.col_idx;

        auto part_it  = co_await btree::begin<schema::PartitionEntry>(tbl->btree);
        auto part_end = btree::end<schema::PartitionEntry>(tbl->btree);

        while (part_it != part_end) {
            auto             pk_view = part_it.key();
            DynamicArray<U8> pk_buf;
            resize(pk_buf, U64(pk_view.length));
            os::memory_copy(pk_buf.ptr, pk_view.ptr, pk_view.length);
            TArrayView<const U8, U16> pk_bytes{pk_buf.ptr, pk_view.length};

            schema::PartitionEntry entry = *part_it;

            if (schema::has_clustering_keys(*tbl)) {
                schema::ClusteringBTree ck_btree{
                    engine.pager, entry.data_page,
                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}};
                auto ck_it  = co_await btree::begin<U64>(ck_btree);
                auto ck_end = btree::end<U64>(ck_btree);
                while (ck_it != ck_end) {
                    auto             ck_view = ck_it.key();
                    DynamicArray<U8> ck_buf;
                    resize(ck_buf, U64(ck_view.length));
                    os::memory_copy(ck_buf.ptr, ck_view.ptr, ck_view.length);
                    TArrayView<const U8, U16> ck_bytes{ck_buf.ptr, ck_view.length};

                    DynamicArray<ColumnValue> cv;
                    DynamicArray<bool>        present;
                    co_await read_row_into(engine, tbl, *ck_it, entry.static_page, cv, present);

                    if (ci < cv.length && present[ci] && !type_matches_tag<Null>(cv[ci])) {
                        DynamicArray<U8> ikey = key::make_full_index_key(
                            key::make_index_prefix_from_cv(cv[ci], dtype), pk_bytes, ck_bytes);
                        auto kv    = TArrayView<const U8, U16>(ikey.ptr, static_cast<U16>(ikey.length));
                        U8   dummy = 0;
                        co_await btree::tinsert(idx.btree, kv, dummy);
                    }
                    co_await ck_it.advance();
                }
            } else {
                TArrayView<const U8, U16> ck_bytes{nullptr, 0};
                DynamicArray<ColumnValue> cv;
                DynamicArray<bool>        present;
                co_await read_row_into(engine, tbl, entry.data_page, entry.static_page, cv, present);

                if (ci < cv.length && present[ci] && !type_matches_tag<Null>(cv[ci])) {
                    DynamicArray<U8> ikey = key::make_full_index_key(
                        key::make_index_prefix_from_cv(cv[ci], dtype), pk_bytes, ck_bytes);
                    auto kv    = TArrayView<const U8, U16>(ikey.ptr, static_cast<U16>(ikey.length));
                    U8   dummy = 0;
                    co_await btree::tinsert(idx.btree, kv, dummy);
                }
            }
            co_await part_it.advance();
        }
    }

    static coroutine::Task<RowRange> create_table_range_it(Engine& engine, schema::Table* tbl,
                                                           const planner::RowLocator& locator) {
        auto copy_ck_bounds = [&](RowIterator& it) {
            it.ck_begin            = locator.ck_begin;
            it.ck_end              = locator.ck_end;
            it.ck_has_begin        = locator.ck_has_begin;
            it.ck_has_end          = locator.ck_has_end;
            it.ck_begin_inclusive  = locator.ck_begin_inclusive;
            it.ck_end_inclusive    = locator.ck_end_inclusive;
            it.ck_begin_is_partial = locator.ck_begin_is_partial;
            it.ck_end_is_partial   = locator.ck_end_is_partial;
            it.reverse_clustering  = locator.reverse_clustering;
        };

        RowIterator start_it, stop_it;
        bool        has_ck_bounds = locator.ck_has_begin || locator.ck_has_end;

        if (locator.pk_is_equality) {
            auto pk_view = TArrayView<const U8, U16>(locator.pk_begin.ptr, static_cast<U16>(locator.pk_begin.length));
            start_it     = co_await create_table_eq_it(engine.pager, tbl, pk_view);
            stop_it      = create_table_end_it(engine.pager, tbl);
            if (start_it != stop_it) {
                stop_it = co_await create_table_le_it(engine.pager, tbl, pk_view);
            }
        } else {
            if (locator.pk_has_begin) {
                auto pk_view = TArrayView<const U8, U16>(locator.pk_begin.ptr, static_cast<U16>(locator.pk_begin.length));
                if (locator.pk_begin_inclusive) {
                    start_it = co_await create_table_ge_it(engine.pager, tbl, pk_view);
                } else {
                    start_it = co_await create_table_gt_it(engine.pager, tbl, pk_view);
                }
            } else {
                start_it = co_await create_table_begin_it(engine.pager, tbl);
            }
            if (locator.pk_has_end) {
                auto pk_view = TArrayView<const U8, U16>(locator.pk_end.ptr, static_cast<U16>(locator.pk_end.length));
                if (locator.pk_end_inclusive) {
                    stop_it = co_await create_table_le_it(engine.pager, tbl, pk_view);
                } else {
                    stop_it = co_await create_table_lt_it(engine.pager, tbl, pk_view);
                }
            } else {
                stop_it = create_table_end_it(engine.pager, tbl);
            }
        }

        // reverse_clustering re-runs setup with the reverse path even when there are no
        // explicit CK bounds (clustering_it must start at the last key, not the first).
        if (has_ck_bounds || locator.reverse_clustering) {
            copy_ck_bounds(start_it);
            co_await apply_ck_bounds_on_clustering(start_it);
            co_await advance_past_empty_ck_partitions(start_it, stop_it);
        }

        co_return RowRange{move(start_it), move(stop_it)};
    }

    static int compare_ck_bytes(TArrayView<const U8, U16> a, const DynamicArray<U8>& b) {
        U64 min_len = min(U64(a.length), b.length);
        int cmp     = min_len > 0 ? os::memory_compare(a.ptr, b.ptr, min_len) : 0;
        if (cmp != 0) {
            return cmp;
        }
        return a.length < b.length ? -1 : (a.length > b.length ? 1 : 0);
    }

    // Unified mutation entry point for INSERT, UPDATE, and DELETE.
    // Precondition: locator.pk_is_equality is true.
    static coroutine::Task<void> apply_mutation(Engine& engine, schema::Table* tbl,
                                                const planner::RowLocator&   locator,
                                                const planner::MutationSpec& spec,
                                                const EvalContext&           ctx) {
        const DynamicArray<U8>& pk_bytes     = locator.pk_begin;
        bool                    have_indexes = tbl->indexes.length > 0;

        if (schema::has_clustering_keys(*tbl)) {
            auto                    entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);
            const DynamicArray<U8>& ck_bytes  = locator.ck_begin;

            if (spec.is_full_delete) {
                if (!entry_opt) {
                    co_return;
                }
                auto                    entry = *entry_opt;
                schema::ClusteringBTree ck_btree{
                    engine.pager, entry.data_page,
                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}};

                if (locator.ck_is_equality) {
                    auto row_page_opt = co_await btree::tfind<U64>(ck_btree, ck_bytes);
                    if (row_page_opt) {
                        DynamicArray<ColumnValue> old_cv;
                        DynamicArray<bool>        old_present;
                        if (have_indexes) {
                            co_await read_row_into(engine, tbl, *row_page_opt, entry.static_page, old_cv, old_present);
                        }
                        blob::BlobDynamicPaged row_blob;
                        co_await blob::load(row_blob, engine.pager, *row_page_opt);
                        co_await blob::remove(row_blob);
                        co_await btree::remove(ck_btree, ck_bytes);
                        if (have_indexes) {
                            DynamicArray<ColumnValue> empty_cv;
                            DynamicArray<bool>        empty_present;
                            auto                      pk_v = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                            auto                      ck_v = TArrayView<const U8, U16>(ck_bytes.ptr, static_cast<U16>(ck_bytes.length));
                            co_await update_indexes(engine, tbl, pk_v, ck_v, old_cv, old_present, empty_cv, empty_present);
                        }
                    }
                } else {
                    // Range or full-partition delete: collect all matching (ck_key, page) pairs.
                    struct CkEntry {
                        DynamicArray<U8> key;
                        U64              page;
                    };
                    DynamicArray<CkEntry> to_delete;

                    auto key_has_prefix = [](TArrayView<const U8, U16> key, const DynamicArray<U8>& bound) -> bool {
                        if (key.length < static_cast<U16>(bound.length)) {
                            return false;
                        }
                        return bound.length == 0 || os::memory_compare(key.ptr, bound.ptr, bound.length) == 0;
                    };

                    auto collect_range = [&](auto&& it, const auto& end_it) -> coroutine::Task<void> {
                        if (locator.ck_begin_is_partial && !locator.ck_begin_inclusive) {
                            while (it != end_it && key_has_prefix(it.key(), locator.ck_begin)) {
                                co_await it.advance();
                            }
                        }
                        while (it != end_it) {
                            auto key_view = it.key();
                            if (locator.ck_has_end) {
                                if (locator.ck_end_is_partial && locator.ck_end_inclusive) {
                                    // Partial inclusive upper bound (c1 <= X): stop when the key's first-component
                                    // bytes are strictly greater than the bound prefix.
                                    U16 B   = static_cast<U16>(locator.ck_end.length);
                                    int cmp = os::memory_compare(key_view.ptr, locator.ck_end.ptr, min(key_view.length, B));
                                    if (cmp > 0) {
                                        break;
                                    }
                                } else {
                                    int cmp = compare_ck_bytes(key_view, locator.ck_end);
                                    if (cmp > 0 || (cmp == 0 && !locator.ck_end_inclusive)) {
                                        break;
                                    }
                                }
                            }
                            CkEntry e;
                            e.page = *it;
                            resize(e.key, U64(key_view.length));
                            os::memory_copy(e.key.ptr, key_view.ptr, key_view.length);
                            push_back(to_delete, move(e));
                            co_await it.advance();
                        }
                    };

                    auto end_it = btree::end<U64>(ck_btree);
                    if (locator.ck_has_begin) {
                        auto ck_begin_view = TArrayView<const U8, U16>(locator.ck_begin.ptr, static_cast<U16>(locator.ck_begin.length));
                        if (locator.ck_begin_inclusive) {
                            auto it = co_await btree::find_it<U64, btree::SearchStrategy::FirstGreaterEqual>(ck_btree, ck_begin_view);
                            co_await collect_range(move(it), end_it);
                        } else {
                            auto it = co_await btree::find_it<U64, btree::SearchStrategy::FirstGreater>(ck_btree, ck_begin_view);
                            co_await collect_range(move(it), end_it);
                        }
                    } else {
                        auto it = co_await btree::begin<U64>(ck_btree);
                        co_await collect_range(move(it), end_it);
                    }

                    for (auto& e : to_delete) {
                        blob::BlobDynamicPaged row_blob;
                        co_await blob::load(row_blob, engine.pager, e.page);
                        co_await blob::remove(row_blob);
                        auto key_view = TArrayView<const U8, U16>(e.key.ptr, static_cast<U16>(e.key.length));
                        co_await btree::remove(ck_btree, key_view);
                    }
                }

                // Remove the partition entry if clustering tree is now empty.
                auto ck_begin_it = co_await btree::begin<U64>(ck_btree);
                if (ck_begin_it == btree::end<U64>(ck_btree)) {
                    if (entry.static_page != 0) {
                        blob::BlobDynamicPaged static_blob;
                        co_await blob::load(static_blob, engine.pager, entry.static_page);
                        co_await blob::remove(static_blob);
                    }
                    co_await btree::remove(tbl->btree, pk_bytes);
                }
            } else if (!locator.ck_is_equality) {
                // Static-only UPDATE: all SET columns are static; no CK provided or required.
                bool                   new_partition = !entry_opt;
                schema::PartitionEntry entry;
                if (entry_opt) {
                    entry = *entry_opt;
                } else {
                    entry.data_page = co_await btree::create_paged(
                        *engine.pager,
                        btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{});
                    entry.static_page = 0;
                }

                DynamicArray<ColumnValue> col_values;
                DynamicArray<bool>        col_present;
                co_await read_row_into(engine, tbl, 0, entry.static_page, col_values, col_present);
                DynamicArray<ColumnValue> old_cv      = have_indexes ? col_values : DynamicArray<ColumnValue>{};
                DynamicArray<bool>        old_present = have_indexes ? col_present : DynamicArray<bool>{};
                co_await apply_updates_to_row(tbl, col_values, col_present, spec, ctx);
                co_await rewrite_static(engine, entry, tbl, col_values, col_present);
                if (have_indexes) {
                    TArrayView<const U8, U16> empty_ck{nullptr, 0};
                    auto                      pk_v = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                    co_await update_indexes(engine, tbl, pk_v, empty_ck, old_cv, old_present, col_values, col_present);
                }

                if (new_partition && entry.static_page != 0) {
                    co_await btree::tinsert(tbl->btree, pk_bytes, entry);
                } else if (!new_partition) {
                    if (entry.static_page == 0) {
                        // @note delete partition only when both static blob and clustering rows are absent
                        schema::ClusteringBTree ck_check{
                            engine.pager, entry.data_page,
                            btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}};
                        auto ck_begin_it = co_await btree::begin<U64>(ck_check);
                        if (ck_begin_it == btree::end<U64>(ck_check)) {
                            co_await btree::remove(tbl->btree, pk_bytes);
                        } else {
                            co_await btree::tupdate(tbl->btree, pk_bytes, entry);
                        }
                    } else {
                        co_await btree::tupdate(tbl->btree, pk_bytes, entry);
                    }
                }
            } else {
                // Read-modify-write: shared path for UPDATE and column-level DELETE.
                bool                   new_partition = !entry_opt;
                schema::PartitionEntry entry;
                if (entry_opt) {
                    entry = *entry_opt;
                } else {
                    entry.data_page = co_await btree::create_paged(
                        *engine.pager,
                        btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{});
                    entry.static_page = 0;
                }

                schema::ClusteringBTree ck_btree{
                    engine.pager, entry.data_page,
                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}};
                auto row_page_opt  = co_await btree::tfind<U64>(ck_btree, ck_bytes);
                U64  existing_page = row_page_opt ? *row_page_opt : 0;

                DynamicArray<ColumnValue> col_values;
                DynamicArray<bool>        col_present;
                co_await read_row_into(engine, tbl, existing_page, entry.static_page, col_values, col_present);

                // @note PK/CK columns are not stored in the row blob — derive them from key bytes
                // so apply_updates_to_row and update_indexes see the full row image.
                {
                    auto                      pk_view = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                    DynamicArray<ColumnValue> pk_vals = key::deserialize_partition(*tbl, pk_view);
                    for (U64 i = 0; i < tbl->partition_key_col_indices.length; i++) {
                        U64 ci = tbl->partition_key_col_indices[i];
                        if (ci < col_values.length && !col_present[ci]) {
                            col_values[ci]  = move(pk_vals[i]);
                            col_present[ci] = true;
                        }
                    }
                    auto                      ck_view = TArrayView<const U8, U16>(ck_bytes.ptr, static_cast<U16>(ck_bytes.length));
                    DynamicArray<ColumnValue> ck_vals = key::deserialize_clustering(*tbl, ck_view);
                    for (U64 i = 0; i < ck_vals.length; i++) {
                        U64 ci = tbl->clustering_key_col_indices[i];
                        if (ci < col_values.length && !col_present[ci]) {
                            col_values[ci]  = move(ck_vals[i]);
                            col_present[ci] = true;
                        }
                    }
                }

                DynamicArray<ColumnValue> old_cv      = have_indexes ? col_values : DynamicArray<ColumnValue>{};
                DynamicArray<bool>        old_present = have_indexes ? col_present : DynamicArray<bool>{};
                co_await apply_updates_to_row(tbl, col_values, col_present, spec, ctx);

                bool any_static_updated = false;
                for (const auto& upd : spec.updates) {
                    if (upd.col_idx < tbl->cols.length && tbl->cols[upd.col_idx].is_static) {
                        any_static_updated = true;
                        break;
                    }
                }

                U64 new_row_page = co_await write_row_blob(engine, tbl, col_values, col_present);
                if (row_page_opt) {
                    blob::BlobDynamicPaged old_blob;
                    co_await blob::load(old_blob, engine.pager, existing_page);
                    co_await blob::remove(old_blob);
                    co_await btree::remove(ck_btree, ck_bytes);
                }
                co_await btree::tinsert(ck_btree, ck_bytes, new_row_page);

                if (any_static_updated) {
                    co_await rewrite_static(engine, entry, tbl, col_values, col_present);
                }
                if (new_partition) {
                    co_await btree::tinsert(tbl->btree, pk_bytes, entry);
                } else if (any_static_updated) {
                    co_await btree::tupdate(tbl->btree, pk_bytes, entry);
                }

                if (have_indexes) {
                    auto pk_v = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                    auto ck_v = TArrayView<const U8, U16>(ck_bytes.ptr, static_cast<U16>(ck_bytes.length));
                    co_await update_indexes(engine, tbl, pk_v, ck_v, old_cv, old_present, col_values, col_present);
                }
            }
        } else {
            // Non-clustering table: data_page is the row blob directly.
            auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);

            if (spec.is_full_delete) {
                if (entry_opt) {
                    DynamicArray<ColumnValue> old_cv;
                    DynamicArray<bool>        old_present;
                    if (have_indexes) {
                        co_await read_row_into(engine, tbl, entry_opt->data_page, entry_opt->static_page, old_cv, old_present);
                    }
                    blob::BlobDynamicPaged row_blob;
                    co_await blob::load(row_blob, engine.pager, entry_opt->data_page);
                    co_await blob::remove(row_blob);
                    co_await btree::remove(tbl->btree, pk_bytes);
                    if (have_indexes) {
                        DynamicArray<ColumnValue> empty_cv;
                        DynamicArray<bool>        empty_present;
                        TArrayView<const U8, U16> empty_ck{nullptr, 0};
                        auto                      pk_v = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                        co_await update_indexes(engine, tbl, pk_v, empty_ck, old_cv, old_present, empty_cv, empty_present);
                    }
                }
            } else {
                schema::PartitionEntry entry = entry_opt ? *entry_opt : schema::PartitionEntry{0, 0};

                DynamicArray<ColumnValue> col_values;
                DynamicArray<bool>        col_present;
                co_await read_row_into(engine, tbl, entry.data_page, entry.static_page, col_values, col_present);

                {
                    auto                      pk_view = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                    DynamicArray<ColumnValue> pk_vals = key::deserialize_partition(*tbl, pk_view);
                    for (U64 i = 0; i < tbl->partition_key_col_indices.length; i++) {
                        U64 ci = tbl->partition_key_col_indices[i];
                        if (ci < col_values.length && !col_present[ci]) {
                            col_values[ci]  = move(pk_vals[i]);
                            col_present[ci] = true;
                        }
                    }
                }

                DynamicArray<ColumnValue> old_cv      = have_indexes ? col_values : DynamicArray<ColumnValue>{};
                DynamicArray<bool>        old_present = have_indexes ? col_present : DynamicArray<bool>{};
                co_await apply_updates_to_row(tbl, col_values, col_present, spec, ctx);

                U64 new_row_page = co_await write_row_blob(engine, tbl, col_values, col_present);
                if (entry_opt) {
                    blob::BlobDynamicPaged old_blob;
                    co_await blob::load(old_blob, engine.pager, entry.data_page);
                    co_await blob::remove(old_blob);
                    co_await btree::remove(tbl->btree, pk_bytes);
                }
                co_await btree::tinsert(tbl->btree, pk_bytes, schema::PartitionEntry{new_row_page, entry.static_page});

                if (have_indexes) {
                    TArrayView<const U8, U16> empty_ck{nullptr, 0};
                    auto                      pk_v = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                    co_await update_indexes(engine, tbl, pk_v, empty_ck, old_cv, old_present, col_values, col_present);
                }
            }
        }
    }

    static coroutine::Task<ExecutionResult> execute_inside_transaction(Engine& engine, const Statement& statement, EvalContext ctx, AutoString8& current_keyspace) {
        co_return co_await visit(statement.value, [&engine, ctx, &current_keyspace](const auto& stmt) -> coroutine::Task<ExecutionResult> {
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
                String8 ks_name = static_cast<bool>(stmt.name.keyspace_name) ? String8(*stmt.name.keyspace_name) : current_keyspace;
                if (is_system_keyspace(ks_name)) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "system keyspaces cannot be modified"};
                }

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }

                if (auto existing = schema::read_table(engine.schema, *ks, stmt.name.table_name); existing.value != nullptr) {
                    co_return (stmt.if_not_exists) ? create_void_success() : create_table_already_exists(ks_name, stmt.name.table_name);
                }

                handle_table_options(stmt.options, engine);

                auto tbl_res = co_await schema::create_table(engine.schema, *ks, stmt);
                if (tbl_res.value == nullptr) {
                    if (tbl_res.error == schema::Error::InvalidOptions) {
                        ExecutionResult r;
                        r.status          = ExecutionStatus::Invalid;
                        r.message_storage = AutoString8(tbl_res.message);
                        r.message         = String8(r.message_storage.c_str, r.message_storage.length);
                        co_return r;
                    }
                    co_return create_server_error("Failed to create table");
                }

                co_return create_table_created(ks_name, stmt.name.table_name);
            } else if constexpr (SameAs<T, UseKeyspace>) {
                if (is_system_keyspace(stmt.keyspace)) {
                    current_keyspace = stmt.keyspace;
                    co_return create_use_keyspace(current_keyspace);
                }

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(stmt.keyspace);
                }
                current_keyspace = stmt.keyspace;

                co_return create_use_keyspace(current_keyspace);
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
                        log::native_info("warning: unknown keyspace option, ignoring");
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
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace DROP TABLE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(ks_name);
                }

                if ((co_await schema::delete_table(engine.schema, *ks, stmt.table.table_name)).error != schema::Error::None) {
                    if (stmt.if_exists) {
                        co_return create_void_success();
                    }
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_return create_schema_changed(ks_name, stmt.table.table_name);
            } else if constexpr (SameAs<T, TruncateTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace TRUNCATE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_await btree::truncate(tbl->btree);

                co_return create_void_success();
            } else if constexpr (SameAs<T, Select>) {
                ZoneScopedN("engine::select");
                String8 ks_name = static_cast<bool>(stmt.from.keyspace_name) ? String8(*stmt.from.keyspace_name) : current_keyspace;

                auto system_vr = try_system_select(engine, ks_name, stmt.from.table_name);
                if (system_vr) {
                    // @note text-column equality is the only WHERE shape applied here;
                    // unsupported predicates are silently ignored.
                    if (stmt.where) {
                        auto matches_row = [&](const VirtualRow& row) -> bool {
                            for (const auto& rel : stmt.where->relations) {
                                bool ok = visit(rel.value, [&](const auto& r) -> bool {
                                    using RT = RemoveCVRef<decltype(r)>;
                                    if constexpr (SameAs<RT, WhereClause::ColumnExpressionRelation>) {
                                        if (r.operator_ != Operator::eq) {
                                            return true;
                                        }
                                        U64 col_idx = MAX_U64;
                                        for (U64 i = 0; i < system_vr->columns.length; i++) {
                                            if (system_vr->columns[i].name == String8(r.column.identifier.c_str, r.column.identifier.length)) {
                                                col_idx = i;
                                                break;
                                            }
                                        }
                                        if (col_idx == MAX_U64) {
                                            return true;
                                        }
                                        Evaluated eval = evaluate(r.value, ctx);
                                        if (!type_matches_tag<Constant>(eval.value)) {
                                            return true;
                                        }
                                        const auto& con = get<Constant>(eval.value);
                                        if (col_idx >= row.values.length) {
                                            return false;
                                        }
                                        const ColumnValue& cv = row.values[col_idx];
                                        if (type_matches_tag<AutoString8>(cv) && type_matches_tag<AutoString8>(con.value)) {
                                            return String8(get<AutoString8>(cv)) == String8(get<AutoString8>(con.value));
                                        }
                                        return true;
                                    } else {
                                        return true;
                                    }
                                });
                                if (!ok) {
                                    return false;
                                }
                            }
                            return true;
                        };
                        DynamicArray<VirtualRow> kept;
                        for (auto& row : system_vr->rows) {
                            if (matches_row(row)) {
                                push_back(kept, move(row));
                            }
                        }
                        system_vr->rows = move(kept);
                    }
                    co_return ExecutionResult{
                        .status       = ExecutionStatus::Success,
                        .kind         = ResultKind::VirtualRows,
                        .keyspace     = AutoString8(ks_name),
                        .table        = AutoString8(stmt.from.table_name),
                        .virtual_rows = move(system_vr),
                    };
                }
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace SELECT is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(engine.schema, *ks, stmt.from.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.from.table_name);
                }

                if (stmt.transform && *stmt.transform == Select::Transform::JSON) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "SELECT JSON is not supported"};
                }
                assert_true_not_implemented(!stmt.group_by, "GROUP BY is not implemented");
                assert_true_not_implemented(!stmt.per_partition_limit.value, "PER PARTITION LIMIT is not implemented");

                U64 limit_count = MAX_U64;
                if (type_matches_tag<S64>(stmt.limit.value)) {
                    limit_count = U64(get<S64>(stmt.limit.value));
                }

                planner::SelectPlan sp = planner::plan_select(stmt, *tbl, ctx);
                if (auto err = validate_plan(sp.result)) {
                    co_return move(*err);
                }
                assert_true_not_implemented(!sp.projection.is_aggregate, "aggregate SELECT (COUNT(*), etc.) is not implemented");

                // Extract column indices for native layer from projection ops.
                DynamicArray<U64> select_col_indices;
                for (const auto& op : sp.projection.ops) {
                    if (type_matches_tag<planner::SelectOp::ColumnRef>(op.value)) {
                        push_back(select_col_indices, get<planner::SelectOp::ColumnRef>(op.value).col_idx);
                    }
                }

                if (sp.locator.index_col_idx) {
                    schema::Index* active_idx = nullptr;
                    for (auto& idx : tbl->indexes) {
                        if (!idx.tombstone && idx.col_idx == *sp.locator.index_col_idx) {
                            active_idx = &idx;
                            break;
                        }
                    }
                    if (active_idx == nullptr) {
                        co_return create_server_error("secondary index not found");
                    }

                    type::Basic dtype = get<type::Basic>(tbl->cols[active_idx->col_idx].type.value);

                    DynamicArray<U64> col_order;
                    if (select_col_indices.length > 0) {
                        col_order = select_col_indices;
                    } else {
                        for (U64 ci : tbl->partition_key_col_indices) {
                            push_back(col_order, ci);
                        }
                        for (U64 ci : tbl->clustering_key_col_indices) {
                            push_back(col_order, ci);
                        }
                        for (U64 ci : tbl->static_col_indices) {
                            push_back(col_order, ci);
                        }
                        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                            if (!tbl->cols[ci].tombstone &&
                                tbl->cols[ci].key_kind == schema::KeyKind::None &&
                                !tbl->cols[ci].is_static) {
                                push_back(col_order, ci);
                            }
                        }
                    }

                    VirtualRows vr;
                    vr.keyspace = AutoString8(ks_name);
                    vr.table    = AutoString8(stmt.from.table_name);
                    for (U64 ci : col_order) {
                        push_back(vr.columns, VirtualColumn{tbl->cols[ci].name, tbl->cols[ci].type});
                    }

                    const DynamicArray<U8>& prefix      = sp.locator.index_key_prefix;
                    auto                    prefix_view = TArrayView<const U8, U16>(prefix.ptr, static_cast<U16>(prefix.length));
                    auto                    idx_it      = co_await btree::find_it<U8, btree::SearchStrategy::FirstGreaterEqual>(
                        active_idx->btree, prefix_view);
                    auto idx_end = btree::end<U8>(active_idx->btree);

                    U64 row_count = 0;
                    while (idx_it != idx_end && row_count < limit_count) {
                        auto key_view = idx_it.key();
                        if (key_view.length < prefix_view.length ||
                            os::memory_compare(key_view.ptr, prefix_view.ptr, prefix_view.length) != 0) {
                            break;
                        }

                        U16                       plen   = key::index_prefix_len(dtype, key_view.ptr);
                        U16                       pk_len = static_cast<U16>((U16(key_view.ptr[plen]) << 8) | U16(key_view.ptr[plen + 1]));
                        TArrayView<const U8, U16> pk_view{key_view.ptr + plen + 2, pk_len};
                        U16                       ck_start = static_cast<U16>(plen + 2 + pk_len);
                        U16                       ck_len   = key_view.length > ck_start ? static_cast<U16>(key_view.length - ck_start) : 0;
                        TArrayView<const U8, U16> ck_view{key_view.ptr + ck_start, ck_len};

                        // @note copy pk/ck before iterator.advance() invalidates key_view
                        DynamicArray<U8> pk_buf, ck_buf;
                        resize(pk_buf, U64(pk_len));
                        os::memory_copy(pk_buf.ptr, pk_view.ptr, pk_len);
                        if (ck_len > 0) {
                            resize(ck_buf, U64(ck_len));
                            os::memory_copy(ck_buf.ptr, ck_view.ptr, ck_len);
                        }

                        auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_buf);
                        if (entry_opt) {
                            U64 row_page = 0;
                            if (schema::has_clustering_keys(*tbl) && ck_len > 0) {
                                schema::ClusteringBTree ck_btree{
                                    engine.pager, entry_opt->data_page,
                                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}};
                                auto rp = co_await btree::tfind<U64>(ck_btree, ck_buf);
                                if (rp) {
                                    row_page = *rp;
                                }
                            } else {
                                row_page = entry_opt->data_page;
                            }

                            if (row_page != 0 || entry_opt->static_page != 0) {
                                DynamicArray<ColumnValue> cv;
                                DynamicArray<bool>        present;
                                co_await read_row_into(engine, tbl, row_page, entry_opt->static_page, cv, present);

                                TArrayView<const U8, U16> pk_v{pk_buf.ptr, pk_len};
                                DynamicArray<ColumnValue> pk_vals = key::deserialize_partition(*tbl, pk_v);
                                for (U64 i = 0; i < tbl->partition_key_col_indices.length; i++) {
                                    U64 ci = tbl->partition_key_col_indices[i];
                                    if (ci < cv.length && !present[ci]) {
                                        cv[ci]      = move(pk_vals[i]);
                                        present[ci] = true;
                                    }
                                }

                                EvalContext row_ctx = ctx;
                                row_ctx.table       = tbl;
                                row_ctx.row_values  = cv.ptr;
                                if (evaluate_where(sp.filter.predicates, row_ctx)) {
                                    VirtualRow vrow;
                                    resize(vrow.values, col_order.length);
                                    for (U64 i = 0; i < col_order.length; i++) {
                                        U64 ci = col_order[i];
                                        if (ci < cv.length && present[ci]) {
                                            vrow.values[i] = cv[ci];
                                        }
                                    }
                                    push_back(vr.rows, move(vrow));
                                    row_count++;
                                }
                            }
                        }
                        co_await idx_it.advance();
                    }

                    co_return ExecutionResult{
                        .status       = ExecutionStatus::Success,
                        .kind         = ResultKind::VirtualRows,
                        .keyspace     = AutoString8(ks_name),
                        .table        = AutoString8(stmt.from.table_name),
                        .virtual_rows = move(vr),
                    };
                }

                // ORDER BY + PK IN: multi-partition merge. Per-partition iteration alone
                // returns rows partition-by-partition; sorting by CK bytes globally produces
                // the merged stream the test expects.
                if (stmt.order_by && sp.locator.pk_in_values.length > 0) {
                    DynamicArray<U64> col_order;
                    if (select_col_indices.length > 0) {
                        col_order = select_col_indices;
                    } else {
                        for (U64 ci : tbl->partition_key_col_indices) {
                            push_back(col_order, ci);
                        }
                        for (U64 ci : tbl->clustering_key_col_indices) {
                            push_back(col_order, ci);
                        }
                        for (U64 ci : tbl->static_col_indices) {
                            push_back(col_order, ci);
                        }
                        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                            if (!tbl->cols[ci].tombstone &&
                                tbl->cols[ci].key_kind == schema::KeyKind::None &&
                                !tbl->cols[ci].is_static) {
                                push_back(col_order, ci);
                            }
                        }
                    }

                    VirtualRows vr;
                    vr.keyspace = AutoString8(ks_name);
                    vr.table    = AutoString8(stmt.from.table_name);
                    for (U64 ci : col_order) {
                        push_back(vr.columns, VirtualColumn{tbl->cols[ci].name, tbl->cols[ci].type});
                    }

                    // Collect (ck_bytes, projected_values) for each row across all partitions.
                    struct CollectedRow {
                        DynamicArray<U8>          ck_bytes;
                        DynamicArray<ColumnValue> values;
                    };
                    DynamicArray<CollectedRow> collected;

                    for (const auto& pk_bytes : sp.locator.pk_in_values) {
                        auto pk_v      = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                        auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_v);
                        if (!entry_opt) {
                            continue;
                        }

                        DynamicArray<ColumnValue> pk_vals = key::deserialize_partition(*tbl, pk_v);

                        if (schema::has_clustering_keys(*tbl)) {
                            schema::ClusteringBTree ck_btree{
                                engine.pager, entry_opt->data_page,
                                btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}};
                            auto ck_it  = co_await btree::begin<U64>(ck_btree);
                            auto ck_end = btree::end<U64>(ck_btree);
                            while (ck_it != ck_end) {
                                auto             ck_view = ck_it.key();
                                DynamicArray<U8> ck_buf;
                                resize(ck_buf, U64(ck_view.length));
                                os::memory_copy(ck_buf.ptr, ck_view.ptr, ck_view.length);

                                DynamicArray<ColumnValue> cv;
                                DynamicArray<bool>        present;
                                co_await read_row_into(engine, tbl, *ck_it, entry_opt->static_page, cv, present);
                                for (U64 i = 0; i < tbl->partition_key_col_indices.length; i++) {
                                    U64 ci = tbl->partition_key_col_indices[i];
                                    if (ci < cv.length && !present[ci]) {
                                        cv[ci]      = pk_vals[i];
                                        present[ci] = true;
                                    }
                                }
                                DynamicArray<ColumnValue> ck_vals = key::deserialize_clustering(*tbl, TArrayView<const U8, U16>(ck_buf.ptr, static_cast<U16>(ck_buf.length)));
                                for (U64 i = 0; i < tbl->clustering_key_col_indices.length && i < ck_vals.length; i++) {
                                    U64 ci = tbl->clustering_key_col_indices[i];
                                    if (ci < cv.length && !present[ci]) {
                                        cv[ci]      = move(ck_vals[i]);
                                        present[ci] = true;
                                    }
                                }

                                EvalContext row_ctx = ctx;
                                row_ctx.table       = tbl;
                                row_ctx.row_values  = cv.ptr;
                                if (evaluate_where(sp.filter.predicates, row_ctx)) {
                                    CollectedRow cr;
                                    cr.ck_bytes = move(ck_buf);
                                    resize(cr.values, col_order.length);
                                    for (U64 i = 0; i < col_order.length; i++) {
                                        U64 ci = col_order[i];
                                        if (ci < cv.length && present[ci]) {
                                            cr.values[i] = cv[ci];
                                        }
                                    }
                                    push_back(collected, move(cr));
                                }
                                co_await ck_it.advance();
                            }
                        } else if (entry_opt->data_page != 0 || entry_opt->static_page != 0) {
                            DynamicArray<ColumnValue> cv;
                            DynamicArray<bool>        present;
                            co_await read_row_into(engine, tbl, entry_opt->data_page, entry_opt->static_page, cv, present);
                            for (U64 i = 0; i < tbl->partition_key_col_indices.length; i++) {
                                U64 ci = tbl->partition_key_col_indices[i];
                                if (ci < cv.length && !present[ci]) {
                                    cv[ci]      = pk_vals[i];
                                    present[ci] = true;
                                }
                            }
                            EvalContext row_ctx = ctx;
                            row_ctx.table       = tbl;
                            row_ctx.row_values  = cv.ptr;
                            if (evaluate_where(sp.filter.predicates, row_ctx)) {
                                CollectedRow cr;
                                resize(cr.values, col_order.length);
                                for (U64 i = 0; i < col_order.length; i++) {
                                    U64 ci = col_order[i];
                                    if (ci < cv.length && present[ci]) {
                                        cr.values[i] = cv[ci];
                                    }
                                }
                                push_back(collected, move(cr));
                            }
                        }
                    }

                    // Sort by CK bytes. Reverse direction flips comparison.
                    bool reverse = sp.locator.reverse_clustering;
                    for (U64 i = 1; i < collected.length; i++) {
                        for (U64 j = i; j > 0; j--) {
                            const auto& a   = collected[j - 1].ck_bytes;
                            const auto& b   = collected[j].ck_bytes;
                            U64         ml  = min(a.length, b.length);
                            int         cmp = ml > 0 ? os::memory_compare(a.ptr, b.ptr, ml) : 0;
                            if (cmp == 0) {
                                cmp = (a.length < b.length) ? -1 : (a.length > b.length ? 1 : 0);
                            }
                            bool swap = reverse ? (cmp < 0) : (cmp > 0);
                            if (!swap) {
                                break;
                            }
                            auto tmp         = move(collected[j - 1]);
                            collected[j - 1] = move(collected[j]);
                            collected[j]     = move(tmp);
                        }
                    }

                    U64 cap = collected.length < limit_count ? collected.length : limit_count;
                    for (U64 i = 0; i < cap; i++) {
                        VirtualRow vrow;
                        vrow.values = move(collected[i].values);
                        push_back(vr.rows, move(vrow));
                    }

                    co_return ExecutionResult{
                        .status       = ExecutionStatus::Success,
                        .kind         = ResultKind::VirtualRows,
                        .keyspace     = AutoString8(ks_name),
                        .table        = AutoString8(stmt.from.table_name),
                        .virtual_rows = move(vr),
                    };
                }

                auto row_range = co_await create_table_range_it(engine, tbl, sp.locator);
                co_return ExecutionResult{
                    .status             = ExecutionStatus::Success,
                    .kind               = ResultKind::Rows,
                    .keyspace           = AutoString8(ks_name),
                    .table              = AutoString8(stmt.from.table_name),
                    .row_limit_count    = limit_count,
                    .is_distinct        = static_cast<bool>(stmt.transform),
                    .rows               = move(row_range),
                    .resolved_table     = tbl,
                    .select_col_indices = move(select_col_indices),
                    .filter_predicates  = move(sp.filter.predicates),
                    .filter_ctx         = ctx,
                };
            } else if constexpr (SameAs<T, Insert>) {
                ZoneScopedN("engine::insert");
                for (const auto& param : stmt.using_parameters) {
                    if (param.kind == UpdateParameter::Kind::TIMESTAMP) {
                        assert_true(engine.single_node, "INSERT USING TIMESTAMP not supported in non-single-node mode");
                        log::native_info("ignoring INSERT USING TIMESTAMP (single-node no-op)");
                    } else if (param.kind == UpdateParameter::Kind::TTL) {
                        // @todo TTL enforcement requires row blob metadata header
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "INSERT USING TTL is not implemented"};
                    }
                }
                assert_true(static_cast<bool>(stmt.insert_clause), "missing insert clause, this should never happen");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace INSERT is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_return co_await visit(stmt.insert_clause, [&engine, ks, tbl, &stmt, ctx](const auto& v) -> coroutine::Task<ExecutionResult> {
                    using T = Decay<decltype(v)>;

                    if constexpr (SameAs<T, Insert::NamesValues>) {
                        if (v.names.length != v.values.length) {
                            co_return create_insert_column_does_not_match_value_count(ks->name, tbl->name);
                        }

                        auto try_get_names_idx = [&v](const String8& q) -> Optional<U64> {
                            for (U64 idx = 0; idx < v.names.length; idx++) {
                                if (v.names[idx].identifier == q) {
                                    return idx;
                                }
                            }
                            return {};
                        };

                        for (U64 ni = 0; ni < v.names.length; ni++) {
                            bool found = false;
                            for (const auto& col : tbl->cols) {
                                if (!col.tombstone && col.name == v.names[ni].identifier) {
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                co_return create_insert_into_unknown_column(ks->name, tbl->name, v.names[ni].identifier);
                            }
                            for (U64 nj = 0; nj < ni; nj++) {
                                if (v.names[nj].identifier == v.names[ni].identifier) {
                                    co_return create_insert_duplicate_column(ks->name, tbl->name, v.names[ni].identifier);
                                }
                            }
                        }

                        for (const auto& col : tbl->cols) {
                            auto names_idx_opt = try_get_names_idx(col.name);
                            if (names_idx_opt) {
                                if (col.tombstone) {
                                    continue; // shadowed by re-added column later in the list
                                }
                                const auto& eval = evaluate(v.values[*names_idx_opt], ctx);
                                if (!io::can_cast_write_evaluated_as_column_value(eval, col.type)) {
                                    co_return create_insert_incompatible_literal(ks->name, tbl->name);
                                }
                            }
                        }

                        for (U64 pk_ci : tbl->partition_key_col_indices) {
                            if (!try_get_names_idx(tbl->cols[pk_ci].name)) {
                                co_return create_insert_missing_pk(ks->name, tbl->name, tbl->cols[pk_ci].name);
                            }
                        }

                        auto is_static_col = [tbl](U64 ci) -> bool {
                            for (U64 si : tbl->static_col_indices) {
                                if (si == ci) {
                                    return true;
                                }
                            }
                            return false;
                        };
                        auto is_pk_col = [tbl](U64 ci) -> bool {
                            for (U64 pk_ci : tbl->partition_key_col_indices) {
                                if (pk_ci == ci) {
                                    return true;
                                }
                            }
                            return false;
                        };

                        bool needs_clustering_row = false;
                        if (schema::has_clustering_keys(*tbl)) {
                            for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                if (!is_pk_col(ci) && !is_static_col(ci) && try_get_names_idx(tbl->cols[ci].name)) {
                                    needs_clustering_row = true;
                                    break;
                                }
                            }
                            if (needs_clustering_row) {
                                for (U64 ck_ci : tbl->clustering_key_col_indices) {
                                    if (!try_get_names_idx(tbl->cols[ck_ci].name)) {
                                        co_return create_insert_missing_ck(ks->name, tbl->name, tbl->cols[ck_ci].name);
                                    }
                                }
                            }
                        }

                        assert_true_not_implemented(!stmt.if_not_exists, "INSERT IF NOT EXISTS is not implemented");

                        // collect regular (non-static) row bytes into buffer (sync)
                        DynamicArray<U8> row_buffer;
                        auto             write_fn      = create_buffer_writer(row_buffer);
                        auto             write         = io::to_writer(write_fn);
                        auto             row_is_active = [&](U64 col_idx) {
                            return !tbl->cols[col_idx].tombstone && static_cast<bool>(try_get_names_idx(tbl->cols[col_idx].name)) && !is_static_col(col_idx);
                        };
                        io::write_column_mask(write, io::to_checker(row_is_active), tbl->cols.length);
                        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                            if (!is_static_col(ci) && !tbl->cols[ci].tombstone) {
                                auto names_idx_opt = try_get_names_idx(tbl->cols[ci].name);
                                if (names_idx_opt) {
                                    const auto& eval = evaluate(v.values[*names_idx_opt], ctx);
                                    io::cast_write_evaluated_as_column_value(write, eval, tbl->cols[ci].type);
                                }
                            }
                        }

                        bool any_static_in_insert = false;
                        for (U64 si : tbl->static_col_indices) {
                            if (!tbl->cols[si].tombstone && try_get_names_idx(tbl->cols[si].name)) {
                                any_static_in_insert = true;
                                break;
                            }
                        }

                        // write regular row buffer to new blob (async)
                        bool needs_row_blob = !schema::has_clustering_keys(*tbl) || needs_clustering_row;
                        U64  row_page       = 0;
                        if (needs_row_blob) {
                            row_page = co_await blob::create_paged_dynamic(*engine.pager);
                            blob::BlobDynamicPaged row_blob;
                            co_await blob::load(row_blob, engine.pager, row_page);
                            co_await blob::insert(row_blob, row_buffer.ptr, row_buffer.length);
                        }

                        // build partition key and insert into btree(s)
                        DynamicArray<Evaluated> partition_evals;
                        for (U64 pk_ci : tbl->partition_key_col_indices) {
                            push_back(partition_evals, evaluate(v.values[*try_get_names_idx(tbl->cols[pk_ci].name)], ctx));
                        }
                        DynamicArray<U8> pk_bytes = key::serialize_partition(*tbl, partition_evals);

                        bool have_indexes = tbl->indexes.length > 0;

                        auto maintain_indexes = [&](TArrayView<const U8, U16> pk_v, TArrayView<const U8, U16> ck_v,
                                                    const DynamicArray<ColumnValue>& old_cv,
                                                    const DynamicArray<bool>&        old_present) -> coroutine::Task<void> {
                            for (auto& idx : tbl->indexes) {
                                if (idx.tombstone) {
                                    continue;
                                }
                                U64 ci = idx.col_idx;
                                if (!type_matches_tag<type::Basic>(tbl->cols[ci].type.value)) {
                                    continue;
                                }
                                type::Basic dtype = get<type::Basic>(tbl->cols[ci].type.value);

                                bool old_has = ci < old_cv.length && old_present[ci] &&
                                               !type_matches_tag<Null>(old_cv[ci]);
                                auto                ni = try_get_names_idx(tbl->cols[ci].name);
                                Optional<Evaluated> new_eval;
                                if (ni) {
                                    Evaluated e         = evaluate(v.values[*ni], ctx);
                                    bool      e_is_null = (type_matches_tag<Constant>(e.value) &&
                                                      type_matches_tag<Null>(get<Constant>(e.value).value)) ||
                                                     (type_matches_tag<ColumnValue>(e.value) &&
                                                      type_matches_tag<Null>(get<ColumnValue>(e.value)));
                                    if (!e_is_null) {
                                        new_eval = move(e);
                                    }
                                }
                                if (old_has) {
                                    DynamicArray<U8> old_key = key::make_full_index_key(
                                        key::make_index_prefix_from_cv(old_cv[ci], dtype), pk_v, ck_v);
                                    auto kv = TArrayView<const U8, U16>(old_key.ptr, static_cast<U16>(old_key.length));
                                    co_await btree::remove(idx.btree, kv);
                                }
                                if (new_eval) {
                                    DynamicArray<U8> new_key = key::make_full_index_key(
                                        key::make_index_prefix(*new_eval, dtype), pk_v, ck_v);
                                    auto kv    = TArrayView<const U8, U16>(new_key.ptr, static_cast<U16>(new_key.length));
                                    U8   dummy = 0;
                                    co_await btree::tinsert(idx.btree, kv, dummy);
                                }
                            }
                        };

                        if (schema::has_clustering_keys(*tbl)) {
                            // look up or create the partition entry
                            auto                   entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);
                            schema::PartitionEntry entry;
                            bool                   new_partition;
                            if (entry_opt) {
                                entry         = *entry_opt;
                                new_partition = false;
                            } else {
                                entry.data_page = co_await btree::create_paged(
                                    *engine.pager,
                                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{});
                                entry.static_page = 0;
                                new_partition     = true;
                            }

                            if (any_static_in_insert) {
                                DynamicArray<ColumnValue> sc_vals;
                                DynamicArray<bool>        sc_present;
                                co_await read_row_into(engine, tbl, 0, entry.static_page, sc_vals, sc_present);
                                planner::MutationSpec static_spec;
                                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                    if (is_static_col(ci) && !tbl->cols[ci].tombstone) {
                                        auto ni = try_get_names_idx(tbl->cols[ci].name);
                                        if (ni) {
                                            push_back(static_spec.updates, planner::ColumnUpdate{ci, evaluate(v.values[*ni])});
                                        }
                                    }
                                }
                                co_await apply_updates_to_row(tbl, sc_vals, sc_present, static_spec, ctx);
                                co_await rewrite_static(engine, entry, tbl, sc_vals, sc_present);
                            }

                            // persist the partition entry (insert new or update existing static_page)
                            if (new_partition) {
                                co_await btree::tinsert(tbl->btree, pk_bytes, entry);
                            } else if (any_static_in_insert) {
                                co_await btree::tupdate(tbl->btree, pk_bytes, entry);
                            }

                            DynamicArray<U8>          ck_bytes;
                            DynamicArray<ColumnValue> old_cv;
                            DynamicArray<bool>        old_present;
                            if (needs_clustering_row) {
                                schema::ClusteringBTree clustering_btree{
                                    engine.pager, entry.data_page,
                                    btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}};
                                DynamicArray<Evaluated> clustering_evals;
                                for (U64 ck_ci : tbl->clustering_key_col_indices) {
                                    push_back(clustering_evals, evaluate(v.values[*try_get_names_idx(tbl->cols[ck_ci].name)], ctx));
                                }
                                ck_bytes = key::serialize_clustering(*tbl, clustering_evals);
                                if (have_indexes) {
                                    auto row_page_opt = co_await btree::tfind<U64>(clustering_btree, ck_bytes);
                                    if (row_page_opt) {
                                        co_await read_row_into(engine, tbl, *row_page_opt, entry.static_page, old_cv, old_present);
                                    }
                                }
                                co_await btree::tinsert(clustering_btree, ck_bytes, row_page);
                            }

                            if (have_indexes && needs_clustering_row) {
                                auto pk_v = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                                auto ck_v = TArrayView<const U8, U16>(ck_bytes.ptr, static_cast<U16>(ck_bytes.length));
                                co_await maintain_indexes(pk_v, ck_v, old_cv, old_present);
                            }
                        } else {
                            DynamicArray<ColumnValue> old_cv;
                            DynamicArray<bool>        old_present;
                            if (have_indexes) {
                                auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);
                                if (entry_opt) {
                                    co_await read_row_into(engine, tbl, entry_opt->data_page, entry_opt->static_page, old_cv, old_present);
                                }
                            }
                            co_await btree::tinsert(tbl->btree, pk_bytes, schema::PartitionEntry{row_page, 0});

                            if (have_indexes) {
                                TArrayView<const U8, U16> empty_ck{nullptr, 0};
                                auto                      pk_v = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                                co_await maintain_indexes(pk_v, empty_ck, old_cv, old_present);
                            }
                        }

                        co_return create_void_success();
                    } else if constexpr (SameAs<T, Insert::JsonClause>) {
                        assert_not_implemented("inserting json is not implemented");
                        co_return ExecutionResult{};
                    } else {
                        static_assert(!SameAs<T, T>, "missing type case");
                    }
                });
            } else if constexpr (SameAs<T, Update>) {
                ZoneScopedN("engine::update");
                for (const auto& param : stmt.using_parameters) {
                    if (param.kind == UpdateParameter::Kind::TIMESTAMP) {
                        assert_true(engine.single_node, "UPDATE USING TIMESTAMP not supported in non-single-node mode");
                        log::native_info("ignoring UPDATE USING TIMESTAMP (single-node no-op)");
                    } else if (param.kind == UpdateParameter::Kind::TTL) {
                        // @todo TTL enforcement requires row blob metadata header
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "UPDATE USING TTL is not implemented"};
                    }
                }
                assert_true_not_implemented(!stmt.if_, "UPDATE IF is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace UPDATE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                planner::MutationPlan mp = planner::plan_update(stmt, *tbl, ctx);
                if (auto err = validate_plan(mp.result)) {
                    co_return move(*err);
                }
                auto apply_for_ck = [&](planner::MutationPlan& mp_ref) -> coroutine::Task<void> {
                    if (mp_ref.locator.ck_in_values.length > 0) {
                        mp_ref.locator.ck_is_equality = true;
                        for (const auto& ck_bytes : mp_ref.locator.ck_in_values) {
                            mp_ref.locator.ck_begin = ck_bytes;
                            co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, ctx);
                        }
                    } else if (!mp_ref.locator.ck_has_in) {
                        co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, ctx);
                    }
                    // else: empty CK IN → no-op
                };
                if (mp.locator.pk_in_values.length > 0) {
                    mp.locator.pk_is_equality = true;
                    for (const auto& pk_bytes : mp.locator.pk_in_values) {
                        mp.locator.pk_begin = pk_bytes;
                        co_await apply_for_ck(mp);
                    }
                } else if (!mp.locator.pk_has_in) {
                    co_await apply_for_ck(mp);
                }
                // else: empty PK IN → no-op
                co_return create_void_success();
            } else if constexpr (SameAs<T, Delete>) {
                ZoneScopedN("engine::delete");
                for (const auto& param : stmt.using_parameters) {
                    if (param.kind == UpdateParameter::Kind::TIMESTAMP) {
                        assert_true(engine.single_node, "DELETE USING TIMESTAMP not supported in non-single-node mode");
                        log::native_info("ignoring DELETE USING TIMESTAMP (single-node no-op)");
                    } else if (param.kind == UpdateParameter::Kind::TTL) {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "DELETE USING TTL is not implemented"};
                    }
                }
                assert_true_not_implemented(!stmt.if_, "DELETE IF is not implemented");

                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
                assert_true_not_implemented(!is_system_keyspace(ks_name), "system keyspace DELETE is not implemented");

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                planner::MutationPlan mp = planner::plan_delete(stmt, *tbl, ctx);
                if (auto err = validate_plan(mp.result)) {
                    co_return move(*err);
                }
                auto apply_for_ck_del = [&](planner::MutationPlan& mp_ref) -> coroutine::Task<void> {
                    if (mp_ref.locator.ck_in_values.length > 0) {
                        mp_ref.locator.ck_is_equality = true;
                        for (const auto& ck_bytes : mp_ref.locator.ck_in_values) {
                            mp_ref.locator.ck_begin = ck_bytes;
                            co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, ctx);
                        }
                    } else if (!mp_ref.locator.ck_has_in) {
                        co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, ctx);
                    }
                    // else: empty CK IN → no-op
                };
                if (mp.locator.pk_in_values.length > 0) {
                    mp.locator.pk_is_equality = true;
                    for (const auto& pk_bytes : mp.locator.pk_in_values) {
                        mp.locator.pk_begin = pk_bytes;
                        co_await apply_for_ck_del(mp);
                    }
                } else if (!mp.locator.pk_has_in) {
                    co_await apply_for_ck_del(mp);
                }
                // else: empty PK IN → no-op
                co_return create_void_success();
            } else if constexpr (SameAs<T, AlterTable>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
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
                                if (instr.if_not_exists) {
                                    continue;
                                }
                                co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Column already exists"};
                            }
                            bool found_tombstoned = false;
                            for (const auto& col : tbl->cols) {
                                if (col.tombstone && col.name == col_def.name.identifier) {
                                    found_tombstoned = true;
                                    bool same_type   = col.type == col_def.type;
                                    bool same_static = col.is_static == col_def._static;
                                    if (!same_type || !same_static) {
                                        if (instr.if_not_exists) {
                                            goto next_col;
                                        }
                                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Cannot re-add previously dropped column"};
                                    }
                                    break;
                                }
                            }
                            if (found_tombstoned && instr.if_not_exists) {
                                goto next_col;
                            }
                            if (auto res = co_await schema::create_column(engine.schema, *tbl, col_def); res.error != schema::Error::None) {
                                co_return create_server_error("Failed to add column");
                            }
                        next_col:;
                        }
                        co_return create_schema_changed(ks_name, stmt.table.table_name);
                    } else if constexpr (SameAs<I, AlterTable::DropColumnInstruction>) {
                        for (const auto& col_name : instr.columns) {
                            auto col_res = schema::read_column(engine.schema, *tbl, col_name.identifier);
                            if (col_res.error != schema::Error::None) {
                                if (instr.if_exists) {
                                    continue;
                                }
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
                            handle_table_option_pair(opt, engine);
                        }
                        co_return create_schema_changed(ks_name, stmt.table.table_name);
                    } else {
                        static_assert(!SameAs<I, I>);
                        co_return ExecutionResult{};
                    }
                });
            } else if constexpr (SameAs<T, CreateIndex>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
                auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                U64 col_idx = MAX_U64;
                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                    if (!tbl->cols[ci].tombstone && tbl->cols[ci].name == String8(stmt.column_name.c_str, stmt.column_name.length)) {
                        col_idx = ci;
                        break;
                    }
                }
                if (col_idx == MAX_U64) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "column not found"};
                }
                if (!type_matches_tag<type::Basic>(tbl->cols[col_idx].type.value)) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "cannot create index on collection column"};
                }
                if (tbl->cols[col_idx].key_kind == schema::KeyKind::PartitionKey &&
                    tbl->partition_key_col_indices.length == 1) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "cannot create secondary index on the only partition key column"};
                }

                AutoString8 index_name;
                if (stmt.index_name) {
                    index_name = *stmt.index_name;
                } else {
                    index_name = AutoString8(stmt.table.table_name);
                    append(index_name, "_");
                    append(index_name, stmt.column_name);
                    append(index_name, "_idx");
                }

                if (schema::read_index(engine.schema, *tbl, index_name).error == schema::Error::None) {
                    co_return stmt.if_not_exists ? create_void_success()
                                                 : ExecutionResult{.status = ExecutionStatus::AlreadyExists, .message = "index already exists"};
                }

                auto res = co_await schema::create_index(engine.schema, *tbl, col_idx, index_name);
                if (res.error != schema::Error::None) {
                    co_return create_server_error("failed to create index");
                }
                co_await backfill_index(engine, tbl, *res.value);

                co_return create_schema_changed(ks_name, stmt.table.table_name);
            } else if constexpr (SameAs<T, DropIndex>) {
                String8 ks_name = static_cast<bool>(stmt.index_name.keyspace_name) ? String8(*stmt.index_name.keyspace_name) : current_keyspace;
                auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(ks_name);
                }

                // @note DROP INDEX <name> doesn't say which table; scan every table in the keyspace.
                String8        idx_name_str(stmt.index_name.table_name.c_str, stmt.index_name.table_name.length);
                schema::Table* found_tbl = nullptr;
                for (auto& tbl : ks->tbls) {
                    if (!tbl.tombstone) {
                        if (schema::read_index(engine.schema, tbl, idx_name_str).error == schema::Error::None) {
                            found_tbl = &tbl;
                            break;
                        }
                    }
                }
                if (found_tbl == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success()
                                               : ExecutionResult{.status = ExecutionStatus::Invalid, .message = "index not found"};
                }
                co_await schema::drop_index(engine.schema, *found_tbl, idx_name_str);
                co_return create_schema_changed(ks_name, found_tbl->name);
            } else if constexpr (SameAs<T, CreateType>) {
                assert_not_implemented("User-defined types are not implemented");
                co_return ExecutionResult{};
            } else if constexpr (SameAs<T, AlterType>) {
                assert_not_implemented("User-defined types are not implemented");
                co_return ExecutionResult{};
            } else if constexpr (SameAs<T, DropType>) {
                assert_not_implemented("User-defined types are not implemented");
                co_return ExecutionResult{};
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
        auto result = co_await execute_inside_transaction(engine, statement, EvalContext{}, engine.current_keyspace);
        if (result.kind == ResultKind::Rows && result.status == ExecutionStatus::Success) {
            result.deferred_tx = move(tx);
        } else {
            co_await tx.commit();
        }
        co_return result;
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, const Statement& statement, AutoString8& current_keyspace) {
        pager::Transaction tx{engine.pager};
        co_await tx.begin();
        auto result = co_await execute_inside_transaction(engine, statement, EvalContext{}, current_keyspace);
        if (result.kind == ResultKind::Rows && result.status == ExecutionStatus::Success) {
            result.deferred_tx = move(tx);
        } else {
            co_await tx.commit();
        }
        co_return result;
    }

    // ========================================================================
    // bind variables
    // ========================================================================
    static type::Type col_type_in_table(const schema::Table& tbl, String8 col_name) {
        for (U64 ci = 0; ci < tbl.cols.length; ci++) {
            if (tbl.cols[ci].name == col_name) {
                return tbl.cols[ci].type;
            }
        }
        // Unknown column: use int as fallback so numeric test values serialize without driver error.
        // The query will fail server-side with ColumnNotFound regardless of the type reported here.
        return type::create_basic(type::Basic::int_);
    }

    static void collect_bind_in_term(const Term& term, type::Type hint_type, DynamicArray<BindVariableSpec>& out) {
        visit(term.value, [&](const auto& v) {
            using V = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<V, BindMarker>) {
                emplace_back(out, BindVariableSpec{.name = AutoString8(v.identifier), .type = hint_type});
            } else if constexpr (SameAs<V, ListOrVectorLiteral>) {
                for (U64 i = 0; i < v.elements.length; i++) {
                    collect_bind_in_term(v.elements[i], hint_type, out);
                }
            } else if constexpr (SameAs<V, SetLiteral>) {
                for (U64 i = 0; i < v.keys.length; i++) {
                    collect_bind_in_term(v.keys[i], hint_type, out);
                }
            } else if constexpr (SameAs<V, MapLiteral>) {
                for (U64 i = 0; i < v.key_values.length; i++) {
                    collect_bind_in_term(v.key_values[i].first, hint_type, out);
                    collect_bind_in_term(v.key_values[i].second, hint_type, out);
                }
            } else if constexpr (SameAs<V, TupleLiteral>) {
                for (U64 i = 0; i < v.elements.length; i++) {
                    collect_bind_in_term(v.elements[i], hint_type, out);
                }
            } else if constexpr (SameAs<V, FunctionCall>) {
                for (U64 i = 0; i < v.arguments.length; i++) {
                    collect_bind_in_term(v.arguments[i], hint_type, out);
                }
            }
        });
    }

    static void collect_bind_in_using_params(const DynamicArray<UpdateParameter>& params, DynamicArray<BindVariableSpec>& out) {
        for (U64 i = 0; i < params.length; i++) {
            if (type_matches_tag<BindMarker>(params[i].value)) {
                auto kind = params[i].kind == UpdateParameter::Kind::TIMESTAMP
                                ? type::Basic::bigint
                                : type::Basic::int_;
                emplace_back(out, BindVariableSpec{.name = {}, .type = type::create_basic(kind)});
            }
        }
    }

    static void collect_bind_in_where(const WhereClause& where, const schema::Table& tbl, DynamicArray<BindVariableSpec>& out) {
        for (U64 i = 0; i < where.relations.length; i++) {
            visit(where.relations[i].value, [&](const auto& rel) {
                using R = RemoveCVRef<decltype(rel)>;
                if constexpr (SameAs<R, WhereClause::ColumnExpressionRelation>) {
                    type::Type elem_t = col_type_in_table(tbl, String8(rel.column.identifier));
                    // For IN ?, the single bind marker holds the whole list.
                    type::Type t = (rel.operator_ == Operator::in && type_matches_tag<BindMarker>(rel.value.value))
                                       ? type::create_list(elem_t)
                                       : elem_t;
                    collect_bind_in_term(rel.value, t, out);
                } else if constexpr (SameAs<R, WhereClause::TupleExpressionRelation>) {
                    if (rel.operator_ == Operator::in) {
                        // (ck) IN ((?), (?)) — each value is a TupleLiteral matching rel.columns
                        for (U64 vi = 0; vi < rel.values.length; vi++) {
                            visit(rel.values[vi].value, [&](const auto& v) {
                                using VT = RemoveCVRef<decltype(v)>;
                                if constexpr (SameAs<VT, TupleLiteral>) {
                                    for (U64 ei = 0; ei < v.elements.length && ei < rel.columns.length; ei++) {
                                        String8    col = String8(rel.columns[ei].identifier);
                                        type::Type t   = col_type_in_table(tbl, col);
                                        collect_bind_in_term(v.elements[ei], t, out);
                                    }
                                } else {
                                    String8    col = rel.columns.length > 0 ? String8(rel.columns[0].identifier) : String8{};
                                    type::Type t   = col.length ? col_type_in_table(tbl, col) : type::create_basic(type::Basic::text);
                                    collect_bind_in_term(rel.values[vi], t, out);
                                }
                            });
                        }
                    } else {
                        for (U64 vi = 0; vi < rel.values.length; vi++) {
                            String8    col = vi < rel.columns.length ? String8(rel.columns[vi].identifier) : String8{};
                            type::Type t   = col.length ? col_type_in_table(tbl, col) : type::create_basic(type::Basic::text);
                            collect_bind_in_term(rel.values[vi], t, out);
                        }
                    }
                } else if constexpr (SameAs<R, WhereClause::TokenRelation>) {
                    collect_bind_in_term(rel.value, type::create_basic(type::Basic::bigint), out);
                }
            });
        }
    }

    static void collect_bind_variables_insert(Engine& engine, const Insert& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
        if (tbl == nullptr) {
            return;
        }

        if (type_matches_tag<Insert::NamesValues>(stmt.insert_clause)) {
            const auto& nv = get<Insert::NamesValues>(stmt.insert_clause);
            for (U64 i = 0; i < nv.values.length; i++) {
                if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                    String8 col_name = nv.names[i].identifier;
                    emplace_back(out, BindVariableSpec{
                                          .name = AutoString8(col_name),
                                          .type = col_type_in_table(*tbl, col_name)});
                }
            }
        }
        collect_bind_in_using_params(stmt.using_parameters, out);
    }

    static void collect_bind_variables_update(Engine& engine, const Update& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
        if (tbl == nullptr) {
            return;
        }

        collect_bind_in_using_params(stmt.using_parameters, out);
        for (U64 i = 0; i < stmt.assignments.length; i++) {
            const auto& asgn = stmt.assignments[i];
            if (type_matches_tag<BindMarker>(asgn.value.value)) {
                type::Type t = col_type_in_table(*tbl, String8(asgn.target.column.identifier));
                emplace_back(out, BindVariableSpec{.name = AutoString8(asgn.target.column.identifier), .type = t});
            }
        }
        collect_bind_in_where(stmt.where, *tbl, out);
    }

    static void collect_bind_variables_delete(Engine& engine, const Delete& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
        if (tbl == nullptr) {
            return;
        }

        collect_bind_in_using_params(stmt.using_parameters, out);
        collect_bind_in_where(stmt.where, *tbl, out);
    }

    static void collect_bind_variables_select(Engine& engine, const Select& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = static_cast<bool>(stmt.from.keyspace_name) ? String8(*stmt.from.keyspace_name) : current_keyspace;
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(engine.schema, *ks, stmt.from.table_name).value;
        if (tbl == nullptr) {
            return;
        }

        if (static_cast<bool>(stmt.where)) {
            collect_bind_in_where(*stmt.where, *tbl, out);
        }

        auto int_type = type::create_basic(type::Basic::int_);
        if (type_matches_tag<BindMarker>(stmt.limit.value)) {
            emplace_back(out, BindVariableSpec{.name = {}, .type = int_type});
        }
        if (type_matches_tag<BindMarker>(stmt.per_partition_limit.value)) {
            emplace_back(out, BindVariableSpec{.name = {}, .type = int_type});
        }
    }

    static DynamicArray<BindVariableSpec> collect_bind_variables_with_keyspace(Engine& engine, const Statement& statement, String8 current_keyspace) {
        DynamicArray<BindVariableSpec> result;
        visit(statement.value, [&](const auto& stmt) {
            using T = RemoveCVRef<decltype(stmt)>;
            if constexpr (SameAs<T, Insert>) {
                collect_bind_variables_insert(engine, stmt, result, current_keyspace);
            } else if constexpr (SameAs<T, Update>) {
                collect_bind_variables_update(engine, stmt, result, current_keyspace);
            } else if constexpr (SameAs<T, Delete>) {
                collect_bind_variables_delete(engine, stmt, result, current_keyspace);
            } else if constexpr (SameAs<T, Select>) {
                collect_bind_variables_select(engine, stmt, result, current_keyspace);
            }
        });
        return result;
    }

    DynamicArray<BindVariableSpec> collect_bind_variables(Engine& engine, const Statement& statement) {
        return collect_bind_variables_with_keyspace(engine, statement, String8(engine.current_keyspace));
    }

    DynamicArray<BindVariableSpec> collect_bind_variables(Engine& engine, const Statement& statement, String8 current_keyspace) {
        return collect_bind_variables_with_keyspace(engine, statement, current_keyspace);
    }

    static void bind_in_term(Term& term, DynamicArray<Term>& bv, U64& idx) {
        if (type_matches_tag<BindMarker>(term.value)) {
            if (idx < bv.length) {
                term = move(bv[idx++]);
            }
            return;
        }
        visit(term.value, [&](auto& v) {
            using V = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<V, ListOrVectorLiteral> || SameAs<V, TupleLiteral>) {
                for (U64 i = 0; i < v.elements.length; i++) {
                    bind_in_term(v.elements[i], bv, idx);
                }
            } else if constexpr (SameAs<V, SetLiteral>) {
                for (U64 i = 0; i < v.keys.length; i++) {
                    bind_in_term(v.keys[i], bv, idx);
                }
            } else if constexpr (SameAs<V, MapLiteral>) {
                for (U64 i = 0; i < v.key_values.length; i++) {
                    bind_in_term(v.key_values[i].first, bv, idx);
                    bind_in_term(v.key_values[i].second, bv, idx);
                }
            } else if constexpr (SameAs<V, FunctionCall>) {
                for (U64 i = 0; i < v.arguments.length; i++) {
                    bind_in_term(v.arguments[i], bv, idx);
                }
            }
        });
    }

    static void bind_in_where(WhereClause& where, DynamicArray<Term>& bv, U64& idx) {
        for (U64 i = 0; i < where.relations.length; i++) {
            visit(where.relations[i].value, [&](auto& rel) {
                using R = RemoveCVRef<decltype(rel)>;
                if constexpr (SameAs<R, WhereClause::ColumnExpressionRelation>) {
                    bind_in_term(rel.value, bv, idx);
                } else if constexpr (SameAs<R, WhereClause::TupleExpressionRelation>) {
                    for (U64 vi = 0; vi < rel.values.length; vi++) {
                        bind_in_term(rel.values[vi], bv, idx);
                    }
                } else if constexpr (SameAs<R, WhereClause::TokenRelation>) {
                    bind_in_term(rel.value, bv, idx);
                }
            });
        }
    }

    static void bind_values_to_statement(Statement& stmt, DynamicArray<Term>& bound_values) {
        if (bound_values.length == 0) {
            return;
        }
        U64 idx = 0;
        visit(stmt.value, [&](auto& s) {
            using T = RemoveCVRef<decltype(s)>;
            if constexpr (SameAs<T, Insert>) {
                if (!type_matches_tag<Insert::NamesValues>(s.insert_clause)) {
                    return;
                }
                auto& nv = get<Insert::NamesValues>(s.insert_clause);
                for (U64 i = 0; i < nv.values.length && idx < bound_values.length; i++) {
                    if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                        nv.values[i] = move(bound_values[idx++]);
                    }
                }
                // advance past any USING TIMESTAMP/TTL bind markers
                for (U64 i = 0; i < s.using_parameters.length; i++) {
                    if (type_matches_tag<BindMarker>(s.using_parameters[i].value)) {
                        idx++;
                    }
                }
            } else if constexpr (SameAs<T, Update>) {
                for (U64 i = 0; i < s.using_parameters.length; i++) {
                    if (type_matches_tag<BindMarker>(s.using_parameters[i].value)) {
                        idx++;
                    }
                }
                for (U64 i = 0; i < s.assignments.length && idx < bound_values.length; i++) {
                    if (type_matches_tag<BindMarker>(s.assignments[i].value.value)) {
                        s.assignments[i].value = TermWithIdentifiers(move(bound_values[idx++]));
                    }
                }
                bind_in_where(s.where, bound_values, idx);
            } else if constexpr (SameAs<T, Delete>) {
                for (U64 i = 0; i < s.using_parameters.length; i++) {
                    if (type_matches_tag<BindMarker>(s.using_parameters[i].value)) {
                        idx++;
                    }
                }
                bind_in_where(s.where, bound_values, idx);
            } else if constexpr (SameAs<T, Select>) {
                if (static_cast<bool>(s.where)) {
                    bind_in_where(*s.where, bound_values, idx);
                }
                if (type_matches_tag<BindMarker>(s.limit.value)) {
                    if (idx < bound_values.length) {
                        const auto& bv = bound_values[idx++];
                        if (type_matches_tag<Constant>(bv.value)) {
                            const auto& c = get<Constant>(bv.value);
                            if (type_matches_tag<S64>(c.value)) {
                                s.limit.value = get<S64>(c.value);
                            }
                        }
                    } else {
                        idx++;
                    }
                }
                if (type_matches_tag<BindMarker>(s.per_partition_limit.value)) {
                    idx++;
                }
            }
        });
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, Statement& statement, DynamicArray<Term>&& bound_values) {
        bind_values_to_statement(statement, bound_values);
        co_return co_await execute(engine, statement);
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, Statement& statement, DynamicArray<Term>&& bound_values, AutoString8& current_keyspace) {
        bind_values_to_statement(statement, bound_values);
        co_return co_await execute(engine, statement, current_keyspace);
    }

    // ========================================================================
    // prepared statements
    // ========================================================================
    PrepareResult prepare(Engine& engine, String8 query, String8 current_keyspace) {
        ZoneScopedN("engine::prepare");
        U64 query_hash = hash(query);

        auto* existing = find(engine.prepared_cache, query_hash);
        if (existing != nullptr) {
            return {.status = ExecutionStatus::Success, .id = query_hash, .entry = existing};
        }

        if (auto specific_err = parsers::check_specific_errors(query)) {
            return {.status = ExecutionStatus::SyntaxError, .message = *specific_err};
        }

        auto cql_opt = parsers::parse(query);
        if (!cql_opt) {
            return {.status = ExecutionStatus::SyntaxError, .message = "Failed to parse CQL"};
        }

        auto& entry          = insert(engine.prepared_cache, query_hash);
        entry.query_string   = AutoString8(query);
        entry.bind_variables = collect_bind_variables_with_keyspace(engine, *cql_opt, current_keyspace);

        visit(cql_opt->value, [&, current_keyspace](const auto& stmt) {
            using T = RemoveCVRef<decltype(stmt)>;
            if constexpr (SameAs<T, Insert>) {
                String8 ks_name = static_cast<bool>(stmt.table.keyspace_name) ? String8(*stmt.table.keyspace_name) : current_keyspace;
                entry.keyspace  = AutoString8(ks_name);
                entry.table     = AutoString8(stmt.table.table_name);

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    return;
                }
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    return;
                }
                for (U64 i = 0; i < entry.bind_variables.length; i++) {
                    if (tbl->partition_key_col_indices.length > 0 &&
                        tbl->cols[tbl->partition_key_col_indices[0]].name == entry.bind_variables[i].name) {
                        entry.pk_index = S32(i);
                        break;
                    }
                }
            }
        });

        return {.status = ExecutionStatus::Success, .id = query_hash, .entry = &entry};
    }

    PreparedEntry* try_get_prepared(Engine& engine, U64 prepared_id) {
        return find(engine.prepared_cache, prepared_id);
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, U64 prepared_id, DynamicArray<Term>&& bound_values) {
        auto* entry = find(engine.prepared_cache, prepared_id);
        if (entry == nullptr) {
            co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Prepared statement not found"};
        }

        auto cql_opt = parsers::parse(String8(entry->query_string));
        if (!cql_opt) {
            co_return ExecutionResult{.status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query"};
        }

        co_return co_await execute(engine, *cql_opt, move(bound_values));
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, U64 prepared_id, DynamicArray<Term>&& bound_values, AutoString8& current_keyspace) {
        auto* entry = find(engine.prepared_cache, prepared_id);
        if (entry == nullptr) {
            co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Prepared statement not found"};
        }

        auto cql_opt = parsers::parse(String8(entry->query_string));
        if (!cql_opt) {
            co_return ExecutionResult{.status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query"};
        }

        co_return co_await execute(engine, *cql_opt, move(bound_values), current_keyspace);
    }
}
