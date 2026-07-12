module;
#include <coroutine>
#include <cstring>
#include <plexdb/support/tracy/tracy.hpp>

module cql.engine;

import plexdb.os;
import plexdb.btree.types;
import plexdb.dynamic.tagged_union;
import plexdb.support.sort;

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
        return ks == "system" || ks == "system_schema" || ks == "system_virtual_schema" || ks == "system_auth" || ks == "system_distributed" || ks == "system_traces";
    }

    // Rebuilds slot `idx` from build_fn only when the schema has changed since it was last
    // cached; returns a copy so callers (which filter/project in place) can't corrupt the cache.
    static VirtualRows cached_virtual_rows(Engine& engine, U64 idx, auto&& build_fn) {
        VirtualTableCacheEntry& entry = engine.virtual_table_cache[idx];
        if (entry.schema_version != engine.schema.version) {
            entry.rows           = build_fn();
            entry.schema_version = engine.schema.version;
        }
        return entry.rows;
    }

    // @todo in cassandra the schema is stored in the database directly, this is probably a good idea in future
    Optional<VirtualRows> try_system_select(Engine& engine, String8 keyspace, String8 table) {
        if (keyspace == "system") {
            if (table == "local") {
                return cached_virtual_rows(engine, 0, [&] { return create_system_local(engine.port, engine.schema); });
            }
            if (table == "peers") {
                return cached_virtual_rows(engine, 1, [&] { return create_system_peers(); });
            }
            if (table == "peers_v2") {
                return cached_virtual_rows(engine, 2, [&] { return create_system_peers_v2(); });
            }
        }
        if (keyspace == "system_schema") {
            if (table == "keyspaces") {
                return cached_virtual_rows(engine, 3, [&] { return create_schema_keyspaces(engine.schema); });
            }
            if (table == "tables") {
                return cached_virtual_rows(engine, 4, [&] { return create_schema_tables(engine.schema); });
            }
            if (table == "columns") {
                return cached_virtual_rows(engine, 5, [&] { return create_schema_columns(engine.schema); });
            }
            if (table == "views") {
                return cached_virtual_rows(engine, 6, [&] { return create_schema_views(engine.schema); });
            }
            if (table == "indexes") {
                return cached_virtual_rows(engine, 7, [&] { return create_schema_indexes(engine.schema); });
            }
            if (table == "triggers") {
                return cached_virtual_rows(engine, 8, [&] { return create_schema_triggers(engine.schema); });
            }
            if (table == "dropped_columns") {
                return cached_virtual_rows(engine, 9, [&] { return create_schema_dropped_columns(engine.schema); });
            }
            if (table == "types") {
                return cached_virtual_rows(engine, 10, [&] { return create_schema_types(engine.schema); });
            }
            if (table == "functions") {
                return cached_virtual_rows(engine, 11, [&] { return create_schema_functions(engine.schema); });
            }
            if (table == "aggregates") {
                return cached_virtual_rows(engine, 12, [&] { return create_schema_aggregates(engine.schema); });
            }
        }
        return {};
    }

    // ========================================================================
    // execute
    // ========================================================================

    // Shared by every clustering-less call site that needs to spell "no clustering key
    // bytes" — read_row_into/rmw_row's ck_bytes default and update_indexes' ck argument for
    // tables without a clustering key both mean the same empty view.
    static const TArrayView<const U8, U16> NO_CK_BYTES{nullptr, 0};

    // Drain `col_it` into parallel col_values/col_present arrays sized to tbl->cols.length.
    // If out_cell_meta is non-null, also populates per-column cell metadata observed during iteration.
    static coroutine::Task<void> drain_columns(const schema::Table* tbl, ColumnIterator& col_it, DynamicArray<ColumnValue>& col_values, DynamicArray<bool>& col_present, DynamicArray<Optional<io::CellMetadata>>* out_cell_meta = nullptr) {
        resize(col_values, tbl->cols.length);
        resize(col_present, tbl->cols.length);
        if (out_cell_meta != nullptr) {
            resize(*out_cell_meta, tbl->cols.length);
        }
        ColumnIterator col_end{};
        U64            ci = 0;
        while (col_it != col_end && ci < tbl->cols.length) {
            ColumnValue val = co_await col_it.deref();
            col_present[ci] = !type_matches_tag<Null>(val);
            col_values[ci]  = move(val);
            if (out_cell_meta != nullptr && col_present[ci] && col_it.current_cell_metadata.flags != 0) {
                (*out_cell_meta)[ci] = col_it.current_cell_metadata;
            }
            co_await col_it.advance();
            ++ci;
        }
        // @note resize default-constructs ColumnValue as an empty TaggedUnion. Anything the
        // iterator didn't reach must become Null{} so downstream visit() does not crash.
        for (U64 j = ci; j < tbl->cols.length; j++) {
            col_values[j]  = ColumnValue{Null{}};
            col_present[j] = false;
        }
    }

    // ck_bytes is only meaningful for clustering tables reading a specific row; omit it
    // (or pass an empty view) to read just the static/pk-preamble portion, or for
    // clustering-less tables — load()/decode_clustering treat an empty view as a no-op, so
    // there's one read path regardless of whether a clustering key is involved.
    static coroutine::Task<void> read_row_into(
        Engine& engine, const schema::Table* tbl, U64 page_idx, U64 static_page_idx,
        DynamicArray<ColumnValue>& col_values, DynamicArray<bool>& col_present,
        io::RowMetadata* out_metadata = nullptr, DynamicArray<Optional<io::CellMetadata>>* out_cell_meta = nullptr,
        TArrayView<const U8, U16> ck_bytes = NO_CK_BYTES
    ) {
        resize(col_values, tbl->cols.length);
        resize(col_present, tbl->cols.length);
        if (out_metadata != nullptr) {
            *out_metadata = {};
        }
        if (out_cell_meta != nullptr) {
            resize(*out_cell_meta, tbl->cols.length);
        }
        if (page_idx == 0 && static_page_idx == 0) {
            co_return;
        }
        ColumnIterator col_it;
        co_await load(col_it, engine.pager, tbl, page_idx, static_page_idx, ck_bytes);
        if (out_metadata != nullptr) {
            *out_metadata = col_it.metadata;
        }
        co_await drain_columns(tbl, col_it, col_values, col_present, out_cell_meta);
    }

    static DynamicArray<U64> build_select_col_order(const schema::Table& tbl, const DynamicArray<U64>& select_col_indices) {
        if (select_col_indices.length > 0) {
            return select_col_indices;
        }
        return tbl.select_star_col_indices;
    }

    // Build the column-metadata header for a VirtualRows result. When `aliases`
    // is populated and aliases[i] holds a value, that label overrides the
    // underlying schema column name. The owned alias bytes live in
    // vr.column_name_storage so the String8 view stays valid for the result.
    static VirtualRows make_virtual_rows_shell(const schema::Table& tbl, String8 ks_name, String8 table_name, const DynamicArray<U64>& col_order, const DynamicArray<Optional<AutoString8>>& aliases = {}) {
        VirtualRows vr;
        vr.keyspace = AutoString8(ks_name);
        vr.table    = AutoString8(table_name);
        reserve(vr.column_name_storage, col_order.length);
        for (U64 i = 0; i < col_order.length; i++) {
            U64        ci   = col_order[i];
            type::Type type = tbl.cols[ci].type;
            if (i < aliases.length && aliases[i].has_value()) {
                push_back(vr.column_name_storage, AutoString8(*aliases[i]));
                const auto& s = vr.column_name_storage[vr.column_name_storage.length - 1];
                push_back(vr.columns, VirtualColumn{String8(s.c_str, s.length), type});
            } else {
                push_back(vr.columns, VirtualColumn{tbl.cols[ci].name, type});
            }
        }
        return vr;
    }

    // Project a fully-materialized (cv, present) row into a VirtualRow following
    // col_order; missing cells stay default-constructed (Null in protocol output).
    static VirtualRow project_virtual_row(const DynamicArray<U64>& col_order, const DynamicArray<ColumnValue>& cv, const DynamicArray<bool>& present) {
        VirtualRow vrow;
        resize(vrow.values, col_order.length);
        for (U64 i = 0; i < col_order.length; i++) {
            U64 ci = col_order[i];
            if (ci < cv.length && present[ci]) {
                vrow.values[i] = cv[ci];
            } else {
                vrow.values[i] = ColumnValue{Null{}};
            }
        }
        return vrow;
    }

    // @note explicit USING TTL of 0 disables TTL (overrides default_ttl_ms); only a missing/unset
    // USING TTL falls back to the default.
    static S64 resolve_using_ttl_ms(const DynamicArray<UpdateParameter>& params, S64 default_ttl_ms) {
        for (const auto& p : params) {
            if (p.kind != UpdateParameter::Kind::TTL) {
                continue;
            }
            if (!type_matches_tag<S64>(p.value)) {
                return default_ttl_ms;
            }
            S64 secs = get<S64>(p.value);
            return secs > 0 ? secs * 1000 : 0;
        }
        return default_ttl_ms;
    }

    // Mirrors Cassandra/Scylla: max TTL is 20 years in seconds, not configurable.
    constexpr S64 MAX_TTL_SECONDS = 20_s64 * 365_s64 * 24_s64 * 60_s64 * 60_s64;

    static Optional<ExecutionResult> validate_using_ttl(const DynamicArray<UpdateParameter>& params) {
        for (const auto& p : params) {
            if (p.kind != UpdateParameter::Kind::TTL) {
                continue;
            }
            if (!type_matches_tag<S64>(p.value)) {
                return {};
            }
            S64 secs = get<S64>(p.value);
            if (secs < 0) {
                return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "A TTL must be greater or equal to 0"};
            }
            if (secs > MAX_TTL_SECONDS) {
                return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "ttl is too large."};
            }
            break;
        }
        return {};
    }

    static Optional<ExecutionResult> validate_using_timestamp(const DynamicArray<UpdateParameter>& params) {
        for (const auto& p : params) {
            if (p.kind != UpdateParameter::Kind::TIMESTAMP) {
                continue;
            }
            if (!type_matches_tag<S64>(p.value)) {
                continue;
            }
            S64 ts = get<S64>(p.value);
            // Cassandra rejects Long.MIN_VALUE as a TIMESTAMP — it is reserved as a sentinel.
            if (ts == MIN_S64) {
                return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Out of range; TIMESTAMP must be greater than Long.MIN_VALUE"};
            }
        }
        return {};
    }

    // @note assert_true requires a static-storage-duration message, so the caller passes its
    // own literal text rather than this function formatting one dynamically.
    static void warn_using_timestamp_single_node(Engine& engine, const DynamicArray<UpdateParameter>& params, const char* assert_msg, const char* log_msg) {
        for (const auto& p : params) {
            if (p.kind == UpdateParameter::Kind::TIMESTAMP) {
                assert_true(engine.single_node, assert_msg);
                log::native_info(log_msg);
            }
        }
    }

    static S64 resolve_using_timestamp_us(const DynamicArray<UpdateParameter>& params) {
        for (const auto& p : params) {
            if (p.kind != UpdateParameter::Kind::TIMESTAMP) {
                continue;
            }
            if (!type_matches_tag<S64>(p.value)) {
                return 0;
            }
            return get<S64>(p.value);
        }
        return 0;
    }

    // Compute the per-cell metadata template stamped on every cell that an INSERT or UPDATE
    // touches. WRITETIME is always present (USING TIMESTAMP or current wall clock);
    // TTL is present only when USING TTL or default_ttl_ms resolves to a positive value.
    static io::CellMetadata resolve_mutation_cell_meta(const DynamicArray<UpdateParameter>& params, S64 default_ttl_ms) {
        S64              ttl_ms       = resolve_using_ttl_ms(params, default_ttl_ms);
        S64              ts_us        = resolve_using_timestamp_us(params);
        S64              now_ms       = S64(os::unix_ms_now());
        S64              writetime_us = ts_us != 0 ? ts_us : os::unix_us_now();
        io::CellMetadata cm{};
        cm.flags |= io::CELL_FLAG_HAS_WRITETIME;
        cm.writetime_us = writetime_us;
        if (ttl_ms > 0) {
            cm.flags |= io::CELL_FLAG_HAS_TTL;
            cm.expiry_unix_ms = now_ms + ttl_ms;
        }
        return cm;
    }

    static io::RowMetadata compute_insert_metadata(const DynamicArray<UpdateParameter>& params, S64 default_ttl_ms) {
        S64 ttl_ms = resolve_using_ttl_ms(params, default_ttl_ms);
        if (ttl_ms <= 0) {
            return {};
        }
        return io::RowMetadata{.flags = io::ROW_FLAG_HAS_TTL, .expiry_unix_ms = S64(os::unix_ms_now()) + ttl_ms};
    }

    // Recompute row-level TTL from per-cell metadata so row_is_expired stays a conservative
    // optimization (only fires when every present non-key cell is expired).
    static io::RowMetadata recompute_row_metadata(const schema::Table* tbl, const DynamicArray<bool>& col_present, const DynamicArray<Optional<io::CellMetadata>>& cell_meta) {
        bool any_cell     = false;
        bool all_have_ttl = true;
        S64  max_expiry   = 0;
        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
            if (tbl->cols[ci].is_static || tbl->cols[ci].key_kind != schema::KeyKind::None || !col_present[ci]) {
                continue;
            }
            any_cell = true;
            if (cell_meta[ci] && io::cell_has_ttl(*cell_meta[ci])) {
                max_expiry = max(max_expiry, cell_meta[ci]->expiry_unix_ms);
            } else {
                all_have_ttl = false;
            }
        }
        if (!any_cell || !all_have_ttl) {
            return {};
        }
        return io::RowMetadata{.flags = io::ROW_FLAG_HAS_TTL, .expiry_unix_ms = max_expiry};
    }

    // Writes `data` into `existing_page` in place (resize + overwrite from offset 0) if
    // nonzero, else allocates a fresh page. A blob's first page is stable for its lifetime
    // (plexdb::blob never reallocates it on resize), so reusing existing_page keeps the
    // page number — and therefore every pointer to it (PartitionEntry, IndexEntry) — valid
    // across content changes; only actual row/partition deletion needs blob::remove.
    static coroutine::Task<U64> write_blob_in_place(Engine& engine, U64 existing_page, const U8* data, U64 length) {
        if (existing_page == 0) {
            U64                    page = co_await blob::create_paged_dynamic(*engine.pager);
            blob::BlobDynamicPaged b;
            co_await blob::load(b, engine.pager, page);
            co_await blob::insert(b, data, length);
            co_return page;
        }
        blob::BlobDynamicPaged b;
        co_await blob::load(b, engine.pager, existing_page);
        co_await blob::resize(b, length);
        co_await blob::update(b, data, length, 0);
        co_return existing_page;
    }

    // Writes the partition-key preamble shared by both pk-preamble locations (see
    // rewrite_static / write_row_blob's callers): one io::write_column_value cell per
    // partition-key column, in key_position order, no mask (pk values are never null).
    static void write_pk_preamble(io::Writer write, const schema::Table* tbl, const DynamicArray<ColumnValue>& col_values, const DynamicArray<bool>& col_present) {
        for (U64 pk_ci : tbl->partition_key_col_indices) {
            assert_true(pk_ci < col_present.length && col_present[pk_ci], "partition-key columns must already be populated to write the pk preamble");
            io::write_column_value(write, col_values[pk_ci], tbl->cols[pk_ci].type);
        }
    }

    // Serialise non-key, non-static columns from col_values/col_present into the row blob;
    // returns the page index (existing_page if reused in place, else a freshly allocated
    // one). CK columns are stored in the encoded key bytes, so they are never written here.
    static coroutine::Task<U64> write_row_blob(Engine& engine, const schema::Table* tbl, const DynamicArray<ColumnValue>& col_values, const DynamicArray<bool>& col_present, const DynamicArray<Optional<io::CellMetadata>>& cell_meta, const io::RowMetadata& metadata = {}, U64 existing_page = 0) {
        DynamicArray<U8> buf;
        auto             write_fn = io::sync_buffer_writer(buf);
        auto             write    = io::to_writer(write_fn);
        // @note tables without a clustering key have exactly one row blob per partition
        // and can never have static columns, so there's no reason to also maintain a
        // separate per-partition static page just to hold the pk preamble — prepend it
        // here instead (avoids doubling page-allocation count for such tables).
        if (!schema::has_clustering_keys(*tbl)) {
            write_pk_preamble(write, tbl, col_values, col_present);
        }
        auto is_active = [&](U64 idx) {
            return idx < col_present.length && col_present[idx] && !tbl->cols[idx].is_static && tbl->cols[idx].key_kind == schema::KeyKind::None;
        };
        auto has_cell_meta = [&](U64 idx) {
            return is_active(idx) && idx < cell_meta.length && cell_meta[idx].has_value();
        };
        io::write_row_metadata(write, metadata);
        io::write_column_mask(write, io::to_checker(is_active), tbl->cols.length);
        io::write_mask_bits(write, io::to_checker(has_cell_meta), tbl->cols.length);
        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
            if (is_active(ci)) {
                if (has_cell_meta(ci)) {
                    io::write_cell_metadata(write, *cell_meta[ci]);
                }
                io::write_column_value(write, col_values[ci], tbl->cols[ci].type);
            }
        }
        co_return co_await write_blob_in_place(engine, existing_page, buf.ptr, buf.length);
    }

    static coroutine::Task<ColumnValue> materialize_as_column_value(
        const Evaluated& eval, const type::Type& cdtype, const EvalContext& ctx
    ) {
        // Direct Evaluated -> ColumnValue (coercion spec §4, §5); no serialize/deserialize round-trip.
        // resolve_evaluated is total over every arm that passes can_cast_write.
        Optional<ColumnValue> direct = io::resolve_evaluated(eval, cdtype, ctx);
        assert_true(direct.has_value(), "resolve_evaluated failed for a writable value");
        co_return move(*direct);
    }

    static bool eval_is_null(const Evaluated& eval) {
        if (type_matches_tag<Literal>(eval.value)) {
            return type_matches_tag<Null>(get<Literal>(eval.value).value);
        }
        if (type_matches_tag<ColumnValue>(eval.value)) {
            return type_matches_tag<Null>(get<ColumnValue>(eval.value));
        }
        return false;
    }

    static bool eval_is_unset(const Evaluated& eval) {
        return type_matches_tag<Literal>(eval.value) && type_matches_tag<Unset>(get<Literal>(eval.value).value);
    }

    static S64 evaluated_to_s64(const Evaluated& eval) {
        return visit(eval.value, [](const auto& top) -> S64 {
            using T = RemoveCVRef<decltype(top)>;
            if constexpr (SameAs<T, Literal>) {
                return visit(top.value, [](const auto& c) -> S64 {
                    using C = RemoveCVRef<decltype(c)>;
                    if constexpr (SameAs<C, S64>) {
                        return c;
                    } else {
                        assert_not_implemented("evaluated_to_s64: non-integer Literal");
                        return 0;
                    }
                });
            } else if constexpr (SameAs<T, ColumnValue>) {
                return visit(top, [](const auto& cv) -> S64 {
                    using V = RemoveCVRef<decltype(cv)>;
                    if constexpr (SameAs<V, S64>) {
                        return cv;
                    } else if constexpr (SameAs<V, S32>) {
                        return S64(cv);
                    } else if constexpr (SameAs<V, S16>) {
                        return S64(cv);
                    } else if constexpr (SameAs<V, U8>) {
                        return S64(static_cast<S8>(cv));
                    } else {
                        assert_not_implemented("evaluated_to_s64: non-integer ColumnValue");
                        return 0;
                    }
                });
            } else {
                assert_not_implemented("evaluated_to_s64: unexpected Evaluated variant");
                return 0;
            }
        });
    }

    static coroutine::Task<bool> apply_collection_patch(
        const schema::Table*            tbl,
        U64                             idx,
        DynamicArray<ColumnValue>&      col_values,
        DynamicArray<bool>&             col_present,
        const planner::CollectionPatch& patch,
        const EvalContext&              ctx
    ) {
        const type::Type& col_type = tbl->cols[idx].type;
        bool              is_list  = type_matches_tag<type::List>(col_type.value);
        bool              is_set   = type_matches_tag<type::Set>(col_type.value);
        bool              is_map   = type_matches_tag<type::Map>(col_type.value);

        auto current_list = [&]() -> DynamicArray<NestedColumnValue> {
            if (col_present[idx] && type_matches_tag<DynamicArray<NestedColumnValue>>(col_values[idx])) {
                return get<DynamicArray<NestedColumnValue>>(col_values[idx]);
            }
            return DynamicArray<NestedColumnValue>{};
        };
        auto current_set = [&]() -> DynamicSet<NestedColumnValue> {
            if (col_present[idx] && type_matches_tag<DynamicSet<NestedColumnValue>>(col_values[idx])) {
                return get<DynamicSet<NestedColumnValue>>(col_values[idx]);
            }
            return DynamicSet<NestedColumnValue>{};
        };
        auto current_map = [&]() -> DynamicMap<NestedColumnValue, NestedColumnValue> {
            if (col_present[idx] && type_matches_tag<DynamicMap<NestedColumnValue, NestedColumnValue>>(col_values[idx])) {
                return get<DynamicMap<NestedColumnValue, NestedColumnValue>>(col_values[idx]);
            }
            return DynamicMap<NestedColumnValue, NestedColumnValue>{};
        };

        using Op = planner::CollectionPatch::Op;

        if (is_list) {
            const type::Type& el_type = get<type::List>(col_type.value).element;
            switch (patch.op) {
                case Op::Append:
                case Op::Prepend:
                case Op::Subtract: {
                    ColumnValue rhs_cv = co_await materialize_as_column_value(patch.value, col_type, ctx);
                    assert_true(type_matches_tag<DynamicArray<NestedColumnValue>>(rhs_cv), "list compound op: planner should have rejected non-list RHS");
                    auto&                           rhs = get<DynamicArray<NestedColumnValue>>(rhs_cv);
                    auto                            cur = current_list();
                    DynamicArray<NestedColumnValue> out{};
                    if (patch.op == Op::Append) {
                        for (auto& e : cur) {
                            push_back(out, e);
                        }
                        for (auto& e : rhs) {
                            push_back(out, e);
                        }
                    } else if (patch.op == Op::Prepend) {
                        for (auto& e : rhs) {
                            push_back(out, e);
                        }
                        for (auto& e : cur) {
                            push_back(out, e);
                        }
                    } else {
                        for (auto& e : cur) {
                            bool drop = false;
                            for (auto& r : rhs) {
                                if (e == r) {
                                    drop = true;
                                    break;
                                }
                            }
                            if (!drop) {
                                push_back(out, e);
                            }
                        }
                    }
                    if (out.length == 0) {
                        col_present[idx] = false;
                    } else {
                        col_values[idx]  = ColumnValue{move(out)};
                        col_present[idx] = true;
                    }
                    co_return true;
                }
                case Op::SubscriptSet: {
                    auto cur = current_list();
                    S64  i   = evaluated_to_s64(patch.key);
                    if (i < 0 || U64(i) >= cur.length) {
                        co_return false;
                    }
                    if (eval_is_null(patch.value)) {
                        DynamicArray<NestedColumnValue> out{};
                        for (U64 j = 0; j < cur.length; j++) {
                            if (j != U64(i)) {
                                push_back(out, cur[j]);
                            }
                        }
                        if (out.length == 0) {
                            col_present[idx] = false;
                        } else {
                            col_values[idx]  = ColumnValue{move(out)};
                            col_present[idx] = true;
                        }
                        co_return true;
                    }
                    ColumnValue el_cv = co_await materialize_as_column_value(patch.value, el_type, ctx);
                    cur[U64(i)]       = NestedColumnValue{move(el_cv)};
                    col_values[idx]   = ColumnValue{move(cur)};
                    col_present[idx]  = true;
                    co_return true;
                }
                case Op::SubscriptDelete: {
                    auto cur = current_list();
                    S64  i   = evaluated_to_s64(patch.key);
                    if (i < 0 || U64(i) >= cur.length) {
                        co_return false;
                    }
                    DynamicArray<NestedColumnValue> out{};
                    for (U64 j = 0; j < cur.length; j++) {
                        if (j != U64(i)) {
                            push_back(out, cur[j]);
                        }
                    }
                    if (out.length == 0) {
                        col_present[idx] = false;
                    } else {
                        col_values[idx]  = ColumnValue{move(out)};
                        col_present[idx] = true;
                    }
                    co_return true;
                }
            }
        }

        if (is_set) {
            const type::Type& key_type = get<type::Set>(col_type.value).key;
            switch (patch.op) {
                case Op::Append: {
                    ColumnValue rhs_cv = co_await materialize_as_column_value(patch.value, col_type, ctx);
                    assert_true(type_matches_tag<DynamicSet<NestedColumnValue>>(rhs_cv), "set compound op: planner should have rejected non-set RHS");
                    auto& rhs = get<DynamicSet<NestedColumnValue>>(rhs_cv);
                    auto  cur = current_set();
                    for (auto it = rhs.begin(); it != rhs.end(); ++it) {
                        insert(cur, *it);
                    }
                    col_values[idx]  = ColumnValue{move(cur)};
                    col_present[idx] = length(get<DynamicSet<NestedColumnValue>>(col_values[idx])) > 0;
                    co_return true;
                }
                case Op::Subtract: {
                    ColumnValue rhs_cv = co_await materialize_as_column_value(patch.value, col_type, ctx);
                    assert_true(type_matches_tag<DynamicSet<NestedColumnValue>>(rhs_cv), "set compound op: planner should have rejected non-set RHS");
                    auto&                         rhs = get<DynamicSet<NestedColumnValue>>(rhs_cv);
                    auto                          cur = current_set();
                    DynamicSet<NestedColumnValue> out{};
                    for (auto it = cur.begin(); it != cur.end(); ++it) {
                        if (!contains(rhs, *it)) {
                            insert(out, *it);
                        }
                    }
                    if (length(out) == 0) {
                        col_present[idx] = false;
                    } else {
                        col_values[idx]  = ColumnValue{move(out)};
                        col_present[idx] = true;
                    }
                    co_return true;
                }
                case Op::SubscriptDelete: {
                    ColumnValue                   el_cv = co_await materialize_as_column_value(patch.key, key_type, ctx);
                    NestedColumnValue             el{move(el_cv)};
                    auto                          cur = current_set();
                    DynamicSet<NestedColumnValue> out{};
                    for (auto it = cur.begin(); it != cur.end(); ++it) {
                        if (!((*it) == el)) {
                            insert(out, *it);
                        }
                    }
                    if (length(out) == 0) {
                        col_present[idx] = false;
                    } else {
                        col_values[idx]  = ColumnValue{move(out)};
                        col_present[idx] = true;
                    }
                    co_return true;
                }
                case Op::Prepend:
                case Op::SubscriptSet:
                    assert_not_implemented("set columns do not support prepend or subscript-set; planner should have rejected");
                    co_return false;
            }
        }

        if (is_map) {
            const auto&       map_t = get<type::Map>(col_type.value);
            const type::Type& key_t = map_t.key;
            const type::Type& val_t = map_t.value;
            switch (patch.op) {
                case Op::Append: {
                    ColumnValue rhs_cv = co_await materialize_as_column_value(patch.value, col_type, ctx);
                    assert_true(type_matches_tag<DynamicMap<NestedColumnValue, NestedColumnValue>>(rhs_cv), "map compound op: planner should have rejected non-map RHS");
                    auto& rhs = get<DynamicMap<NestedColumnValue, NestedColumnValue>>(rhs_cv);
                    auto  cur = current_map();
                    for (auto it = rhs.begin(); it != rhs.end(); ++it) {
                        const auto& k = (*it).first;
                        const auto& v = (*it).second;
                        if (auto* slot = find(cur, k); slot) {
                            *slot = v;
                        } else {
                            insert(cur, k, v);
                        }
                    }
                    col_values[idx]  = ColumnValue{move(cur)};
                    col_present[idx] = length(get<DynamicMap<NestedColumnValue, NestedColumnValue>>(col_values[idx])) > 0;
                    co_return true;
                }
                case Op::Subtract: {
                    type::Type  keys_type = type::create_set(key_t);
                    ColumnValue rhs_cv    = co_await materialize_as_column_value(patch.value, keys_type, ctx);
                    assert_true(type_matches_tag<DynamicSet<NestedColumnValue>>(rhs_cv), "map subtract: planner should have rejected non-set-of-keys RHS");
                    auto& rhs_keys = get<DynamicSet<NestedColumnValue>>(rhs_cv);
                    auto  cur      = current_map();
                    for (auto it = rhs_keys.begin(); it != rhs_keys.end(); ++it) {
                        remove(cur, *it);
                    }
                    if (length(cur) == 0) {
                        col_present[idx] = false;
                    } else {
                        col_values[idx]  = ColumnValue{move(cur)};
                        col_present[idx] = true;
                    }
                    co_return true;
                }
                case Op::SubscriptSet: {
                    ColumnValue       k_cv = co_await materialize_as_column_value(patch.key, key_t, ctx);
                    NestedColumnValue k{move(k_cv)};
                    auto              cur = current_map();
                    if (eval_is_null(patch.value)) {
                        remove(cur, k);
                        if (length(cur) == 0) {
                            col_present[idx] = false;
                        } else {
                            col_values[idx]  = ColumnValue{move(cur)};
                            col_present[idx] = true;
                        }
                        co_return true;
                    }
                    ColumnValue       v_cv = co_await materialize_as_column_value(patch.value, val_t, ctx);
                    NestedColumnValue v{move(v_cv)};
                    if (auto* slot = find(cur, k); slot) {
                        *slot = v;
                    } else {
                        insert(cur, k, v);
                    }
                    col_values[idx]  = ColumnValue{move(cur)};
                    col_present[idx] = true;
                    co_return true;
                }
                case Op::SubscriptDelete: {
                    ColumnValue       k_cv = co_await materialize_as_column_value(patch.key, key_t, ctx);
                    NestedColumnValue k{move(k_cv)};
                    auto              cur = current_map();
                    remove(cur, k);
                    if (length(cur) == 0) {
                        col_present[idx] = false;
                    } else {
                        col_values[idx]  = ColumnValue{move(cur)};
                        col_present[idx] = true;
                    }
                    co_return true;
                }
                case Op::Prepend:
                    assert_not_implemented("map columns do not support prepend; planner should have rejected");
                    co_return false;
            }
        }

        assert_not_implemented("CollectionPatch on non-collection column; planner should have rejected");
        co_return false;
    }

    // @note an absent counter cell reads as 0 for `c = c + n` style RHS — primed
    // here so the TWI evaluation under `row_values` sees a defined value.
    // Updates the parallel `cell_meta` array so that any cell touched by an assignment,
    // collection patch, or column delete records `new_cell_meta` (or is cleared when the cell
    // becomes absent). Cells not in the spec keep their existing metadata — preserving USING TTL
    // semantics across partial updates.
    static coroutine::Task<void> apply_updates_to_row(const schema::Table* tbl, DynamicArray<ColumnValue>& col_values, DynamicArray<bool>& col_present, DynamicArray<Optional<io::CellMetadata>>& cell_meta, const io::CellMetadata& new_cell_meta, const planner::MutationSpec& spec, const EvalContext& ctx) {
        auto touch_cell = [&](U64 idx) {
            cell_meta[idx] = col_present[idx] ? Optional<io::CellMetadata>{new_cell_meta} : Optional<io::CellMetadata>{};
        };
        for (const auto& upd : spec.updates) {
            U64 idx = upd.col_idx;
            if (type_matches_tag<planner::CollectionPatch>(upd.new_value)) {
                co_await apply_collection_patch(tbl, idx, col_values, col_present, get<planner::CollectionPatch>(upd.new_value), ctx);
                touch_cell(idx);
                continue;
            }
            Evaluated eval;
            if (type_matches_tag<TermWithIdentifiers>(upd.new_value)) {
                bool is_counter = type_matches_tag<type::Basic>(tbl->cols[idx].type.value) && get<type::Basic>(tbl->cols[idx].type.value) == type::Basic::counter;
                if (is_counter && !col_present[idx]) {
                    col_values[idx]  = S64(0);
                    col_present[idx] = true;
                }
                EvalContext row_ctx = ctx;
                row_ctx.table       = tbl;
                row_ctx.row_values  = col_values.ptr;
                eval                = evaluate(get<TermWithIdentifiers>(upd.new_value), row_ctx);
            } else {
                eval = get<Evaluated>(upd.new_value);
            }
            // UNSET → column unchanged (Cassandra semantics for both counter and non-counter).
            if (type_matches_tag<Literal>(eval.value) && type_matches_tag<Unset>(get<Literal>(eval.value).value)) {
                continue;
            }
            if (type_matches_tag<ColumnValue>(eval.value)) {
                const ColumnValue& cv = get<ColumnValue>(eval.value);
                if (type_matches_tag<Null>(cv)) {
                    col_present[idx] = false;
                    touch_cell(idx);
                } else if (io::can_write_column_value(cv, tbl->cols[idx].type)) {
                    col_present[idx] = true;
                    col_values[idx]  = cv;
                    touch_cell(idx);
                }
            } else if (type_matches_tag<Literal>(eval.value) && type_matches_tag<Null>(get<Literal>(eval.value).value)) {
                col_present[idx] = false;
                touch_cell(idx);
            } else if (Optional<ColumnValue> resolved = io::resolve_evaluated(eval, tbl->cols[idx].type, ctx); resolved.has_value()) {
                col_present[idx] = true;
                col_values[idx]  = move(*resolved);
                touch_cell(idx);
            }
        }
    }

    // Rewrite the static page for a partition entry: a partition-key preamble (always
    // present, one io::write_column_value cell per partition-key column) followed by the
    // static-row-header section (RowMetadata + masks + cells) when the table has static
    // columns with values set. Reuses entry.static_page in place if already allocated, so
    // the page number — and any IndexEntry pointing at it — stays valid.
    // @note precondition: col_values/col_present must already have every partition-key
    // column populated — either injected from an existing partition's preamble (read via
    // read_row_into) or explicitly materialized by the caller for a brand-new partition.
    // Returns whether the table has any present static column value (callers use this,
    // not entry.static_page, to decide whether an otherwise-empty partition should be
    // dropped — the static page itself is now always present).
    static coroutine::Task<bool> rewrite_static(Engine& engine, schema::PartitionEntry& entry, const schema::Table* tbl, const DynamicArray<ColumnValue>& col_values, const DynamicArray<bool>& col_present, const DynamicArray<Optional<io::CellMetadata>>& cell_meta = {}) {
        bool any_static = false;
        for (U64 si : tbl->static_col_indices) {
            if (si < col_present.length && col_present[si]) {
                any_static = true;
                break;
            }
        }

        DynamicArray<U8> buf;
        auto             write_fn = io::sync_buffer_writer(buf);
        auto             write    = io::to_writer(write_fn);
        write_pk_preamble(write, tbl, col_values, col_present);

        if (any_static) {
            auto is_active = [&](U64 idx) {
                return idx < col_present.length && col_present[idx] && tbl->cols[idx].is_static;
            };
            auto has_cell_meta = [&](U64 idx) {
                return is_active(idx) && idx < cell_meta.length && cell_meta[idx].has_value();
            };
            // @note static blobs never carry row-level TTL but the header is always present for uniform reading.
            io::write_row_metadata(write, io::RowMetadata{});
            io::write_column_mask(write, io::to_checker(is_active), tbl->cols.length);
            io::write_mask_bits(write, io::to_checker(has_cell_meta), tbl->cols.length);
            for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                if (is_active(ci)) {
                    if (has_cell_meta(ci)) {
                        io::write_cell_metadata(write, *cell_meta[ci]);
                    }
                    io::write_column_value(write, col_values[ci], tbl->cols[ci].type);
                }
            }
        }

        entry.static_page = co_await write_blob_in_place(engine, entry.static_page, buf.ptr, buf.length);
        co_return any_static;
    }

    // Materialize partition-key column values into col_values/col_present so rewrite_static
    // can write the pk preamble for a brand-new partition (one that has no existing static
    // page to inject pk values from). locator.pk_evals is populated by the planner alongside
    // any partition-key equality restriction.
    static coroutine::Task<void> inject_pk_values(const schema::Table* tbl, const planner::RowLocator& locator, const EvalContext& ctx, DynamicArray<ColumnValue>& col_values, DynamicArray<bool>& col_present) {
        assert_true(locator.pk_evals.length == tbl->partition_key_col_indices.length, "pk_evals must match the partition key column count to create a new partition");
        for (U64 i = 0; i < tbl->partition_key_col_indices.length; i++) {
            U64 pk_ci          = tbl->partition_key_col_indices[i];
            col_values[pk_ci]  = co_await materialize_as_column_value(locator.pk_evals[i], tbl->cols[pk_ci].type, ctx);
            col_present[pk_ci] = true;
        }
    }

    // Read+apply core of a row's read-modify-write — shared by every UPDATE (clustering,
    // static-only, clustering-less), column-level DELETE, and INSERT read-modify-write path
    // in this file. Cassandra INSERT skips omitted/UNSET columns exactly like UPDATE, so
    // both need "keep what's not touched" rather than an overwrite of the row blob.
    // ck_bytes is empty (NO_CK_BYTES) for static-only and clustering-less callers — the
    // shared read_row_into/load underneath already treat that as a no-op, so one function
    // covers both shapes. Callers still own deriving the row's TTL metadata (INSERT and
    // UPDATE compute it differently — see any_regular_cell at the INSERT clustering-row call
    // site), writing the blob (or rewrite_static, for static-only callers), inserting the
    // row-page pointer if new, and maintaining indexes; those steps are cheap, near-identical
    // one-liners at each call site and don't carry the same duplication risk as the
    // read+apply sequence below.
    //
    // pk_inject_locator is non-null only when the caller must backfill partition-key
    // columns into a brand-new partition's row before indexing it (UPDATE: the static page
    // doesn't have the pk preamble yet when this runs; INSERT already wrote it via an
    // earlier rewrite_static call, so it passes nullptr — see the call site).
    struct RmwRowState {
        DynamicArray<ColumnValue>                old_cv, col_values;
        DynamicArray<bool>                       old_present, col_present;
        DynamicArray<Optional<io::CellMetadata>> cell_meta;
    };

    static coroutine::Task<RmwRowState> rmw_row(
        Engine& engine, const schema::Table* tbl,
        U64 existing_row_page, U64 static_page, TArrayView<const U8, U16> ck_bytes,
        const planner::RowLocator*   pk_inject_locator,
        const planner::MutationSpec& spec, const io::CellMetadata& new_cell_meta,
        bool have_indexes, const EvalContext& ctx
    ) {
        RmwRowState st;
        co_await read_row_into(engine, tbl, existing_row_page, static_page, st.col_values, st.col_present, nullptr, &st.cell_meta, ck_bytes);
        if (pk_inject_locator != nullptr) {
            co_await inject_pk_values(tbl, *pk_inject_locator, ctx, st.col_values, st.col_present);
        }
        st.old_cv      = have_indexes ? st.col_values : DynamicArray<ColumnValue>{};
        st.old_present = have_indexes ? st.col_present : DynamicArray<bool>{};
        co_await apply_updates_to_row(tbl, st.col_values, st.col_present, st.cell_meta, new_cell_meta, spec, ctx);
        co_return st;
    }

    // @note Each entry encodes one (element, pk, ck) tuple. For map indexes
    // the key half determines the element identity; for Entries kind we also
    // need the value bytes to form a unique entry per pair.
    struct CollectionIndexEntry {
        NestedColumnValue key;       // element (List/Set) or map key
        NestedColumnValue map_value; // map value (Entries/Values-on-map only)
        bool              has_map_value = false;
    };

    static DynamicArray<CollectionIndexEntry> enumerate_collection_entries(const ColumnValue& cv) {
        DynamicArray<CollectionIndexEntry> out;
        visit(cv, [&](const auto& v) {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                for (const auto& e : v) {
                    push_back(out, CollectionIndexEntry{e, NestedColumnValue{}, false});
                }
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                for (auto it = v.begin(); it != v.end(); ++it) {
                    push_back(out, CollectionIndexEntry{*it, NestedColumnValue{}, false});
                }
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                for (auto it = v.begin(); it != v.end(); ++it) {
                    push_back(out, CollectionIndexEntry{(*it).first, (*it).second, true});
                }
            }
        });
        return out;
    }

    static bool collection_entry_equal(const CollectionIndexEntry& a, const CollectionIndexEntry& b, schema::IndexKind kind) {
        switch (kind) {
            case schema::IndexKind::Values:
                if (a.has_map_value) {
                    return a.map_value == b.map_value;
                }
                return a.key == b.key;
            case schema::IndexKind::Keys:
                return a.key == b.key;
            case schema::IndexKind::Entries:
                return a.key == b.key && a.map_value == b.map_value;
            case schema::IndexKind::Full:
                return a.key == b.key && a.map_value == b.map_value;
        }
        return false;
    }

    static DynamicArray<U8> encode_collection_index_prefix(const CollectionIndexEntry& e, schema::IndexKind kind, const type::Type& col_type) {
        switch (kind) {
            case schema::IndexKind::Values: {
                if (type_matches_tag<type::List>(col_type.value)) {
                    auto& el = get<type::List>(col_type.value).element;
                    return key::encode_index_prefix_from_cv(e.key.value, get<type::Basic>(el.value));
                }
                if (type_matches_tag<type::Set>(col_type.value)) {
                    auto& el = get<type::Set>(col_type.value).key;
                    return key::encode_index_prefix_from_cv(e.key.value, get<type::Basic>(el.value));
                }
                // Map<K,V>: index the value half.
                auto& mv = get<type::Map>(col_type.value).value;
                return key::encode_index_prefix_from_cv(e.map_value.value, get<type::Basic>(mv.value));
            }
            case schema::IndexKind::Keys: {
                auto& mk = get<type::Map>(col_type.value).key;
                return key::encode_index_prefix_from_cv(e.key.value, get<type::Basic>(mk.value));
            }
            case schema::IndexKind::Entries: {
                const auto&      m       = get<type::Map>(col_type.value);
                DynamicArray<U8> out     = key::encode_index_prefix_from_cv(e.key.value, get<type::Basic>(m.key.value));
                DynamicArray<U8> val_buf = key::encode_index_prefix_from_cv(e.map_value.value, get<type::Basic>(m.value.value));
                for (U64 i = 0; i < val_buf.length; i++) {
                    push_back(out, val_buf[i]);
                }
                return out;
            }
            case schema::IndexKind::Full:
                return {};
        }
        return {};
    }

    static coroutine::Task<void> apply_index_entry_diff(
        schema::Index&                         idx,
        const type::Type&                      col_type,
        TArrayView<const CollectionIndexEntry> removed,
        TArrayView<const CollectionIndexEntry> added,
        TArrayView<const U8, U16>              pk_bytes,
        TArrayView<const U8, U16>              ck_bytes,
        schema::IndexEntry                     entry_value
    ) {
        for (const auto& e : removed) {
            DynamicArray<U8> prefix = encode_collection_index_prefix(e, idx.kind, col_type);
            DynamicArray<U8> ikey   = key::make_full_index_key(prefix, pk_bytes, ck_bytes);
            auto             kv     = TArrayView<const U8, U16>(ikey.ptr, static_cast<U16>(ikey.length));
            co_await btree::remove(idx.btree, kv);
        }
        for (const auto& e : added) {
            DynamicArray<U8> prefix = encode_collection_index_prefix(e, idx.kind, col_type);
            DynamicArray<U8> ikey   = key::make_full_index_key(prefix, pk_bytes, ck_bytes);
            auto             kv     = TArrayView<const U8, U16>(ikey.ptr, static_cast<U16>(ikey.length));
            co_await btree::tinsert(idx.btree, kv, entry_value);
        }
    }

    // @note empty old_cv/old_present means "row did not exist"; empty new_cv/new_present means "row was deleted".
    // entry_value is the {static_page,row_page} to store for any newly-added entry — the
    // current location of the row/partition after this mutation.
    static coroutine::Task<void> update_indexes(schema::Table* tbl, S64 pk_token, TArrayView<const U8, U16> ck_bytes, schema::IndexEntry entry_value, const DynamicArray<ColumnValue>& old_cv, const DynamicArray<bool>& old_present, const DynamicArray<ColumnValue>& new_cv, const DynamicArray<bool>& new_present) {
        if (tbl->indexes.length == 0) {
            co_return;
        }
        // Partition key values are present in whichever of old_cv/new_cv is populated (pk
        // never changes across an UPDATE); see encode_index_pk_component.
        const DynamicArray<ColumnValue>& pk_source = new_cv.length > 0 ? new_cv : old_cv;
        DynamicArray<U8>                 pk_bytes  = key::encode_index_pk_component(*tbl, pk_token, {pk_source.ptr, pk_source.length});
        auto                             pk_view   = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));

        for (auto& idx : tbl->indexes) {
            if (idx.tombstone) {
                continue;
            }
            U64               ci       = idx.col_idx;
            const type::Type& col_type = tbl->cols[ci].type;

            if (type_matches_tag<type::Basic>(col_type.value)) {
                type::Basic dtype   = get<type::Basic>(col_type.value);
                bool        old_has = ci < old_cv.length && old_present[ci] && !type_matches_tag<Null>(old_cv[ci]);
                bool        new_has = ci < new_cv.length && new_present[ci] && !type_matches_tag<Null>(new_cv[ci]);
                if (old_has) {
                    DynamicArray<U8> old_key = key::make_full_index_key(
                        key::encode_index_prefix_from_cv(old_cv[ci], dtype), pk_view, ck_bytes
                    );
                    auto kv = TArrayView<const U8, U16>(old_key.ptr, static_cast<U16>(old_key.length));
                    co_await btree::remove(idx.btree, kv);
                }
                if (new_has) {
                    DynamicArray<U8> new_key = key::make_full_index_key(
                        key::encode_index_prefix_from_cv(new_cv[ci], dtype), pk_view, ck_bytes
                    );
                    auto kv = TArrayView<const U8, U16>(new_key.ptr, static_cast<U16>(new_key.length));
                    co_await btree::tinsert(idx.btree, kv, entry_value);
                }
                continue;
            }

            bool                               old_has = ci < old_cv.length && old_present[ci] && !type_matches_tag<Null>(old_cv[ci]);
            bool                               new_has = ci < new_cv.length && new_present[ci] && !type_matches_tag<Null>(new_cv[ci]);
            DynamicArray<CollectionIndexEntry> old_entries =
                old_has ? enumerate_collection_entries(old_cv[ci]) : DynamicArray<CollectionIndexEntry>{};
            DynamicArray<CollectionIndexEntry> new_entries =
                new_has ? enumerate_collection_entries(new_cv[ci]) : DynamicArray<CollectionIndexEntry>{};

            DynamicArray<CollectionIndexEntry> removed;
            DynamicArray<CollectionIndexEntry> added;
            for (const auto& oe : old_entries) {
                bool present_in_new = false;
                for (const auto& ne : new_entries) {
                    if (collection_entry_equal(oe, ne, idx.kind)) {
                        present_in_new = true;
                        break;
                    }
                }
                if (!present_in_new) {
                    push_back(removed, oe);
                }
            }
            for (const auto& ne : new_entries) {
                bool present_in_old = false;
                for (const auto& oe : old_entries) {
                    if (collection_entry_equal(ne, oe, idx.kind)) {
                        present_in_old = true;
                        break;
                    }
                }
                if (!present_in_old) {
                    push_back(added, ne);
                }
            }

            co_await apply_index_entry_diff(
                idx, col_type,
                TArrayView<const CollectionIndexEntry>(removed.ptr, removed.length),
                TArrayView<const CollectionIndexEntry>(added.ptr, added.length),
                pk_view, ck_bytes, entry_value
            );
        }
    }

    static coroutine::Task<void> backfill_index(Engine& engine, schema::Table* tbl, schema::Index& idx) {
        const type::Type& col_type    = tbl->cols[idx.col_idx].type;
        bool              is_basic    = type_matches_tag<type::Basic>(col_type.value);
        type::Basic       basic_dtype = is_basic ? get<type::Basic>(col_type.value) : type::Basic::int_;
        U64               ci          = idx.col_idx;

        auto part_it  = co_await btree::begin<schema::PartitionEntry>(tbl->btree);
        auto part_end = btree::end<schema::PartitionEntry>(tbl->btree);

        while (part_it != part_end) {
            S64 pk_token = part_it.key();

            schema::PartitionEntry entry = *part_it;

            if (schema::has_clustering_keys(*tbl)) {
                schema::ClusteringBTree ck_btree{
                    engine.pager, entry.data_page,
                    schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                };
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
                    co_await read_row_into(engine, tbl, *ck_it, entry.static_page, cv, present, nullptr, nullptr, ck_bytes);

                    if (ci < cv.length && present[ci] && !type_matches_tag<Null>(cv[ci])) {
                        DynamicArray<U8>   pk_bytes    = key::encode_index_pk_component(*tbl, pk_token, {cv.ptr, cv.length});
                        auto               pk_view     = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                        schema::IndexEntry entry_value = {entry.static_page, *ck_it};
                        if (is_basic) {
                            DynamicArray<U8> ikey = key::make_full_index_key(
                                key::encode_index_prefix_from_cv(cv[ci], basic_dtype), pk_view, ck_bytes
                            );
                            auto kv = TArrayView<const U8, U16>(ikey.ptr, static_cast<U16>(ikey.length));
                            co_await btree::tinsert(idx.btree, kv, entry_value);
                        } else {
                            auto entries = enumerate_collection_entries(cv[ci]);
                            co_await apply_index_entry_diff(
                                idx, col_type,
                                TArrayView<const CollectionIndexEntry>{},
                                TArrayView<const CollectionIndexEntry>(entries.ptr, entries.length),
                                pk_view, ck_bytes, entry_value
                            );
                        }
                    }
                    co_await ck_it.advance();
                }
            } else {
                TArrayView<const U8, U16> ck_bytes{nullptr, 0};
                DynamicArray<ColumnValue> cv;
                DynamicArray<bool>        present;
                co_await read_row_into(engine, tbl, entry.data_page, entry.static_page, cv, present);

                if (ci < cv.length && present[ci] && !type_matches_tag<Null>(cv[ci])) {
                    DynamicArray<U8>   pk_bytes    = key::encode_index_pk_component(*tbl, pk_token, {cv.ptr, cv.length});
                    auto               pk_view     = TArrayView<const U8, U16>(pk_bytes.ptr, static_cast<U16>(pk_bytes.length));
                    schema::IndexEntry entry_value = {entry.static_page, entry.data_page};
                    if (is_basic) {
                        DynamicArray<U8> ikey = key::make_full_index_key(
                            key::encode_index_prefix_from_cv(cv[ci], basic_dtype), pk_view, ck_bytes
                        );
                        auto kv = TArrayView<const U8, U16>(ikey.ptr, static_cast<U16>(ikey.length));
                        co_await btree::tinsert(idx.btree, kv, entry_value);
                    } else {
                        auto entries = enumerate_collection_entries(cv[ci]);
                        co_await apply_index_entry_diff(
                            idx, col_type,
                            TArrayView<const CollectionIndexEntry>{},
                            TArrayView<const CollectionIndexEntry>(entries.ptr, entries.length),
                            pk_view, ck_bytes, entry_value
                        );
                    }
                }
            }
            co_await part_it.advance();
        }
    }

    static coroutine::Task<RowRange> create_table_range_it(Engine& engine, schema::Table* tbl, const planner::RowLocator& locator) {
        auto copy_ck_bounds = [&](RowIterator& it) {
            it.ck                 = locator.ck;
            it.reverse_clustering = locator.reverse_clustering;
        };

        RowIterator start_it, stop_it;
        bool        has_ck_bounds = locator.ck.has_begin || locator.ck.has_end;

        if (locator.pk.is_equality) {
            start_it = co_await create_table_eq_it(engine.pager, tbl, locator.pk.begin);
            stop_it  = create_table_end_it(engine.pager, tbl);
            if (start_it != stop_it) {
                stop_it = co_await create_table_le_it(engine.pager, tbl, locator.pk.begin);
            }
        } else {
            if (locator.pk.has_begin) {
                if (locator.pk.begin_inclusive) {
                    start_it = co_await create_table_ge_it(engine.pager, tbl, locator.pk.begin);
                } else {
                    start_it = co_await create_table_gt_it(engine.pager, tbl, locator.pk.begin);
                }
            } else {
                start_it = co_await create_table_begin_it(engine.pager, tbl);
            }
            if (locator.pk.has_end) {
                if (locator.pk.end_inclusive) {
                    stop_it = co_await create_table_le_it(engine.pager, tbl, locator.pk.end);
                } else {
                    stop_it = co_await create_table_lt_it(engine.pager, tbl, locator.pk.end);
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

    // @note only text-column equality is applied; other predicate shapes pass through.
    static void apply_virtual_where(VirtualRows& vr, const WhereClause& where, const EvalContext& ctx) {
        auto matches_row = [&](const VirtualRow& row) -> bool {
            for (const auto& rel : where.relations) {
                bool ok = visit(rel.value, [&](const auto& r) -> bool {
                    using RT = RemoveCVRef<decltype(r)>;
                    if constexpr (SameAs<RT, WhereClause::ColumnExpressionRelation>) {
                        if (r.operator_ != Operator::eq) {
                            return true;
                        }
                        U64 col_idx = MAX_U64;
                        for (U64 i = 0; i < vr.columns.length; i++) {
                            if (vr.columns[i].name == String8(r.column.identifier.c_str, r.column.identifier.length)) {
                                col_idx = i;
                                break;
                            }
                        }
                        if (col_idx == MAX_U64) {
                            return true;
                        }
                        Evaluated eval = evaluate(r.value, ctx);
                        if (!type_matches_tag<Literal>(eval.value)) {
                            return true;
                        }
                        const auto& con = get<Literal>(eval.value);
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
        for (auto& row : vr.rows) {
            if (matches_row(row)) {
                push_back(kept, move(row));
            }
        }
        vr.rows = move(kept);
    }

    static ExecutionResult create_void_success() {
        return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
    }
    // @note msg should be static storage duration
    static ExecutionResult create_server_error(const char* msg) {
        return {.status = ExecutionStatus::ServerError, .message = msg};
    }
    static ExecutionResult create_invalid(AutoString8 msg) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.message_storage = move(msg);
        r.message         = String8(r.message_storage);
        return r;
    }
    static ExecutionResult create_system_keyspace_invalid() {
        return {.status = ExecutionStatus::Invalid, .message = "system keyspaces cannot be modified"};
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
        r.message         = String8(r.message_storage);
        return r;
    }
    static ExecutionResult create_no_keyspace_specified() {
        return {.status = ExecutionStatus::Invalid, .message = "No keyspace has been specified. USE a keyspace, or explicitly specify keyspace.tablename"};
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
        r.message         = String8(r.message_storage);
        return r;
    }
    static String8 resolve_ks_name(const Optional<AutoString8>& stmt_ks_name, String8 current_keyspace) {
        return stmt_ks_name ? String8(*stmt_ks_name) : current_keyspace;
    }

    struct KsResolution {
        schema::Keyspace*         ks = nullptr;
        Optional<ExecutionResult> error;
        bool                      not_found = false; // true only when error is specifically "keyspace not found"
    };

    // Validates ks_name (non-empty, non-system) and looks it up; on failure `error` is set
    // and `ks` is null. `not_found` distinguishes the IF EXISTS-suppressible case from the
    // empty-name/system-keyspace validation errors, which are not suppressible.
    static KsResolution resolve_ks(Engine& engine, String8 ks_name) {
        if (ks_name.length == 0) {
            return {nullptr, create_no_keyspace_specified(), false};
        }
        if (is_system_keyspace(ks_name)) {
            return {nullptr, create_system_keyspace_invalid(), false};
        }
        auto ks = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return {nullptr, create_keyspace_not_found(ks_name), true};
        }
        return {ks, {}, false};
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
        r.message         = String8(r.message_storage);
        return r;
    }
    static ExecutionResult create_insert_duplicate_column(const String8& keyspace_name, const String8& table_name, const String8& col_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Multiple definitions of identifier " + AutoString8(col_name);
        r.message         = String8(r.message_storage);
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
        r.message         = String8(r.message_storage);
        return r;
    }
    static ExecutionResult create_insert_missing_ck(const String8& keyspace_name, const String8& table_name, const String8& col_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Missing mandatory PRIMARY KEY part " + AutoString8(col_name);
        r.message         = String8(r.message_storage);
        return r;
    }
    static ExecutionResult create_unset_on_key_column(const String8& keyspace_name, const String8& table_name, const String8& col_name) {
        ExecutionResult r;
        r.status          = ExecutionStatus::Invalid;
        r.keyspace        = AutoString8(keyspace_name);
        r.table           = AutoString8(table_name);
        r.message_storage = "Invalid unset value for column " + AutoString8(col_name);
        r.message         = String8(r.message_storage);
        return r;
    }
    static Optional<ExecutionResult> create_error_if_plan_invalid(const planner::PlanResult& result) {
        switch (result.error) {
            case planner::PlanError::None:
                return {};
            case planner::PlanError::RequiresAllowFiltering:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "Cannot execute this query as it might involve data filtering and thus may have unpredictable performance. If you want to execute this query despite the performance unpredictability, use ALLOW FILTERING",
                };
            case planner::PlanError::ClusteringRestrictedAfterNonEq:
            case planner::PlanError::ClusteringRestrictedWithoutPrefix: {
                return create_invalid(AutoString8(result.context));
            }
            case planner::PlanError::MissingPartitionKey: {
                return create_invalid("Some partition key parts are missing: " + result.context);
            }
            case planner::PlanError::MissingClusteringKey: {
                return create_invalid("Some clustering keys are missing: " + result.context);
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
                return create_invalid("Order by is currently only supported on the clustered columns of the PRIMARY KEY, got " + result.context);
            }
            case planner::PlanError::ColumnNotFound: {
                return create_invalid("Undefined column name " + result.context);
            }
            case planner::PlanError::TypeMismatch: {
                return create_invalid(result.context.length > 0 ? AutoString8(result.context) : AutoString8("Type mismatch for key column"));
            }
            case planner::PlanError::TokenFunctionInMutation:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "The token function cannot be used in WHERE clauses for UPDATE",
                };
            case planner::PlanError::DuplicateColumnInMutation: {
                return create_invalid("Column " + result.context + " is assigned twice in UPDATE");
            }
            case planner::PlanError::NonKeyColumnInMutationWhere: {
                return create_invalid("Non PRIMARY KEY columns found in where clause: " + result.context);
            }
            case planner::PlanError::NonEqInOnPartitionKeyMutation: {
                ExecutionResult r;
                r.status = ExecutionStatus::Invalid;
                if (result.context.length > 0) {
                    r.message_storage = AutoString8("Only EQ and IN relation are supported on the partition key (unless you use the token() function) — ") + result.context + " is not supported";
                } else {
                    r.message_storage = AutoString8("Only EQ and IN relation are supported on the partition key (unless you use the token() function)");
                }
                r.message = String8(r.message_storage);
                return r;
            }
            case planner::PlanError::CounterOperationOnNonCounter: {
                return create_invalid("Cannot apply counter operations on non-counter column " + result.context);
            }
            case planner::PlanError::CounterAssignmentNotIncrement: {
                return create_invalid("Invalid operation for counter column " + result.context);
            }
            case planner::PlanError::NullValueForCounter:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "Invalid null value for counter increment/decrement",
                };
            case planner::PlanError::DistinctRestrictionInvalid:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "SELECT DISTINCT with WHERE clause only supports restriction by partition key and/or static columns.",
                };
            case planner::PlanError::InvalidCollectionMutation: {
                return create_invalid(result.context);
            }
            case planner::PlanError::InvalidSubscriptTarget: {
                return create_invalid(result.context);
            }
            case planner::PlanError::UnsetSubscriptValue: {
                return create_invalid(AutoString8("Invalid unset value for argument in call to subscript on ") + result.context);
            }
            case planner::PlanError::UnsetValueInWhere:
                return ExecutionResult{
                    .status  = ExecutionStatus::Invalid,
                    .message = "Invalid unset value in where clause",
                };
            case planner::PlanError::InvalidTtlArgument:
            case planner::PlanError::InvalidWritetimeArgument: {
                return create_invalid(result.context);
            }
        }
        return {};
    }

    struct IndexRowHit {
        DynamicArray<ColumnValue> cv;
        DynamicArray<bool>        present;
    };

    // Reads the row an index entry points to (IndexEntry already carries static_page/
    // row_page directly, so this needs no partition/clustering BTree lookup) and applies
    // the row's TTL expiry check. Returns nullopt if the entry has no data or is expired.
    static coroutine::Task<Optional<IndexRowHit>> read_index_hit_row(
        Engine& engine, const schema::Table* tbl, const schema::Index& idx,
        TArrayView<const U8, U16> key_view, schema::IndexEntry entry, S64 now_unix_ms
    ) {
        if (entry.row_page == 0 && entry.static_page == 0) {
            co_return {};
        }
        U16              ck_start = key::index_key_ck_start(*tbl, idx, key_view);
        U16              ck_len   = key_view.length > ck_start ? static_cast<U16>(key_view.length - ck_start) : 0;
        DynamicArray<U8> ck_buf;
        if (ck_len > 0) {
            resize(ck_buf, U64(ck_len));
            os::memory_copy(ck_buf.ptr, key_view.ptr + ck_start, ck_len);
        }
        IndexRowHit     hit;
        io::RowMetadata row_meta;
        auto            ck_v = TArrayView<const U8, U16>{ck_buf.ptr, ck_len};
        co_await read_row_into(engine, tbl, entry.row_page, entry.static_page, hit.cv, hit.present, &row_meta, nullptr, ck_v);
        if (io::row_is_expired(row_meta, now_unix_ms)) {
            co_return {};
        }
        co_return move(hit);
    }

    // @note caller must have set sp.locator.index_col_idx.
    static coroutine::Task<ExecutionResult> execute_select_index(
        Engine& engine, schema::Table* tbl, String8 ks_name, String8 table_name,
        const planner::SelectPlan& sp, DynamicArray<U64> select_col_indices,
        DynamicArray<Optional<AutoString8>> select_col_aliases,
        U64 limit_count, EvalContext ctx
    ) {
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

        DynamicArray<U64> col_order = build_select_col_order(*tbl, select_col_indices);
        VirtualRows       vr        = make_virtual_rows_shell(*tbl, ks_name, table_name, col_order, select_col_aliases);

        const DynamicArray<U8>& prefix      = sp.locator.index_key_prefix;
        auto                    prefix_view = TArrayView<const U8, U16>(prefix.ptr, static_cast<U16>(prefix.length));
        auto                    idx_it      = co_await btree::find_it<schema::IndexEntry, btree::SearchStrategy::FirstGreaterEqual>(
            active_idx->btree, prefix_view
        );
        auto idx_end = btree::end<schema::IndexEntry>(active_idx->btree);

        U64 row_count   = 0;
        S64 now_unix_ms = S64(os::unix_ms_now());
        while (idx_it != idx_end && row_count < limit_count) {
            auto key_view = idx_it.key();
            if (key_view.length < prefix_view.length || os::memory_compare(key_view.ptr, prefix_view.ptr, prefix_view.length) != 0) {
                break;
            }

            schema::IndexEntry entry = *idx_it;
            auto               hit   = co_await read_index_hit_row(engine, tbl, *active_idx, key_view, entry, now_unix_ms);
            if (hit) {
                EvalContext row_ctx = ctx;
                row_ctx.table       = tbl;
                row_ctx.row_values  = hit->cv.ptr;
                if (evaluate_where(sp.filter.predicates, row_ctx)) {
                    push_back(vr.rows, project_virtual_row(col_order, hit->cv, hit->present));
                    row_count++;
                }
            }
            co_await idx_it.advance();
        }

        co_return ExecutionResult{
            .status       = ExecutionStatus::Success,
            .kind         = ResultKind::VirtualRows,
            .keyspace     = AutoString8(ks_name),
            .table        = AutoString8(table_name),
            .virtual_rows = move(vr),
        };
    }

    // @profile materializes all matching rows then sorts; LIMIT applied last.
    static coroutine::Task<ExecutionResult> execute_select_pk_in_ordered(
        Engine& engine, schema::Table* tbl, String8 ks_name, String8 table_name,
        const planner::SelectPlan& sp, DynamicArray<U64> select_col_indices,
        DynamicArray<Optional<AutoString8>> select_col_aliases,
        U64 limit_count, EvalContext ctx
    ) {
        DynamicArray<U64> col_order = build_select_col_order(*tbl, select_col_indices);
        VirtualRows       vr        = make_virtual_rows_shell(*tbl, ks_name, table_name, col_order, select_col_aliases);

        struct CollectedRow {
            DynamicArray<U8>          ck_bytes;
            DynamicArray<ColumnValue> values;
        };
        DynamicArray<CollectedRow> collected;
        S64                        now_unix_ms = S64(os::unix_ms_now());

        for (S64 pk_token : sp.locator.pk.in_values) {
            auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_token);
            if (!entry_opt) {
                continue;
            }

            if (schema::has_clustering_keys(*tbl)) {
                schema::ClusteringBTree ck_btree{
                    engine.pager, entry_opt->data_page,
                    schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                };
                auto ck_it  = co_await btree::begin<U64>(ck_btree);
                auto ck_end = btree::end<U64>(ck_btree);
                while (ck_it != ck_end) {
                    auto             ck_view = ck_it.key();
                    DynamicArray<U8> ck_buf;
                    resize(ck_buf, U64(ck_view.length));
                    os::memory_copy(ck_buf.ptr, ck_view.ptr, ck_view.length);

                    DynamicArray<ColumnValue> cv;
                    DynamicArray<bool>        present;
                    io::RowMetadata           row_meta;
                    auto                      ck_v = TArrayView<const U8, U16>{ck_buf.ptr, static_cast<U16>(ck_buf.length)};
                    co_await read_row_into(engine, tbl, *ck_it, entry_opt->static_page, cv, present, &row_meta, nullptr, ck_v);
                    if (io::row_is_expired(row_meta, now_unix_ms)) {
                        co_await ck_it.advance();
                        continue;
                    }

                    EvalContext row_ctx = ctx;
                    row_ctx.table       = tbl;
                    row_ctx.row_values  = cv.ptr;
                    if (evaluate_where(sp.filter.predicates, row_ctx)) {
                        VirtualRow vrow = project_virtual_row(col_order, cv, present);
                        push_back(collected, CollectedRow{move(ck_buf), move(vrow.values)});
                    }
                    co_await ck_it.advance();
                }
            } else if (entry_opt->data_page != 0 || entry_opt->static_page != 0) {
                DynamicArray<ColumnValue> cv;
                DynamicArray<bool>        present;
                io::RowMetadata           row_meta;
                co_await read_row_into(engine, tbl, entry_opt->data_page, entry_opt->static_page, cv, present, &row_meta);
                if (io::row_is_expired(row_meta, now_unix_ms)) {
                    continue;
                }
                EvalContext row_ctx = ctx;
                row_ctx.table       = tbl;
                row_ctx.row_values  = cv.ptr;
                if (evaluate_where(sp.filter.predicates, row_ctx)) {
                    VirtualRow vrow = project_virtual_row(col_order, cv, present);
                    push_back(collected, CollectedRow{{}, move(vrow.values)});
                }
            }
        }

        bool reverse = sp.locator.reverse_clustering;
        auto ck_lt   = [tbl, reverse](const CollectedRow& x, const CollectedRow& y) {
            auto     a   = TArrayView<const U8, U16>(x.ck_bytes.ptr, static_cast<U16>(x.ck_bytes.length));
            auto     b   = TArrayView<const U8, U16>(y.ck_bytes.ptr, static_cast<U16>(y.ck_bytes.length));
            Ordering ord = key::compare_clustering(*tbl, a, b);
            return reverse ? ord == Ordering::Greater : ord == Ordering::Less;
        };
        support::sort::sort<CollectedRow, U64>(collected, ck_lt);

        U64 cap = min(collected.length, limit_count);
        for (U64 i = 0; i < cap; i++) {
            VirtualRow vrow;
            vrow.values = move(collected[i].values);
            push_back(vr.rows, move(vrow));
        }

        co_return ExecutionResult{
            .status       = ExecutionStatus::Success,
            .kind         = ResultKind::VirtualRows,
            .keyspace     = AutoString8(ks_name),
            .table        = AutoString8(table_name),
            .virtual_rows = move(vr),
        };
    }

    // @note partitions are iterated in IN-list input order to match the order
    // Cassandra returns for SELECT...WHERE pk IN (...). The BTree's natural
    // order is Murmur3 token order, which differs from input order.
    static coroutine::Task<ExecutionResult> execute_select_pk_in(
        Engine& engine, schema::Table* tbl, String8 ks_name, String8 table_name,
        const Select& stmt, const planner::SelectPlan& sp,
        DynamicArray<U64>                   select_col_indices,
        DynamicArray<Optional<AutoString8>> select_col_aliases,
        U64 limit_count, EvalContext ctx
    ) {
        DynamicArray<U64> col_order   = build_select_col_order(*tbl, select_col_indices);
        VirtualRows       vr          = make_virtual_rows_shell(*tbl, ks_name, table_name, col_order, select_col_aliases);
        bool              is_distinct = static_cast<bool>(stmt.transform);
        S64               now_unix_ms = S64(os::unix_ms_now());

        for (const auto& pk_bytes : sp.locator.pk.in_values) {
            if (vr.rows.length >= limit_count) {
                break;
            }
            planner::RowLocator one_locator = sp.locator;
            one_locator.pk.is_equality      = true;
            one_locator.pk.has_in           = false;
            one_locator.pk.begin            = pk_bytes;
            one_locator.pk.has_begin        = true;
            one_locator.pk.begin_inclusive  = true;
            auto range                      = co_await create_table_range_it(engine, tbl, one_locator);

            auto advance = [&]() -> coroutine::Task<void> {
                if (is_distinct) {
                    co_await range.start.advance_partition();
                } else {
                    co_await range.start.advance(range.stop);
                }
            };

            while (range.start != range.stop && vr.rows.length < limit_count) {
                ColumnRange col_range = co_await range.start.deref();
                if (io::row_is_expired(col_range.start.metadata, now_unix_ms)) {
                    co_await advance();
                    continue;
                }

                DynamicArray<ColumnValue> row_values;
                DynamicArray<bool>        row_present;
                co_await drain_columns(tbl, col_range.start, row_values, row_present);

                bool pass = true;
                if (sp.filter.predicates.length > 0) {
                    EvalContext row_ctx = ctx;
                    row_ctx.table       = tbl;
                    row_ctx.row_values  = row_values.ptr;
                    pass                = evaluate_where(sp.filter.predicates, row_ctx);
                }
                if (pass) {
                    push_back(vr.rows, project_virtual_row(col_order, row_values, row_present));
                }

                co_await advance();
            }
        }

        co_return ExecutionResult{
            .status       = ExecutionStatus::Success,
            .kind         = ResultKind::VirtualRows,
            .keyspace     = AutoString8(ks_name),
            .table        = AutoString8(table_name),
            .virtual_rows = move(vr),
        };
    }

    // @note `decode_to_s32` distinguishes ColumnValue's storage tag for the integer-shaped
    // CQL types: int/date are tagged S32, the rest (incl. smallint/tinyint) tag as S64.
    struct IntConvType {
        type::Basic type;
        U8          bytes;
        bool        decode_to_s32;
    };
    static constexpr IntConvType kIntConvTypes[] = {
        {   type::Basic::bigint, 8, false},
        {type::Basic::timestamp, 8, false},
        {  type::Basic::counter, 8, false},
        {     type::Basic::time, 8, false},
        {     type::Basic::int_, 4,  true},
        {     type::Basic::date, 4,  true},
        { type::Basic::smallint, 2, false},
        {  type::Basic::tinyint, 1, false},
    };
    static Optional<IntConvType> int_conv_for(type::Basic t) {
        for (const auto& e : kIntConvTypes) {
            if (e.type == t) {
                return e;
            }
        }
        return {};
    }
    static U64 read_be_u64(const U8* src, U64 n) {
        U64 acc = 0;
        for (U64 i = 0; i < n; i++) {
            acc = (acc << 8) | src[i];
        }
        return acc;
    }
    static void write_be_u64(U8* dst, U64 v, U64 n) {
        for (U64 i = 0; i < n; i++) {
            dst[n - 1 - i] = static_cast<U8>((v >> (i * 8)) & 0xff);
        }
    }
    static S64 sign_extend_be(U64 raw, U64 bytes) {
        U64 width = bytes * 8;
        U64 sign  = 1ULL << (width - 1);
        if (raw & sign) {
            raw |= ~((1ULL << width) - 1ULL);
        }
        return static_cast<S64>(raw);
    }
    // @note returns Null on a type mismatch (e.g. wrong blob length, unsupported pair).
    static ColumnValue apply_typed_conversion(ColumnValue v, type::Basic from, type::Basic to) {
        if (type_matches_tag<Null>(v) || from == to) {
            return v;
        }
        if (to == type::Basic::blob) {
            auto info = int_conv_for(from);
            if (!info) {
                return ColumnValue{Null{}};
            }
            S64 iv;
            if (type_matches_tag<S64>(v)) {
                iv = get<S64>(v);
            } else if (type_matches_tag<S32>(v)) {
                iv = S64(get<S32>(v));
            } else {
                return ColumnValue{Null{}};
            }
            DynamicArray<U8> buf;
            resize(buf, U64(info->bytes));
            write_be_u64(buf.ptr, static_cast<U64>(iv), info->bytes);
            return ColumnValue{Blob{move(buf)}};
        }
        if (from == type::Basic::blob && type_matches_tag<Blob>(v)) {
            auto info = int_conv_for(to);
            if (!info) {
                return ColumnValue{Null{}};
            }
            const auto& blob = get<Blob>(v);
            if (blob.value.length != info->bytes) {
                return ColumnValue{Null{}};
            }
            S64 iv = sign_extend_be(read_be_u64(blob.value.ptr, info->bytes), info->bytes);
            return info->decode_to_s32 ? ColumnValue{static_cast<S32>(iv)} : ColumnValue{iv};
        }
        return ColumnValue{Null{}};
    }

    static ColumnValue apply_conversion_stack(ColumnValue v, const DynamicArray<planner::SelectOp::Conversion>& conversions) {
        for (const auto& c : conversions) {
            v = apply_typed_conversion(move(v), c.from, c.to);
        }
        return v;
    }

    // Convert a registry-result Evaluated into a ColumnValue narrowed to the
    // declared SELECT-projection return type. The registry returns Evaluated
    // values whose inner Literal uses S64/F64/etc.; the projection frame
    // expects S32/S16/F32 for the corresponding narrower CQL types.
    static ColumnValue narrow_evaluated(const Evaluated& e, type::Basic target) {
        if (type_matches_tag<ColumnValue>(e.value)) {
            return get<ColumnValue>(e.value);
        }
        if (!type_matches_tag<Literal>(e.value)) {
            return ColumnValue{Null{}};
        }
        return visit(get<Literal>(e.value).value, [&](const auto& v) -> ColumnValue {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, Null> || SameAs<T, Unset>) {
                return ColumnValue{Null{}};
            } else if constexpr (SameAs<T, S64>) {
                if (target == type::Basic::int_ || target == type::Basic::date) {
                    return ColumnValue{S32(v)};
                }
                if (target == type::Basic::smallint) {
                    return ColumnValue{S16(v)};
                }
                if (target == type::Basic::tinyint) {
                    return ColumnValue{U8(S8(v))};
                }
                return ColumnValue{v};
            } else if constexpr (SameAs<T, F64>) {
                if (target == type::Basic::float_) {
                    return ColumnValue{F32(v)};
                }
                return ColumnValue{v};
            } else if constexpr (SameAs<T, bool>) {
                return ColumnValue{U8(v ? 1 : 0)};
            } else if constexpr (SameAs<T, Hex>) {
                Blob b{};
                b.value = v.value;
                return ColumnValue{move(b)};
            } else {
                return ColumnValue{v};
            }
        });
    }

    static VirtualRow project_row_via_ops(const schema::Table& tbl, const DynamicArray<planner::SelectOp>& ops, const DynamicArray<ColumnValue>& cv, const DynamicArray<bool>& present, const DynamicArray<Optional<io::CellMetadata>>& cell_meta, S64 now_unix_ms, const EvalContext& base_ctx) {
        VirtualRow vrow;
        for (const auto& op : ops) {
            ColumnValue base;
            bool        is_null = false;
            visit(op.value, [&](const auto& o) {
                using OT = RemoveCVRef<decltype(o)>;
                if constexpr (SameAs<OT, planner::SelectOp::ColumnRef>) {
                    if (present[o.col_idx]) {
                        base = cv[o.col_idx];
                    } else {
                        is_null = true;
                    }
                } else if constexpr (SameAs<OT, planner::SelectOp::TtlOf>) {
                    if (present[o.col_idx] && cell_meta[o.col_idx] && io::cell_has_ttl(*cell_meta[o.col_idx])) {
                        S64 remain = (cell_meta[o.col_idx]->expiry_unix_ms - now_unix_ms) / 1000;
                        base       = ColumnValue{S32(max(S64(0), remain))};
                    } else {
                        is_null = true;
                    }
                } else if constexpr (SameAs<OT, planner::SelectOp::WritetimeOf>) {
                    if (present[o.col_idx] && cell_meta[o.col_idx] && io::cell_has_writetime(*cell_meta[o.col_idx])) {
                        base = ColumnValue{cell_meta[o.col_idx]->writetime_us};
                    } else {
                        is_null = true;
                    }
                } else if constexpr (SameAs<OT, planner::SelectOp::Token>) {
                    base = ColumnValue{key::compute_partition_token(tbl, {cv.ptr, cv.length})};
                } else if constexpr (SameAs<OT, planner::SelectOp::FuncCall>) {
                    DynamicArray<Evaluated> args;
                    for (const auto& a : o.args) {
                        if (type_matches_tag<U64>(a.value)) {
                            U64 ci = get<U64>(a.value);
                            if (ci < present.length && present[ci]) {
                                push_back(args, Evaluated{cv[ci]});
                            } else {
                                push_back(args, Evaluated{Literal{Null{}}});
                            }
                        } else {
                            push_back(args, Evaluated{get<Literal>(a.value)});
                        }
                    }
                    EvalContext rc = base_ctx;
                    rc.table       = &tbl;
                    rc.row_values  = cv.ptr;
                    Evaluated res  = call_registered_function(String8(o.name.c_str, o.name.length), {args.ptr, args.length}, rc);
                    base           = narrow_evaluated(res, o.return_type);
                } else if constexpr (SameAs<OT, planner::SelectOp::CountStar>) {
                    assert_not_implemented("CountStar reached project_row_via_ops; aggregate path should handle it");
                }
            });
            if (is_null) {
                push_back(vrow.values, ColumnValue{Null{}});
            } else {
                push_back(vrow.values, apply_conversion_stack(move(base), op.conversions));
            }
        }
        return vrow;
    }

    // Build the VirtualRows header from select ops. The order of ops corresponds to the
    // order of stmt.select.clauses; labels honor AS. Owned labels live in vr.column_name_storage
    // so the String8 views in vr.columns stay valid for the lifetime of the VirtualRows.
    static VirtualRows make_virtual_rows_shell_from_ops(const schema::Table& tbl, String8 ks_name, String8 table_name, const Select& stmt, const planner::ProjectionPlan& projection) {
        VirtualRows vr;
        vr.keyspace = AutoString8(ks_name);
        vr.table    = AutoString8(table_name);
        // @note column_name_storage views are taken into vr.columns; reserve up-front so push_back never reallocates.
        reserve(vr.column_name_storage, projection.ops.length);
        auto add_owned_column = [&](AutoString8 label, type::Type t) {
            push_back(vr.column_name_storage, move(label));
            const auto& s = vr.column_name_storage[vr.column_name_storage.length - 1];
            push_back(vr.columns, VirtualColumn{String8(s.c_str, s.length), t});
        };
        for (U64 i = 0; i < projection.ops.length; i++) {
            const auto& op        = projection.ops[i];
            bool        has_alias = (i < stmt.select.clauses.length && stmt.select.clauses[i].as.has_value());
            type::Type  out_type;
            AutoString8 default_label;
            visit(op.value, [&](const auto& o) {
                using OT = RemoveCVRef<decltype(o)>;
                if constexpr (SameAs<OT, planner::SelectOp::ColumnRef>) {
                    out_type      = tbl.cols[o.col_idx].type;
                    default_label = AutoString8(tbl.cols[o.col_idx].name);
                } else if constexpr (SameAs<OT, planner::SelectOp::TtlOf>) {
                    out_type      = type::create_basic(type::Basic::int_);
                    default_label = AutoString8("ttl(") + AutoString8(tbl.cols[o.col_idx].name) + AutoString8(")");
                } else if constexpr (SameAs<OT, planner::SelectOp::WritetimeOf>) {
                    out_type      = type::create_basic(type::Basic::bigint);
                    default_label = AutoString8("writetime(") + AutoString8(tbl.cols[o.col_idx].name) + AutoString8(")");
                } else if constexpr (SameAs<OT, planner::SelectOp::Token>) {
                    out_type      = type::create_basic(type::Basic::bigint);
                    default_label = AutoString8("system.token(");
                    for (U64 ti = 0; ti < o.pk_col_indices.length; ti++) {
                        if (ti > 0) {
                            default_label = default_label + AutoString8(", ");
                        }
                        default_label = default_label + AutoString8(tbl.cols[o.pk_col_indices[ti]].name);
                    }
                    default_label = default_label + AutoString8(")");
                } else if constexpr (SameAs<OT, planner::SelectOp::FuncCall>) {
                    out_type      = type::create_basic(o.return_type);
                    default_label = AutoString8(o.name) + AutoString8("(");
                    for (U64 ai = 0; ai < o.args.length; ai++) {
                        if (ai > 0) {
                            default_label = default_label + AutoString8(", ");
                        }
                        if (type_matches_tag<U64>(o.args[ai].value)) {
                            default_label = default_label + AutoString8(tbl.cols[get<U64>(o.args[ai].value)].name);
                        } else {
                            default_label = default_label + AutoString8("?");
                        }
                    }
                    default_label = default_label + AutoString8(")");
                } else if constexpr (SameAs<OT, planner::SelectOp::CountStar>) {
                    out_type      = type::create_basic(type::Basic::bigint);
                    default_label = AutoString8("count");
                }
            });
            if (op.conversions.length > 0) {
                out_type = type::create_basic(op.conversions[op.conversions.length - 1].to);
            }
            AutoString8 label = has_alias ? AutoString8(*stmt.select.clauses[i].as) : move(default_label);
            add_owned_column(move(label), out_type);
        }
        return vr;
    }

    static bool projection_needs_cell_meta(const planner::ProjectionPlan& p) {
        for (const auto& op : p.ops) {
            if (type_matches_tag<planner::SelectOp::TtlOf>(op.value) || type_matches_tag<planner::SelectOp::WritetimeOf>(op.value)) {
                return true;
            }
            // @note Token and FuncCall both need full row materialization
            // (Token reads PK columns; FuncCall may take column refs as args).
            // Route through the same projection pipeline.
            if (type_matches_tag<planner::SelectOp::Token>(op.value)) {
                return true;
            }
            if (type_matches_tag<planner::SelectOp::FuncCall>(op.value)) {
                return true;
            }
        }
        return false;
    }

    // Iteration + projection for SELECTs that need per-cell metadata (TTL/WRITETIME).
    // Produces a fully materialized VirtualRows result; uses the standard locator so it
    // composes with WHERE filters, ORDER BY reverse_clustering, and LIMIT.
    static coroutine::Task<ExecutionResult> execute_select_with_meta(
        Engine& engine, schema::Table* tbl, String8 ks_name, String8 table_name,
        const Select& stmt, const planner::SelectPlan& sp, EvalContext ctx, U64 limit_count
    ) {
        VirtualRows vr  = make_virtual_rows_shell_from_ops(*tbl, ks_name, table_name, stmt, sp.projection);
        S64         now = S64(os::unix_ms_now());

        auto iterate_range = [&](RowIterator& start, RowIterator& stop) -> coroutine::Task<void> {
            while (start != stop && vr.rows.length < limit_count) {
                ColumnRange col_range = co_await start.deref();
                if (io::row_is_expired(col_range.start.metadata, now)) {
                    co_await start.advance(stop);
                    continue;
                }
                DynamicArray<ColumnValue>                row_values;
                DynamicArray<bool>                       row_present;
                DynamicArray<Optional<io::CellMetadata>> cell_meta;
                co_await drain_columns(tbl, col_range.start, row_values, row_present, &cell_meta);
                bool pass = true;
                if (sp.filter.predicates.length > 0) {
                    EvalContext rc = ctx;
                    rc.table       = tbl;
                    rc.row_values  = row_values.ptr;
                    pass           = evaluate_where(sp.filter.predicates, rc);
                }
                if (pass) {
                    push_back(vr.rows, project_row_via_ops(*tbl, sp.projection.ops, row_values, row_present, cell_meta, now, ctx));
                }
                co_await start.advance(stop);
            }
        };

        if (sp.locator.pk.in_values.length > 0) {
            planner::RowLocator pk_eq_locator = sp.locator;
            pk_eq_locator.pk.is_equality      = true;
            pk_eq_locator.pk.has_in           = false;
            for (const auto& pk_bytes : sp.locator.pk.in_values) {
                if (vr.rows.length >= limit_count) {
                    break;
                }
                pk_eq_locator.pk.begin = pk_bytes;
                auto row_range         = co_await create_table_range_it(engine, tbl, pk_eq_locator);
                co_await iterate_range(row_range.start, row_range.stop);
            }
        } else if (!sp.locator.pk.has_in) {
            auto row_range = co_await create_table_range_it(engine, tbl, sp.locator);
            co_await iterate_range(row_range.start, row_range.stop);
        }
        // else: empty PK IN → no rows

        co_return ExecutionResult{
            .status       = ExecutionStatus::Success,
            .kind         = ResultKind::VirtualRows,
            .keyspace     = AutoString8(ks_name),
            .table        = AutoString8(table_name),
            .virtual_rows = move(vr),
        };
    }

    // @note LIMIT and ORDER BY have no effect on aggregate SELECT (Cassandra
    // semantics): the result is always a single row, and ordering does not
    // change the count.
    static coroutine::Task<ExecutionResult> execute_select_aggregate(
        Engine& engine, schema::Table* tbl, String8 ks_name, String8 table_name,
        const Select& stmt, const planner::SelectPlan& sp, EvalContext ctx
    ) {
        VirtualRows vr;
        vr.keyspace      = AutoString8(ks_name);
        vr.table         = AutoString8(table_name);
        String8 col_name = (stmt.select.clauses.length > 0 && stmt.select.clauses[0].as)
                             ? String8(*stmt.select.clauses[0].as)
                             : String8("count");
        emplace_back(vr.columns, VirtualColumn{col_name, type::create_basic(type::Basic::bigint)});

        bool has_filter = sp.filter.predicates.length > 0;
        S64  count      = 0;

        auto eval_filter = [&](const DynamicArray<ColumnValue>& row_values) -> bool {
            EvalContext row_ctx = ctx;
            row_ctx.table       = tbl;
            row_ctx.row_values  = row_values.ptr;
            return evaluate_where(sp.filter.predicates, row_ctx);
        };

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
            const DynamicArray<U8>& prefix      = sp.locator.index_key_prefix;
            auto                    prefix_view = TArrayView<const U8, U16>(prefix.ptr, static_cast<U16>(prefix.length));
            auto                    idx_it      = co_await btree::find_it<schema::IndexEntry, btree::SearchStrategy::FirstGreaterEqual>(
                active_idx->btree, prefix_view
            );
            auto idx_end     = btree::end<schema::IndexEntry>(active_idx->btree);
            S64  now_unix_ms = S64(os::unix_ms_now());

            while (idx_it != idx_end) {
                auto key_view = idx_it.key();
                if (key_view.length < prefix_view.length || os::memory_compare(key_view.ptr, prefix_view.ptr, prefix_view.length) != 0) {
                    break;
                }

                schema::IndexEntry entry = *idx_it;
                auto               hit   = co_await read_index_hit_row(engine, tbl, *active_idx, key_view, entry, now_unix_ms);
                if (hit && (!has_filter || eval_filter(hit->cv))) {
                    count++;
                }
                co_await idx_it.advance();
            }
        } else {
            auto row_range   = co_await create_table_range_it(engine, tbl, sp.locator);
            S64  now_unix_ms = S64(os::unix_ms_now());
            while (row_range.start != row_range.stop) {
                ColumnRange col_range = co_await row_range.start.deref();
                if (io::row_is_expired(col_range.start.metadata, now_unix_ms)) {
                    co_await row_range.start.advance(row_range.stop);
                    continue;
                }
                if (!has_filter) {
                    count++;
                } else {
                    DynamicArray<ColumnValue> row_values;
                    while (col_range.start != col_range.stop && row_values.length < tbl->cols.length) {
                        push_back(row_values, co_await col_range.start.deref());
                        co_await col_range.start.advance();
                    }
                    while (row_values.length < tbl->cols.length) {
                        push_back(row_values, ColumnValue{Null{}});
                    }
                    if (eval_filter(row_values)) {
                        count++;
                    }
                }
                co_await row_range.start.advance(row_range.stop);
            }
        }

        VirtualRow row;
        emplace_back(row.values, ColumnValue{S64(count)});
        push_back(vr.rows, move(row));

        co_return ExecutionResult{
            .status       = ExecutionStatus::Success,
            .kind         = ResultKind::VirtualRows,
            .keyspace     = AutoString8(ks_name),
            .table        = AutoString8(table_name),
            .virtual_rows = move(vr),
        };
    }

    // @note locator.pk.is_equality must be true. `new_cell_meta` is the per-cell metadata
    // template stamped on every cell touched by this mutation (TTL+writetime, or empty for
    // mutations without using-parameters); cells the spec doesn't touch keep their existing meta.
    static coroutine::Task<void> read_old_row_for_reindex(
        Engine& engine, schema::Table* tbl, U64 row_page, U64 static_page, bool have_indexes,
        DynamicArray<ColumnValue>& old_cv, DynamicArray<bool>& old_present
    ) {
        if (have_indexes) {
            co_await read_row_into(engine, tbl, row_page, static_page, old_cv, old_present);
        }
    }

    static coroutine::Task<void> clear_indexes_for_deleted_row(
        schema::Table* tbl, S64 pk_bytes, TArrayView<const U8, U16> ck_bytes, bool have_indexes,
        const DynamicArray<ColumnValue>& old_cv, const DynamicArray<bool>& old_present
    ) {
        if (have_indexes) {
            DynamicArray<ColumnValue> empty_cv;
            DynamicArray<bool>        empty_present;
            co_await update_indexes(tbl, pk_bytes, ck_bytes, schema::IndexEntry{0, 0}, old_cv, old_present, empty_cv, empty_present);
        }
    }

    static coroutine::Task<void> apply_mutation(
        Engine& engine, schema::Table* tbl,
        const planner::RowLocator&   locator,
        const planner::MutationSpec& spec,
        const io::CellMetadata&      new_cell_meta,
        const EvalContext&           ctx
    ) {
        S64  pk_bytes     = locator.pk.begin;
        bool have_indexes = tbl->indexes.length > 0;

        if (schema::has_clustering_keys(*tbl)) {
            auto                    entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);
            const DynamicArray<U8>& ck_bytes  = locator.ck.begin;

            if (spec.is_full_delete) {
                if (!entry_opt) {
                    co_return;
                }
                auto                    entry = *entry_opt;
                schema::ClusteringBTree ck_btree{
                    engine.pager, entry.data_page,
                    schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                };

                if (locator.ck.is_equality) {
                    auto row_page_opt = co_await btree::tfind<U64>(ck_btree, ck_bytes);
                    if (row_page_opt) {
                        DynamicArray<ColumnValue> old_cv;
                        DynamicArray<bool>        old_present;
                        co_await read_old_row_for_reindex(engine, tbl, *row_page_opt, entry.static_page, have_indexes, old_cv, old_present);
                        blob::BlobDynamicPaged row_blob;
                        co_await blob::load(row_blob, engine.pager, *row_page_opt);
                        co_await blob::remove(row_blob);
                        co_await btree::remove(ck_btree, ck_bytes);
                        auto ck_v = TArrayView<const U8, U16>(ck_bytes.ptr, static_cast<U16>(ck_bytes.length));
                        co_await clear_indexes_for_deleted_row(tbl, pk_bytes, ck_v, have_indexes, old_cv, old_present);
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
                        if (locator.ck.begin_is_partial && !locator.ck.begin_inclusive) {
                            while (it != end_it && key_has_prefix(it.key(), locator.ck.begin)) {
                                co_await it.advance();
                            }
                        }
                        while (it != end_it) {
                            auto key_view = it.key();
                            if (locator.ck.has_end) {
                                auto     end_view = TArrayView<const U8, U16>(locator.ck.end.ptr, static_cast<U16>(locator.ck.end.length));
                                Ordering ord      = key::compare_clustering(*tbl, key_view, end_view);
                                if (ord == Ordering::Greater || (ord == Ordering::Equal && !locator.ck.end_inclusive)) {
                                    break;
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
                    if (locator.ck.has_begin) {
                        auto ck_begin_view = TArrayView<const U8, U16>(locator.ck.begin.ptr, static_cast<U16>(locator.ck.begin.length));
                        if (locator.ck.begin_inclusive) {
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
                        DynamicArray<ColumnValue> old_cv;
                        DynamicArray<bool>        old_present;
                        co_await read_old_row_for_reindex(engine, tbl, e.page, entry.static_page, have_indexes, old_cv, old_present);
                        blob::BlobDynamicPaged row_blob;
                        co_await blob::load(row_blob, engine.pager, e.page);
                        co_await blob::remove(row_blob);
                        auto key_view = TArrayView<const U8, U16>(e.key.ptr, static_cast<U16>(e.key.length));
                        co_await btree::remove(ck_btree, key_view);
                        co_await clear_indexes_for_deleted_row(tbl, pk_bytes, key_view, have_indexes, old_cv, old_present);
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
            } else if (!locator.ck.is_equality) {
                // Static-only UPDATE: all SET columns are static; no CK provided or required.
                bool                   new_partition = !entry_opt;
                schema::PartitionEntry entry;
                if (entry_opt) {
                    entry = *entry_opt;
                } else {
                    entry.data_page = co_await btree::create_paged(
                        *engine.pager,
                        schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                    );
                    entry.static_page = 0;
                }

                RmwRowState st         = co_await rmw_row(engine, tbl, 0, entry.static_page, NO_CK_BYTES, new_partition ? &locator : nullptr, spec, new_cell_meta, have_indexes, ctx);
                bool        any_static = co_await rewrite_static(engine, entry, tbl, st.col_values, st.col_present, st.cell_meta);
                if (have_indexes) {
                    co_await update_indexes(tbl, pk_bytes, NO_CK_BYTES, schema::IndexEntry{entry.static_page, 0}, st.old_cv, st.old_present, st.col_values, st.col_present);
                }

                if (new_partition && any_static) {
                    co_await btree::tinsert(tbl->btree, pk_bytes, entry);
                } else if (new_partition && !any_static) {
                    // @note nothing to create — rewrite_static still allocated a static page
                    // for the (unused) pk preamble; free it rather than leaking it.
                    blob::BlobDynamicPaged orphan;
                    co_await blob::load(orphan, engine.pager, entry.static_page);
                    co_await blob::remove(orphan);
                } else if (!new_partition && !any_static) {
                    // entry.data_page/static_page are unchanged (rewrite_static reuses the
                    // static page in place) — only remove the partition if it's now fully
                    // empty of both static values and clustering rows.
                    schema::ClusteringBTree ck_check{
                        engine.pager, entry.data_page,
                        schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                    };
                    auto ck_begin_it = co_await btree::begin<U64>(ck_check);
                    if (ck_begin_it == btree::end<U64>(ck_check)) {
                        co_await btree::remove(tbl->btree, pk_bytes);
                    }
                }
                // !new_partition && any_static: entry unchanged in place, nothing to persist.
            } else {
                // Read-modify-write: shared path for UPDATE and column-level DELETE.
                bool                   new_partition = !entry_opt;
                schema::PartitionEntry entry;
                if (entry_opt) {
                    entry = *entry_opt;
                } else {
                    entry.data_page = co_await btree::create_paged(
                        *engine.pager,
                        schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                    );
                    entry.static_page = 0;
                }

                schema::ClusteringBTree ck_btree{
                    engine.pager, entry.data_page,
                    schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                };
                auto row_page_opt  = co_await btree::tfind<U64>(ck_btree, ck_bytes);
                U64  existing_page = row_page_opt ? *row_page_opt : 0;

                auto ck_v = TArrayView<const U8, U16>(ck_bytes.ptr, static_cast<U16>(ck_bytes.length));
                // new_partition: the static page doesn't have the pk preamble yet (rewrite_static
                // for it runs below, after this read), so backfill pk values into the row now —
                // update_indexes needs them present to build the index key's pk component.
                RmwRowState st = co_await rmw_row(engine, tbl, existing_page, entry.static_page, ck_v, new_partition ? &locator : nullptr, spec, new_cell_meta, have_indexes, ctx);

                bool any_static_updated = false;
                for (const auto& upd : spec.updates) {
                    if (upd.col_idx < tbl->cols.length && tbl->cols[upd.col_idx].is_static) {
                        any_static_updated = true;
                        break;
                    }
                }

                io::RowMetadata row_meta     = recompute_row_metadata(tbl, st.col_present, st.cell_meta);
                U64             new_row_page = co_await write_row_blob(engine, tbl, st.col_values, st.col_present, st.cell_meta, row_meta, existing_page);
                if (!row_page_opt) {
                    co_await btree::tinsert(ck_btree, ck_bytes, new_row_page);
                }
                // row_page_opt case: write_row_blob reused existing_page in place, so
                // new_row_page == existing_page and ck_btree already maps to it — no change.

                // @note a brand-new partition always needs its static page written (at least
                // the pk preamble), regardless of whether this particular mutation touches a
                // static column.
                if (new_partition || any_static_updated) {
                    co_await rewrite_static(engine, entry, tbl, st.col_values, st.col_present, st.cell_meta);
                }
                if (new_partition) {
                    co_await btree::tinsert(tbl->btree, pk_bytes, entry);
                }
                // !new_partition: entry.data_page/static_page unchanged in place, nothing to persist.

                if (have_indexes) {
                    co_await update_indexes(tbl, pk_bytes, ck_v, schema::IndexEntry{entry.static_page, new_row_page}, st.old_cv, st.old_present, st.col_values, st.col_present);
                }
            }
        } else {
            // Non-clustering table: data_page is the row blob directly.
            auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);

            if (spec.is_full_delete) {
                if (entry_opt) {
                    DynamicArray<ColumnValue> old_cv;
                    DynamicArray<bool>        old_present;
                    co_await read_old_row_for_reindex(engine, tbl, entry_opt->data_page, entry_opt->static_page, have_indexes, old_cv, old_present);
                    // @note no clustering key means no separate static page — the pk preamble
                    // lives in data_page's own blob (see write_row_blob), removed below with it.
                    blob::BlobDynamicPaged row_blob;
                    co_await blob::load(row_blob, engine.pager, entry_opt->data_page);
                    co_await blob::remove(row_blob);
                    co_await btree::remove(tbl->btree, pk_bytes);
                    co_await clear_indexes_for_deleted_row(tbl, pk_bytes, NO_CK_BYTES, have_indexes, old_cv, old_present);
                }
            } else {
                bool                   new_partition = !entry_opt;
                schema::PartitionEntry entry         = entry_opt ? *entry_opt : schema::PartitionEntry{0, 0};

                // @note this table has no clustering key, so a new partition's pk preamble is
                // written by write_row_blob directly into the row blob below, not a static page.
                RmwRowState st = co_await rmw_row(engine, tbl, entry.data_page, entry.static_page, NO_CK_BYTES, new_partition ? &locator : nullptr, spec, new_cell_meta, have_indexes, ctx);

                io::RowMetadata row_meta     = recompute_row_metadata(tbl, st.col_present, st.cell_meta);
                U64             new_row_page = co_await write_row_blob(engine, tbl, st.col_values, st.col_present, st.cell_meta, row_meta, entry.data_page);
                if (new_partition) {
                    co_await btree::tinsert(tbl->btree, pk_bytes, schema::PartitionEntry{new_row_page, entry.static_page});
                }
                // !new_partition: write_row_blob reused entry.data_page in place, so
                // new_row_page == entry.data_page — nothing to persist.

                if (have_indexes) {
                    co_await update_indexes(tbl, pk_bytes, NO_CK_BYTES, schema::IndexEntry{entry.static_page, new_row_page}, st.old_cv, st.old_present, st.col_values, st.col_present);
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

                // @note Cassandra/Scylla allow keyspace names up to 192 chars.
                if (stmt.name.length > 192) {
                    co_return ExecutionResult{
                        .status  = ExecutionStatus::Invalid,
                        .message = "Keyspace name exceeds 192 characters",
                    };
                }

                if (auto existing = schema::read_keyspace(engine.schema, stmt.name).value; existing != nullptr) {
                    co_return (stmt.if_not_exists) ? create_void_success() : create_keyspace_already_exists(stmt.name);
                }

                auto ks_res = co_await schema::create_keyspace(engine.schema, stmt);
                if (ks_res.value == nullptr) {
                    if (ks_res.error == schema::Error::SyntaxOptions) {
                        ExecutionResult r;
                        r.status          = ExecutionStatus::SyntaxError;
                        r.message_storage = AutoString8(ks_res.message);
                        r.message         = String8(r.message_storage);
                        co_return r;
                    }
                    if (ks_res.error == schema::Error::InvalidOptions) {
                        ExecutionResult r;
                        r.status          = ExecutionStatus::ConfigError;
                        r.message_storage = AutoString8(ks_res.message);
                        r.message         = String8(r.message_storage);
                        co_return r;
                    }
                    co_return create_server_error("Failed to create keyspace");
                }

                co_return create_keyspace_created(stmt.name);
            } else if constexpr (SameAs<T, CreateTable>) {
                String8 ks_name = resolve_ks_name(stmt.name.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks = ksr.ks;

                if (auto existing = schema::read_table(*ks, stmt.name.table_name); existing.value != nullptr) {
                    co_return (stmt.if_not_exists) ? create_void_success() : create_table_already_exists(ks_name, stmt.name.table_name);
                }

                for (U64 i = 0; i < stmt.column_definitions.length; i++) {
                    for (U64 j = i + 1; j < stmt.column_definitions.length; j++) {
                        if (stmt.column_definitions[i].name.identifier == stmt.column_definitions[j].name.identifier) {
                            ExecutionResult r;
                            r.status          = ExecutionStatus::Invalid;
                            r.message_storage = AutoString8("Multiple definition of identifier ") + AutoString8(stmt.column_definitions[i].name.identifier);
                            r.message         = String8(r.message_storage);
                            co_return r;
                        }
                    }
                }

                auto tbl_res = co_await schema::create_table(engine.schema, *ks, stmt);
                if (tbl_res.value == nullptr) {
                    if (tbl_res.error == schema::Error::InvalidOptions) {
                        ExecutionResult r;
                        r.status          = ExecutionStatus::Invalid;
                        r.message_storage = AutoString8(tbl_res.message);
                        r.message         = String8(r.message_storage);
                        co_return r;
                    }
                    if (tbl_res.error == schema::Error::MissingPrimaryKey) {
                        co_return ExecutionResult{
                            .status  = ExecutionStatus::Invalid,
                            .message = "No PRIMARY KEY specified (exactly one required)",
                        };
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
                if (is_system_keyspace(stmt.keyspace)) {
                    co_return create_system_keyspace_invalid();
                }

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(stmt.keyspace);
                }

                if (auto vr = schema::parse_keyspace_options(stmt.options); vr.error != schema::Error::None) {
                    ExecutionResult r;
                    r.status          = (vr.error == schema::Error::SyntaxOptions) ? ExecutionStatus::SyntaxError : ExecutionStatus::ConfigError;
                    r.message_storage = AutoString8(vr.message);
                    r.message         = String8(r.message_storage);
                    co_return r;
                }

                // ALTER KEYSPACE must specify at least one replication option when changing
                // strategy. CREATE allows defaults, but ALTER requires explicit options for
                // SimpleStrategy (replication_factor) and NetworkTopologyStrategy (any DC).
                for (const auto& opt : stmt.options.identifier_values) {
                    if (opt.first != "replication") {
                        continue;
                    }
                    if (!type_matches_tag<MapLiteral>(opt.second)) {
                        continue;
                    }
                    const auto& m               = get<MapLiteral>(opt.second);
                    U64         non_class_count = 0;
                    for (const auto& [kt, vt] : m.key_values) {
                        if (!type_matches_tag<Literal>(kt.value)) {
                            continue;
                        }
                        const auto& kc = get<Literal>(kt.value);
                        if (!type_matches_tag<AutoString8>(kc.value)) {
                            continue;
                        }
                        if (get<AutoString8>(kc.value) != "class") {
                            non_class_count++;
                        }
                    }
                    if (non_class_count == 0) {
                        co_return ExecutionResult{
                            .status  = ExecutionStatus::ConfigError,
                            .message = "ALTER KEYSPACE requires at least one replication option",
                        };
                    }
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
                if (is_system_keyspace(stmt.keyspace)) {
                    co_return create_system_keyspace_invalid();
                }

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace).value;
                if (ks == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_keyspace_not_found(stmt.keyspace);
                }

                co_await schema::delete_keyspace(engine.schema, stmt.keyspace);

                co_return create_schema_changed(stmt.keyspace);
            } else if constexpr (SameAs<T, DropTable>) {
                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return (stmt.if_exists && ksr.not_found) ? create_void_success() : move(*ksr.error);
                }
                auto ks = ksr.ks;

                if ((co_await schema::delete_table(engine.schema, *ks, stmt.table.table_name)).error != schema::Error::None) {
                    if (stmt.if_exists) {
                        co_return create_void_success();
                    }
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_return create_schema_changed(ks_name, stmt.table.table_name);
            } else if constexpr (SameAs<T, TruncateTable>) {
                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks = ksr.ks;

                auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_await btree::truncate(tbl->btree);

                co_return create_void_success();
            } else if constexpr (SameAs<T, Select>) {
                ZoneScopedN("engine::select");
                String8 ks_name = resolve_ks_name(stmt.from.keyspace_name, current_keyspace);
                if (ks_name.length == 0) {
                    co_return create_no_keyspace_specified();
                }

                auto system_vr = try_system_select(engine, ks_name, stmt.from.table_name);
                if (system_vr) {
                    if (stmt.where) {
                        apply_virtual_where(*system_vr, *stmt.where, ctx);
                    }
                    // Apply column projection for `SELECT col1, col2 FROM system.X` shapes.
                    // SELECT * leaves clauses empty and keeps the full row.
                    if (stmt.select.clauses.length > 0) {
                        DynamicArray<U64> keep_idx;
                        bool              all_simple = true;
                        for (const auto& sc : stmt.select.clauses) {
                            if (!type_matches_tag<ColumnName>(sc.column.value)) {
                                all_simple = false;
                                break;
                            }
                            const auto& cn  = get<ColumnName>(sc.column.value);
                            U64         hit = system_vr->columns.length;
                            for (U64 ci = 0; ci < system_vr->columns.length; ci++) {
                                if (system_vr->columns[ci].name == String8(cn.identifier)) {
                                    hit = ci;
                                    break;
                                }
                            }
                            if (hit == system_vr->columns.length) {
                                all_simple = false;
                                break;
                            }
                            push_back(keep_idx, hit);
                        }
                        if (all_simple) {
                            VirtualRows projected;
                            projected.keyspace = move(system_vr->keyspace);
                            projected.table    = move(system_vr->table);
                            for (U64 i = 0; i < keep_idx.length; i++) {
                                push_back(projected.columns, system_vr->columns[keep_idx[i]]);
                            }
                            for (const auto& row : system_vr->rows) {
                                VirtualRow new_row;
                                for (U64 i = 0; i < keep_idx.length; i++) {
                                    push_back(new_row.values, row.values[keep_idx[i]]);
                                }
                                push_back(projected.rows, move(new_row));
                            }
                            system_vr = move(projected);
                        }
                    }
                    co_return ExecutionResult{
                        .status       = ExecutionStatus::Success,
                        .kind         = ResultKind::VirtualRows,
                        .keyspace     = AutoString8(ks_name),
                        .table        = AutoString8(stmt.from.table_name),
                        .virtual_rows = move(system_vr),
                    };
                }
                if (is_system_keyspace(ks_name)) {
                    co_return create_system_keyspace_invalid();
                }

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    co_return create_keyspace_not_found(ks_name);
                }

                auto tbl = schema::read_table(*ks, stmt.from.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.from.table_name);
                }

                if (stmt.transform && *stmt.transform == Select::Transform::JSON) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "SELECT JSON is not supported"};
                }
                if (stmt.group_by) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "GROUP BY is not supported"};
                }
                if (stmt.per_partition_limit.value) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "PER PARTITION LIMIT is not supported"};
                }

                U64 limit_count = MAX_U64;
                if (type_matches_tag<S64>(stmt.limit.value)) {
                    limit_count = U64(get<S64>(stmt.limit.value));
                }

                planner::SelectPlan sp = planner::plan_select(stmt, *tbl, ctx);
                if (auto err = create_error_if_plan_invalid(sp.result)) {
                    co_return move(*err);
                }

                if (sp.projection.is_aggregate) {
                    co_return co_await execute_select_aggregate(
                        engine, tbl, ks_name, stmt.from.table_name, stmt, sp, ctx
                    );
                }

                if (projection_needs_cell_meta(sp.projection)) {
                    co_return co_await execute_select_with_meta(
                        engine, tbl, ks_name, stmt.from.table_name, stmt, sp, ctx, limit_count
                    );
                }

                // Extract column indices for native layer from projection ops.
                DynamicArray<U64>                   select_col_indices;
                DynamicArray<Optional<AutoString8>> select_col_aliases;
                {
                    // @note projection-op index → SELECT-clause index. In this raw-Rows path
                    // every clause is a plain ColumnName (Function / Cast clauses route to the
                    // VirtualRows path), so each ColumnRef op corresponds 1:1 to a clause.
                    U64 clause_idx = 0;
                    for (const auto& op : sp.projection.ops) {
                        if (type_matches_tag<planner::SelectOp::ColumnRef>(op.value)) {
                            push_back(select_col_indices, get<planner::SelectOp::ColumnRef>(op.value).col_idx);
                            if (clause_idx < stmt.select.clauses.length && stmt.select.clauses[clause_idx].as.has_value()) {
                                push_back(select_col_aliases, Optional<AutoString8>{*stmt.select.clauses[clause_idx].as});
                            } else {
                                push_back(select_col_aliases, Optional<AutoString8>{});
                            }
                        }
                        clause_idx++;
                    }
                }

                if (sp.locator.index_col_idx) {
                    co_return co_await execute_select_index(
                        engine, tbl, ks_name, stmt.from.table_name, sp, move(select_col_indices), move(select_col_aliases), limit_count, ctx
                    );
                }

                if (stmt.order_by && sp.locator.pk.in_values.length > 0) {
                    co_return co_await execute_select_pk_in_ordered(
                        engine, tbl, ks_name, stmt.from.table_name, sp, move(select_col_indices), move(select_col_aliases), limit_count, ctx
                    );
                }

                // @note PK IN must iterate listed partitions individually in input order
                // (Cassandra semantics). A BTree range walk would return them in token order.
                // has_in with empty in_values means an empty IN list (zero rows).
                if (sp.locator.pk.in_values.length > 0 || sp.locator.pk.has_in) {
                    co_return co_await execute_select_pk_in(
                        engine, tbl, ks_name, stmt.from.table_name, stmt, sp, move(select_col_indices), move(select_col_aliases), limit_count, ctx
                    );
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
                    .select_col_aliases = move(select_col_aliases),
                    .filter_predicates  = move(sp.filter.predicates),
                    .filter_ctx         = ctx,
                };
            } else if constexpr (SameAs<T, Insert>) {
                ZoneScopedN("engine::insert");
                warn_using_timestamp_single_node(engine, stmt.using_parameters, "INSERT USING TIMESTAMP not supported in non-single-node mode", "ignoring INSERT USING TIMESTAMP (single-node no-op)");
                if (auto bad_ttl = validate_using_ttl(stmt.using_parameters)) {
                    co_return move(*bad_ttl);
                }
                if (auto bad_ts = validate_using_timestamp(stmt.using_parameters)) {
                    co_return move(*bad_ts);
                }
                assert_true(static_cast<bool>(stmt.insert_clause), "missing insert clause, this should never happen");

                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks = ksr.ks;

                auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                if (planner::table_has_counter(*tbl)) {
                    co_return ExecutionResult{
                        .status  = ExecutionStatus::Invalid,
                        .message = "INSERT statements are not allowed on counter tables, use UPDATE instead",
                    };
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
                                if (eval_is_unset(eval)) {
                                    if (col.key_kind != schema::KeyKind::None) {
                                        co_return create_unset_on_key_column(ks->name, tbl->name, col.name);
                                    }
                                    continue;
                                }
                                if (!io::can_write_evaluated_as_column_value(eval, col.type, ctx)) {
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

                        io::RowMetadata  row_meta         = compute_insert_metadata(stmt.using_parameters, tbl->options.default_ttl_ms);
                        io::CellMetadata insert_cell_meta = resolve_mutation_cell_meta(stmt.using_parameters, tbl->options.default_ttl_ms);

                        // collect regular (non-key, non-static) row bytes into buffer (sync)
                        DynamicArray<U8> row_buffer;
                        auto             write_fn      = io::sync_buffer_writer(row_buffer);
                        auto             write         = io::to_writer(write_fn);
                        auto             row_is_active = [&](U64 col_idx) -> bool {
                            if (tbl->cols[col_idx].tombstone || tbl->cols[col_idx].is_static || tbl->cols[col_idx].key_kind != schema::KeyKind::None) {
                                return false;
                            }
                            auto ni = try_get_names_idx(tbl->cols[col_idx].name);
                            if (!ni) {
                                return false;
                            }
                            // Treat null/unset as column-absent so the cell mask reflects reality.
                            const Evaluated eval = evaluate(v.values[*ni], ctx);
                            if (eval_is_null(eval)) {
                                return false;
                            }
                            if (type_matches_tag<Literal>(eval.value) && type_matches_tag<Unset>(get<Literal>(eval.value).value)) {
                                return false;
                            }
                            return true;
                        };
                        // @note tables without a clustering key have exactly one row blob per
                        // partition and can never have static columns, so the pk preamble is
                        // prepended here instead of maintaining a separate static page for it
                        // (matches write_row_blob's convention for the UPDATE path).
                        if (!schema::has_clustering_keys(*tbl)) {
                            DynamicArray<ColumnValue> pk_col_values;
                            DynamicArray<bool>        pk_col_present;
                            resize(pk_col_values, tbl->cols.length);
                            resize(pk_col_present, tbl->cols.length);
                            for (U64 pk_ci : tbl->partition_key_col_indices) {
                                Evaluated pk_eval     = evaluate(v.values[*try_get_names_idx(tbl->cols[pk_ci].name)], ctx);
                                pk_col_values[pk_ci]  = co_await materialize_as_column_value(pk_eval, tbl->cols[pk_ci].type, ctx);
                                pk_col_present[pk_ci] = true;
                            }
                            write_pk_preamble(write, tbl, pk_col_values, pk_col_present);
                        }
                        io::write_row_metadata(write, row_meta);
                        io::write_column_mask(write, io::to_checker(row_is_active), tbl->cols.length);
                        // INSERT stamps the same cell metadata on every written column.
                        io::write_mask_bits(write, io::to_checker(row_is_active), tbl->cols.length);
                        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                            if (row_is_active(ci)) {
                                io::write_cell_metadata(write, insert_cell_meta);
                                const auto& eval = evaluate(v.values[*try_get_names_idx(tbl->cols[ci].name)], ctx);
                                io::write_evaluated_as_column_value(write, eval, tbl->cols[ci].type, ctx);
                            }
                        }

                        bool any_static_in_insert = false;
                        for (U64 si : tbl->static_col_indices) {
                            if (!tbl->cols[si].tombstone && try_get_names_idx(tbl->cols[si].name)) {
                                any_static_in_insert = true;
                                break;
                            }
                        }

                        // build partition key and insert into btree(s)
                        DynamicArray<Evaluated> partition_evals;
                        for (U64 pk_ci : tbl->partition_key_col_indices) {
                            push_back(partition_evals, evaluate(v.values[*try_get_names_idx(tbl->cols[pk_ci].name)], ctx));
                        }
                        S64 pk_bytes = key::compute_partition_token_from_evals(*tbl, partition_evals);

                        bool have_indexes = tbl->indexes.length > 0;

                        // @note Build a new_cv/new_present pair from the named insert values so
                        // the shared collection-aware update_indexes runs uniformly across both
                        // scalar and collection indexed columns.
                        auto build_new_cv = [&]() -> coroutine::Task<Pair<DynamicArray<ColumnValue>, DynamicArray<bool>>> {
                            DynamicArray<ColumnValue> new_cv;
                            DynamicArray<bool>        new_present;
                            resize(new_cv, tbl->cols.length);
                            resize(new_present, tbl->cols.length);
                            for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                new_present[ci] = false;
                                auto ni         = try_get_names_idx(tbl->cols[ci].name);
                                if (!ni) {
                                    continue;
                                }
                                Evaluated e = evaluate(v.values[*ni], ctx);
                                if (eval_is_null(e)) {
                                    continue;
                                }
                                new_cv[ci]      = co_await materialize_as_column_value(e, tbl->cols[ci].type, ctx);
                                new_present[ci] = !type_matches_tag<Null>(new_cv[ci]);
                            }
                            co_return Pair<DynamicArray<ColumnValue>, DynamicArray<bool>>{move(new_cv), move(new_present)};
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
                                    schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                                );
                                entry.static_page = 0;
                                new_partition     = true;
                            }

                            // @note a brand-new partition always needs its static page written
                            // (at least the pk preamble), regardless of whether this INSERT sets
                            // any static column.
                            if (any_static_in_insert || new_partition) {
                                DynamicArray<ColumnValue>                sc_vals;
                                DynamicArray<bool>                       sc_present;
                                DynamicArray<Optional<io::CellMetadata>> sc_meta;
                                co_await read_row_into(engine, tbl, 0, entry.static_page, sc_vals, sc_present, nullptr, &sc_meta);
                                if (new_partition) {
                                    for (U64 i = 0; i < tbl->partition_key_col_indices.length; i++) {
                                        U64 pk_ci         = tbl->partition_key_col_indices[i];
                                        sc_vals[pk_ci]    = co_await materialize_as_column_value(partition_evals[i], tbl->cols[pk_ci].type, ctx);
                                        sc_present[pk_ci] = true;
                                    }
                                }
                                planner::MutationSpec static_spec;
                                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                    if (is_static_col(ci) && !tbl->cols[ci].tombstone) {
                                        auto ni = try_get_names_idx(tbl->cols[ci].name);
                                        if (ni) {
                                            push_back(static_spec.updates, planner::ColumnUpdate{ci, TaggedUnion<Evaluated, TermWithIdentifiers, planner::CollectionPatch>{evaluate(v.values[*ni])}});
                                        }
                                    }
                                }
                                co_await apply_updates_to_row(tbl, sc_vals, sc_present, sc_meta, insert_cell_meta, static_spec, ctx);
                                co_await rewrite_static(engine, entry, tbl, sc_vals, sc_present, sc_meta);
                            }

                            // persist the partition entry (new partitions only — an existing
                            // entry's data_page/static_page are unchanged in place)
                            if (new_partition) {
                                co_await btree::tinsert(tbl->btree, pk_bytes, entry);
                            }

                            if (needs_clustering_row) {
                                schema::ClusteringBTree clustering_btree{
                                    engine.pager, entry.data_page,
                                    schema::make_clustering_key_policy(*tbl), btree::FixedValuePolicy<sizeof(U64)>{}
                                };
                                DynamicArray<Evaluated> clustering_evals;
                                for (U64 ck_ci : tbl->clustering_key_col_indices) {
                                    push_back(clustering_evals, evaluate(v.values[*try_get_names_idx(tbl->cols[ck_ci].name)], ctx));
                                }
                                DynamicArray<U8> ck_bytes = key::encode_clustering(*tbl, clustering_evals);
                                auto             ck_v     = TArrayView<const U8, U16>(ck_bytes.ptr, static_cast<U16>(ck_bytes.length));

                                auto row_page_opt  = co_await btree::tfind<U64>(clustering_btree, ck_bytes);
                                U64  existing_page = row_page_opt ? *row_page_opt : 0;

                                planner::MutationSpec spec;
                                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                    if (tbl->cols[ci].tombstone || tbl->cols[ci].is_static || tbl->cols[ci].key_kind != schema::KeyKind::None) {
                                        continue;
                                    }
                                    auto ni = try_get_names_idx(tbl->cols[ci].name);
                                    if (!ni) {
                                        continue;
                                    }
                                    push_back(spec.updates, planner::ColumnUpdate{ci, TaggedUnion<Evaluated, TermWithIdentifiers, planner::CollectionPatch>{evaluate(v.values[*ni], ctx)}});
                                }
                                // Cassandra INSERT writes per cell: omitted or UNSET-bound columns
                                // keep their existing values, so this reads-modifies-writes like
                                // UPDATE rather than overwriting the row blob (read_row_into
                                // tolerates existing_page == 0 for a brand-new clustering row).
                                // No pk_inject_locator: unlike UPDATE, a brand-new partition's
                                // static page (and pk preamble) was already written above,
                                // before needs_clustering_row runs.
                                RmwRowState st = co_await rmw_row(engine, tbl, existing_page, entry.static_page, ck_v, nullptr, spec, insert_cell_meta, have_indexes, ctx);

                                // recompute_row_metadata derives the row's TTL from touched
                                // regular cells, but a key-only INSERT (no regular columns
                                // given, e.g. INSERT INTO t (k, c) VALUES (...) USING TTL n)
                                // has none to derive from — its liveness is carried by the
                                // statement's own row-marker TTL (row_meta) instead.
                                bool any_regular_cell = false;
                                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                    if (!tbl->cols[ci].is_static && tbl->cols[ci].key_kind == schema::KeyKind::None && st.col_present[ci]) {
                                        any_regular_cell = true;
                                        break;
                                    }
                                }
                                io::RowMetadata new_row_meta = any_regular_cell ? recompute_row_metadata(tbl, st.col_present, st.cell_meta) : row_meta;
                                U64             row_page     = co_await write_row_blob(engine, tbl, st.col_values, st.col_present, st.cell_meta, new_row_meta, existing_page);
                                if (!row_page_opt) {
                                    co_await btree::tinsert(clustering_btree, ck_bytes, row_page);
                                }

                                if (have_indexes) {
                                    co_await update_indexes(tbl, pk_bytes, ck_v, schema::IndexEntry{entry.static_page, row_page}, st.old_cv, st.old_present, st.col_values, st.col_present);
                                }
                            }
                        } else {
                            // @note Cassandra INSERT writes per cell: omitted or UNSET-bound
                            // columns keep their existing values, so existing partitions take
                            // a read-modify-write path rather than overwriting the row blob.
                            auto entry_opt = co_await btree::tfind<schema::PartitionEntry>(tbl->btree, pk_bytes);
                            if (!entry_opt) {
                                // @note row_page's blob already carries the pk preamble (written
                                // above alongside the row header) — no separate static page.
                                U64 row_page = co_await write_blob_in_place(engine, 0, row_buffer.ptr, row_buffer.length);
                                co_await btree::tinsert(tbl->btree, pk_bytes, schema::PartitionEntry{row_page, 0});

                                if (have_indexes) {
                                    DynamicArray<ColumnValue> old_cv;
                                    DynamicArray<bool>        old_present;
                                    auto                      built = co_await build_new_cv();
                                    co_await update_indexes(tbl, pk_bytes, NO_CK_BYTES, schema::IndexEntry{0, row_page}, old_cv, old_present, built.first, built.second);
                                }
                            } else {
                                planner::MutationSpec spec;
                                for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                    if (tbl->cols[ci].tombstone || tbl->cols[ci].is_static || tbl->cols[ci].key_kind != schema::KeyKind::None) {
                                        continue;
                                    }
                                    auto ni = try_get_names_idx(tbl->cols[ci].name);
                                    if (!ni) {
                                        continue;
                                    }
                                    push_back(spec.updates, planner::ColumnUpdate{ci, TaggedUnion<Evaluated, TermWithIdentifiers, planner::CollectionPatch>{evaluate(v.values[*ni], ctx)}});
                                }
                                RmwRowState st = co_await rmw_row(engine, tbl, entry_opt->data_page, entry_opt->static_page, NO_CK_BYTES, nullptr, spec, insert_cell_meta, have_indexes, ctx);

                                io::RowMetadata new_row_meta = recompute_row_metadata(tbl, st.col_present, st.cell_meta);
                                U64             new_row_page = co_await write_row_blob(engine, tbl, st.col_values, st.col_present, st.cell_meta, new_row_meta, entry_opt->data_page);
                                if (have_indexes) {
                                    co_await update_indexes(tbl, pk_bytes, NO_CK_BYTES, schema::IndexEntry{entry_opt->static_page, new_row_page}, st.old_cv, st.old_present, st.col_values, st.col_present);
                                }
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
                warn_using_timestamp_single_node(engine, stmt.using_parameters, "UPDATE USING TIMESTAMP not supported in non-single-node mode", "ignoring UPDATE USING TIMESTAMP (single-node no-op)");
                if (auto bad_ttl = validate_using_ttl(stmt.using_parameters)) {
                    co_return move(*bad_ttl);
                }
                if (auto bad_ts = validate_using_timestamp(stmt.using_parameters)) {
                    co_return move(*bad_ts);
                }
                assert_true_not_implemented(!stmt.if_, "UPDATE IF is not implemented");

                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks = ksr.ks;

                auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                planner::MutationPlan mp = planner::plan_update(stmt, *tbl, ctx);
                if (auto err = create_error_if_plan_invalid(mp.result)) {
                    co_return move(*err);
                }
                io::CellMetadata upd_cell_meta = resolve_mutation_cell_meta(stmt.using_parameters, tbl->options.default_ttl_ms);
                auto             apply_for_ck  = [&](planner::MutationPlan& mp_ref) -> coroutine::Task<void> {
                    if (mp_ref.locator.ck.in_values.length > 0) {
                        mp_ref.locator.ck.is_equality = true;
                        for (const auto& ck_bytes : mp_ref.locator.ck.in_values) {
                            mp_ref.locator.ck.begin = ck_bytes;
                            co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, upd_cell_meta, ctx);
                        }
                    } else if (!mp_ref.locator.ck.has_in) {
                        co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, upd_cell_meta, ctx);
                    }
                    // else: empty CK IN → no-op
                };
                if (mp.locator.pk.in_values.length > 0) {
                    mp.locator.pk.is_equality = true;
                    for (const auto& pk_bytes : mp.locator.pk.in_values) {
                        mp.locator.pk.begin = pk_bytes;
                        co_await apply_for_ck(mp);
                    }
                } else if (!mp.locator.pk.has_in) {
                    co_await apply_for_ck(mp);
                }
                // else: empty PK IN → no-op
                co_return create_void_success();
            } else if constexpr (SameAs<T, Delete>) {
                ZoneScopedN("engine::delete");
                warn_using_timestamp_single_node(engine, stmt.using_parameters, "DELETE USING TIMESTAMP not supported in non-single-node mode", "ignoring DELETE USING TIMESTAMP (single-node no-op)");
                for (const auto& param : stmt.using_parameters) {
                    if (param.kind == UpdateParameter::Kind::TTL) {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "DELETE USING TTL is not implemented"};
                    }
                }
                assert_true_not_implemented(!stmt.if_, "DELETE IF is not implemented");

                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks = ksr.ks;

                auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                planner::MutationPlan mp = planner::plan_delete(stmt, *tbl, ctx);
                if (auto err = create_error_if_plan_invalid(mp.result)) {
                    co_return move(*err);
                }
                io::CellMetadata del_cell_meta{}; // column-level DELETE clears metadata on the touched cells
                auto             apply_for_ck_del = [&](planner::MutationPlan& mp_ref) -> coroutine::Task<void> {
                    if (mp_ref.locator.ck.in_values.length > 0) {
                        mp_ref.locator.ck.is_equality = true;
                        for (const auto& ck_bytes : mp_ref.locator.ck.in_values) {
                            mp_ref.locator.ck.begin = ck_bytes;
                            co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, del_cell_meta, ctx);
                        }
                    } else if (!mp_ref.locator.ck.has_in) {
                        co_await apply_mutation(engine, tbl, mp_ref.locator, mp_ref.spec, del_cell_meta, ctx);
                    }
                    // else: empty CK IN → no-op
                };
                if (mp.locator.pk.in_values.length > 0) {
                    mp.locator.pk.is_equality = true;
                    for (const auto& pk_bytes : mp.locator.pk.in_values) {
                        mp.locator.pk.begin = pk_bytes;
                        co_await apply_for_ck_del(mp);
                    }
                } else if (!mp.locator.pk.has_in) {
                    co_await apply_for_ck_del(mp);
                }
                // else: empty PK IN → no-op
                co_return create_void_success();
            } else if constexpr (SameAs<T, AlterTable>) {
                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return (stmt.if_exists && ksr.not_found) ? create_void_success() : move(*ksr.error);
                }
                auto ks = ksr.ks;

                auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success() : create_table_not_found(ks_name, stmt.table.table_name);
                }

                co_return co_await visit(stmt.alter_table_instruction, [&engine, tbl, &ks_name, &stmt](const auto& instr) -> coroutine::Task<ExecutionResult> {
                    using I = RemoveCVRef<decltype(instr)>;
                    if constexpr (SameAs<I, AlterTable::AddColumnInstruction>) {
                        for (const auto& col_def : instr.column_definitions) {
                            auto resolved = schema::resolve_type_ast(engine.schema, ks_name, col_def.type);
                            if (resolved.error != schema::Error::None) {
                                ExecutionResult er;
                                er.status          = ExecutionStatus::Invalid;
                                er.keyspace        = AutoString8(ks_name);
                                er.message_storage = AutoString8(resolved.message);
                                er.message         = String8(er.message_storage);
                                co_return er;
                            }
                            auto existing = schema::read_column(*tbl, col_def.name.identifier);
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
                                    bool same_type   = col.type == resolved.value;
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
                            if (auto res = co_await schema::create_column(engine.schema, *tbl, String8{col_def.name.identifier.c_str, col_def.name.identifier.length}, move(resolved.value), col_def._static); res.error != schema::Error::None) {
                                co_return create_server_error("Failed to add column");
                            }
                        next_col:;
                        }
                        co_return create_schema_changed(ks_name, stmt.table.table_name);
                    } else if constexpr (SameAs<I, AlterTable::DropColumnInstruction>) {
                        for (const auto& col_name : instr.columns) {
                            auto col_res = schema::read_column(*tbl, col_name.identifier);
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
                        // Wrap loose Options in a TableOptions AST node so we can reuse schema::parse_table_options
                        CreateTable::TableOptions wrapped{};
                        for (const auto& opt : instr.identifier_values) {
                            push_back(wrapped.value, decltype(wrapped.value)::Element{opt});
                        }
                        auto popts = schema::parse_table_options(wrapped);
                        if (popts.error != schema::Error::None) {
                            ExecutionResult r;
                            r.status          = ExecutionStatus::SyntaxError;
                            r.message_storage = AutoString8(popts.message);
                            r.message         = String8(r.message_storage);
                            co_return r;
                        }
                        // Merge: start with current options, overwrite only those mentioned in WITH.
                        schema::TableOptions merged = tbl->options;
                        for (const auto& opt : instr.identifier_values) {
                            String8 key = opt.first;
                            if (key == "default_time_to_live") {
                                merged.default_ttl_ms = popts.value.options.default_ttl_ms;
                            } else if (key == "gc_grace_seconds") {
                                merged.gc_grace_seconds = popts.value.options.gc_grace_seconds;
                            } else if (key == "min_index_interval") {
                                merged.min_index_interval = popts.value.options.min_index_interval;
                            } else if (key == "max_index_interval") {
                                merged.max_index_interval = popts.value.options.max_index_interval;
                            } else if (key == "memtable_flush_period_in_ms") {
                                merged.memtable_flush_period_in_ms = popts.value.options.memtable_flush_period_in_ms;
                            }
                        }
                        co_await schema::set_table_options(engine.schema, *tbl, merged);
                        co_return create_schema_changed(ks_name, stmt.table.table_name);
                    } else {
                        static_assert(!SameAs<I, I>);
                        co_return ExecutionResult{};
                    }
                });
            } else if constexpr (SameAs<T, CreateIndex>) {
                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks  = ksr.ks;
                auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    co_return create_table_not_found(ks_name, stmt.table.table_name);
                }

                Optional<U64> col_idx = schema::find_column(*tbl, stmt.column_name);
                if (!col_idx) {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "column not found"};
                }
                const auto& col_type = tbl->cols[*col_idx].type.value;

                schema::IndexKind kind = schema::IndexKind::Values;
                if (stmt.selector) {
                    String8 sel(stmt.selector->c_str, stmt.selector->length);
                    if (sel == "values") {
                        kind = schema::IndexKind::Values;
                    } else if (sel == "keys") {
                        kind = schema::IndexKind::Keys;
                    } else if (sel == "entries") {
                        kind = schema::IndexKind::Entries;
                    } else if (sel == "full") {
                        kind = schema::IndexKind::Full;
                    } else {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Unsupported index selector function"};
                    }
                }

                if (type_matches_tag<type::Basic>(col_type)) {
                    if (stmt.selector) {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Selector function on non-collection column"};
                    }
                } else if (type_matches_tag<type::List>(col_type) || type_matches_tag<type::Set>(col_type)) {
                    if (kind == schema::IndexKind::Keys || kind == schema::IndexKind::Entries) {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Cannot create keys() or entries() index on a list/set column"};
                    }
                    const auto& el = type_matches_tag<type::List>(col_type)
                                       ? get<type::List>(col_type).element
                                       : get<type::Set>(col_type).key;
                    if (!type_matches_tag<type::Basic>(el.value)) {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Cannot index collection of complex element type"};
                    }
                } else if (type_matches_tag<type::Map>(col_type)) {
                    // @note Cassandra/Scylla default plain `CREATE INDEX ON map_col` to values(map_col).
                    const auto& m = get<type::Map>(col_type);
                    if (!type_matches_tag<type::Basic>(m.key.value) || !type_matches_tag<type::Basic>(m.value.value)) {
                        co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Cannot index map with complex key or value type"};
                    }
                } else {
                    co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Cannot create index on column of this type"};
                }
                if (tbl->cols[*col_idx].key_kind == schema::KeyKind::PartitionKey && tbl->partition_key_col_indices.length == 1) {
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

                if (schema::read_index(*tbl, index_name).error == schema::Error::None) {
                    co_return stmt.if_not_exists ? create_void_success()
                                                 : ExecutionResult{.status = ExecutionStatus::AlreadyExists, .message = "index already exists"};
                }

                auto res = co_await schema::create_index(engine.schema, *tbl, *col_idx, index_name, kind);
                if (res.error != schema::Error::None) {
                    co_return create_server_error("failed to create index");
                }
                co_await backfill_index(engine, tbl, *res.value);

                co_return create_schema_changed(ks_name, stmt.table.table_name);
            } else if constexpr (SameAs<T, DropIndex>) {
                String8 ks_name = resolve_ks_name(stmt.index_name.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return (stmt.if_exists && ksr.not_found) ? create_void_success() : move(*ksr.error);
                }
                auto ks = ksr.ks;

                // @note DROP INDEX <name> doesn't say which table; scan every table in the keyspace.
                String8        idx_name_str(stmt.index_name.table_name.c_str, stmt.index_name.table_name.length);
                schema::Table* found_tbl = nullptr;
                for (auto& tbl : ks->tbls) {
                    if (!tbl.tombstone) {
                        if (schema::read_index(tbl, idx_name_str).error == schema::Error::None) {
                            found_tbl = &tbl;
                            break;
                        }
                    }
                }
                if (found_tbl == nullptr) {
                    co_return (stmt.if_exists) ? create_void_success()
                                               : ExecutionResult{.status = ExecutionStatus::Invalid, .message = "index not found"};
                }
                co_await schema::delete_index(engine.schema, *found_tbl, idx_name_str);
                co_return create_schema_changed(ks_name, found_tbl->name);
            } else if constexpr (SameAs<T, CreateType>) {
                String8 ks_name = resolve_ks_name(stmt.name.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks = ksr.ks;
                if (schema::read_udt(*ks, stmt.name.table_name).value != nullptr) {
                    if (stmt.if_not_exists) {
                        co_return create_void_success();
                    }
                    ExecutionResult r;
                    r.status          = ExecutionStatus::AlreadyExists;
                    r.keyspace        = AutoString8(ks_name);
                    r.message_storage = AutoString8("Type ") + AutoString8(ks_name) + "." + AutoString8(stmt.name.table_name) + " already exists";
                    r.message         = String8(r.message_storage);
                    co_return r;
                }

                DynamicArray<AutoString8> field_names;
                DynamicArray<type::Type>  field_types;
                for (const auto& fd : stmt.fields) {
                    auto resolved = schema::resolve_type_ast(engine.schema, ks_name, fd.type);
                    if (resolved.error != schema::Error::None) {
                        ExecutionResult er;
                        er.status          = ExecutionStatus::Invalid;
                        er.keyspace        = AutoString8(ks_name);
                        er.message_storage = AutoString8(resolved.message);
                        er.message         = String8(er.message_storage);
                        co_return er;
                    }
                    push_back(field_names, AutoString8(fd.name.identifier));
                    push_back(field_types, move(resolved.value));
                }
                auto res = co_await schema::create_udt(engine.schema, *ks, stmt.name.table_name, move(field_names), move(field_types));
                if (res.error != schema::Error::None) {
                    ExecutionResult er;
                    er.status          = ExecutionStatus::Invalid;
                    er.keyspace        = AutoString8(ks_name);
                    er.message_storage = AutoString8(res.message);
                    er.message         = String8(er.message_storage);
                    co_return er;
                }
                co_return create_schema_changed(ks_name);
            } else if constexpr (SameAs<T, AlterType>) {
                String8 ks_name = resolve_ks_name(stmt.name.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return move(*ksr.error);
                }
                auto ks = ksr.ks;
                if (schema::read_udt(*ks, stmt.name.table_name).value == nullptr) {
                    ExecutionResult r;
                    r.status          = ExecutionStatus::Invalid;
                    r.keyspace        = AutoString8(ks_name);
                    r.message_storage = AutoString8("Type ") + AutoString8(ks_name) + "." + AutoString8(stmt.name.table_name) + " does not exist";
                    r.message         = String8(r.message_storage);
                    co_return r;
                }
                if (type_matches_tag<AlterType::AddFieldInstruction>(stmt.instruction)) {
                    const auto& ins = get<AlterType::AddFieldInstruction>(stmt.instruction);
                    for (const auto& fd : ins.fields) {
                        auto resolved = schema::resolve_type_ast(engine.schema, ks_name, fd.type);
                        if (resolved.error != schema::Error::None) {
                            ExecutionResult er;
                            er.status          = ExecutionStatus::Invalid;
                            er.keyspace        = AutoString8(ks_name);
                            er.message_storage = AutoString8(resolved.message);
                            er.message         = String8(er.message_storage);
                            co_return er;
                        }
                        auto res = co_await schema::alter_udt_add_field(engine.schema, *ks, stmt.name.table_name, AutoString8(fd.name.identifier), move(resolved.value));
                        if (res.error != schema::Error::None) {
                            ExecutionResult er;
                            er.status          = ExecutionStatus::Invalid;
                            er.keyspace        = AutoString8(ks_name);
                            er.message_storage = AutoString8(res.message);
                            er.message         = String8(er.message_storage);
                            co_return er;
                        }
                    }
                } else {
                    const auto& ins = get<AlterType::RenameFieldInstruction>(stmt.instruction);
                    auto        res = co_await schema::alter_udt_rename_fields(engine.schema, *ks, stmt.name.table_name, ins.old_to_new_fields);
                    if (res.error != schema::Error::None) {
                        ExecutionResult er;
                        er.status          = ExecutionStatus::Invalid;
                        er.keyspace        = AutoString8(ks_name);
                        er.message_storage = AutoString8(res.message);
                        er.message         = String8(er.message_storage);
                        co_return er;
                    }
                }
                co_return create_schema_changed(ks_name);
            } else if constexpr (SameAs<T, DropType>) {
                String8 ks_name = resolve_ks_name(stmt.name.keyspace_name, current_keyspace);
                auto    ksr     = resolve_ks(engine, ks_name);
                if (ksr.error) {
                    co_return (stmt.if_exists && ksr.not_found) ? create_void_success() : move(*ksr.error);
                }
                auto ks = ksr.ks;
                if (schema::read_udt(*ks, stmt.name.table_name).value == nullptr) {
                    if (stmt.if_exists) {
                        co_return create_void_success();
                    }
                    ExecutionResult r;
                    r.status          = ExecutionStatus::Invalid;
                    r.keyspace        = AutoString8(ks_name);
                    r.message_storage = AutoString8("Type ") + AutoString8(ks_name) + "." + AutoString8(stmt.name.table_name) + " does not exist";
                    r.message         = String8(r.message_storage);
                    co_return r;
                }
                auto res = co_await schema::delete_udt(engine.schema, *ks, stmt.name.table_name);
                if (res.error != schema::Error::None) {
                    ExecutionResult er;
                    er.status          = ExecutionStatus::Invalid;
                    er.keyspace        = AutoString8(ks_name);
                    er.message_storage = AutoString8(res.message);
                    er.message         = String8(er.message_storage);
                    co_return er;
                }
                co_return create_schema_changed(ks_name);
            } else if constexpr (SameAs<T, Batch>) {
                ZoneScopedN("engine::batch");
                // @note batch-level USING TIMESTAMP/TTL falls through to children that don't supply their own.
                warn_using_timestamp_single_node(engine, stmt.using_parameters, "BATCH USING TIMESTAMP not supported in non-single-node mode", "ignoring BATCH USING TIMESTAMP (single-node no-op)");

                auto dispatch_child = [&engine, &ctx, &current_keyspace, &stmt](const auto& m) -> coroutine::Task<ExecutionResult> {
                    using M     = Decay<decltype(m)>;
                    M child_mod = m;
                    if (child_mod.using_parameters.length == 0) {
                        for (const auto& p : stmt.using_parameters) {
                            push_back(child_mod.using_parameters, p);
                        }
                    }
                    bool has_conditional = false;
                    if constexpr (SameAs<M, Insert>) {
                        has_conditional = child_mod.if_not_exists;
                    } else {
                        has_conditional = static_cast<bool>(child_mod.if_);
                    }
                    if (has_conditional) {
                        co_return ExecutionResult{
                            .status  = ExecutionStatus::Invalid,
                            .message = "Conditional statements (IF / IF NOT EXISTS) inside BATCH are not supported",
                        };
                    }
                    Statement child;
                    child.value = move(child_mod);
                    co_return co_await execute_inside_transaction(engine, child, ctx, current_keyspace);
                };

                for (const auto& mod : stmt.statements) {
                    ExecutionResult child_res = co_await visit(mod.value, dispatch_child);
                    if (child_res.status != ExecutionStatus::Success) {
                        co_return child_res;
                    }
                }
                co_return create_void_success();
            } else {
                static_assert(false, "Unhandled statement type in engine::execute_inside_transaction");
            }
        });
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, const Statement& statement) {
        co_return co_await execute(engine, statement, engine.current_keyspace);
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
        if (Optional<U64> ci = schema::find_column(tbl, col_name); ci) {
            return tbl.cols[*ci].type;
        }
        // Unknown column: use int as fallback so numeric test values serialize without driver error.
        // The query will fail server-side with ColumnNotFound regardless of the type reported here.
        return type::create_basic(type::Basic::int_);
    }

    static type::Type subscript_hint_type(type::Type col_type) {
        return visit(col_type.value, [](const auto& v) -> type::Type {
            using V = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<V, type::Map>) {
                return v.key;
            } else if constexpr (SameAs<V, type::List> || SameAs<V, type::Vector>) {
                return type::create_basic(type::Basic::int_);
            } else {
                return type::create_basic(type::Basic::text);
            }
        });
    }

    static type::Type subscript_rhs_hint_type(type::Type col_type) {
        return visit(col_type.value, [&col_type](const auto& v) -> type::Type {
            using V = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<V, type::Map>) {
                return v.value;
            } else if constexpr (SameAs<V, type::List> || SameAs<V, type::Vector>) {
                return v.element;
            } else {
                return col_type;
            }
        });
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

    static void collect_bind_in_twi(const TermWithIdentifiers& twi, type::Type hint_type, DynamicArray<BindVariableSpec>& out) {
        visit(twi.value, [&](const auto& v) {
            using V = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<V, BindMarker>) {
                emplace_back(out, BindVariableSpec{.name = AutoString8(v.identifier), .type = hint_type});
            } else if constexpr (SameAs<V, TOIArithmeticOperation>) {
                visit(v.value, [&](const auto& arith) {
                    using AT = RemoveCVRef<decltype(arith)>;
                    if constexpr (SameAs<AT, TOIUnaryMinus>) {
                        collect_bind_in_twi(arith.operand, hint_type, out);
                    } else if constexpr (SameAs<AT, TOIBinaryArithmetic>) {
                        collect_bind_in_twi(arith.lhs, hint_type, out);
                        collect_bind_in_twi(arith.rhs, hint_type, out);
                    }
                });
            } else if constexpr (SameAs<V, ListOrVectorLiteral> || SameAs<V, TupleLiteral>) {
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

    // @note resolves the bind-marker payload type for a `WHERE col OP ?` predicate.
    // For IN ?, the marker carries the whole list. For CONTAINS / CONTAINS KEY / col[k] = ?,
    // the marker carries an element of the collection: list/set element, map value, or map key.
    // @note this runs on unvalidated user CQL, so semantically wrong combinations (e.g.
    // CONTAINS on a basic column) fall through to col_type and let the planner emit Invalid.
    static type::Type bind_type_for_where_relation(const WhereClause::ColumnExpressionRelation& rel, type::Type col_type) {
        if (rel.operator_ == Operator::in && type_matches_tag<BindMarker>(rel.value.value)) {
            return type::create_list(col_type);
        }
        if (rel.operator_ != Operator::contains && rel.operator_ != Operator::contains_key) {
            return col_type;
        }
        return visit(col_type.value, [&](const auto& v) -> type::Type {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::List>) {
                return v.element;
            } else if constexpr (SameAs<T, type::Set>) {
                return v.key;
            } else if constexpr (SameAs<T, type::Map>) {
                return rel.operator_ == Operator::contains_key ? v.key : v.value;
            } else if constexpr (SameAs<T, type::Vector>) {
                return v.element;
            } else {
                return col_type;
            }
        });
    }

    static void collect_bind_in_where(const WhereClause& where, const schema::Table& tbl, DynamicArray<BindVariableSpec>& out) {
        for (U64 i = 0; i < where.relations.length; i++) {
            visit(where.relations[i].value, [&](const auto& rel) {
                using R = RemoveCVRef<decltype(rel)>;
                if constexpr (SameAs<R, WhereClause::ColumnExpressionRelation>) {
                    type::Type col_t = col_type_in_table(tbl, String8(rel.column.identifier));
                    type::Type t     = bind_type_for_where_relation(rel, col_t);
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
                    } else if (rel.values.length == 1 && rel.columns.length > 1 && type_matches_tag<BindMarker>(rel.values[0].value)) {
                        // (a, b) = ? — single bind variable typed as a tuple of the column types.
                        DynamicArray<type::Type> elems;
                        for (U64 ci = 0; ci < rel.columns.length; ci++) {
                            push_back(elems, col_type_in_table(tbl, String8(rel.columns[ci].identifier)));
                        }
                        collect_bind_in_term(rel.values[0], type::create_tuple(move(elems), true), out);
                    } else {
                        for (U64 vi = 0; vi < rel.values.length; vi++) {
                            String8    col = vi < rel.columns.length ? String8(rel.columns[vi].identifier) : String8{};
                            type::Type t   = col.length ? col_type_in_table(tbl, col) : type::create_basic(type::Basic::text);
                            collect_bind_in_term(rel.values[vi], t, out);
                        }
                    }
                } else if constexpr (SameAs<R, WhereClause::TokenRelation>) {
                    collect_bind_in_term(rel.value, type::create_basic(type::Basic::bigint), out);
                } else if constexpr (SameAs<R, WhereClause::SubscriptedRelation>) {
                    type::Type col_t = col_type_in_table(tbl, String8(rel.column.identifier));
                    collect_bind_in_term(rel.subscript, subscript_hint_type(col_t), out);
                    collect_bind_in_term(rel.value, subscript_rhs_hint_type(col_t), out);
                }
            });
        }
    }

    static void collect_bind_variables_insert(Engine& engine, const Insert& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
        if (tbl == nullptr) {
            return;
        }

        if (type_matches_tag<Insert::NamesValues>(stmt.insert_clause)) {
            const auto& nv = get<Insert::NamesValues>(stmt.insert_clause);
            for (U64 i = 0; i < nv.values.length; i++) {
                if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                    String8 col_name = nv.names[i].identifier;
                    emplace_back(out, BindVariableSpec{.name = AutoString8(col_name), .type = col_type_in_table(*tbl, col_name)});
                }
            }
        }
        collect_bind_in_using_params(stmt.using_parameters, out);
    }

    static void collect_bind_variables_update(Engine& engine, const Update& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
        if (tbl == nullptr) {
            return;
        }

        collect_bind_in_using_params(stmt.using_parameters, out);
        for (U64 i = 0; i < stmt.assignments.length; i++) {
            const auto& asgn  = stmt.assignments[i];
            type::Type  col_t = col_type_in_table(*tbl, String8(asgn.target.column.identifier));
            if (const Term* sub = try_subscript_index_term(asgn.target)) {
                collect_bind_in_term(*sub, subscript_hint_type(col_t), out);
                collect_bind_in_twi(asgn.value, subscript_rhs_hint_type(col_t), out);
            } else {
                collect_bind_in_twi(asgn.value, col_t, out);
            }
        }
        collect_bind_in_where(stmt.where, *tbl, out);
    }

    static void collect_bind_variables_delete(Engine& engine, const Delete& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
        if (tbl == nullptr) {
            return;
        }

        collect_bind_in_using_params(stmt.using_parameters, out);
        for (U64 i = 0; i < stmt.selections.length; i++) {
            const auto& sel = stmt.selections[i];
            if (const Term* sub = try_subscript_index_term(sel)) {
                type::Type col_t = col_type_in_table(*tbl, String8(sel.column.identifier));
                collect_bind_in_term(*sub, subscript_hint_type(col_t), out);
            }
        }
        collect_bind_in_where(stmt.where, *tbl, out);
    }

    static void collect_bind_variables_select(Engine& engine, const Select& stmt, DynamicArray<BindVariableSpec>& out, String8 current_keyspace) {
        String8 ks_name = resolve_ks_name(stmt.from.keyspace_name, current_keyspace);
        auto    ks      = schema::read_keyspace(engine.schema, ks_name).value;
        if (ks == nullptr) {
            return;
        }
        auto tbl = schema::read_table(*ks, stmt.from.table_name).value;
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
            } else if constexpr (SameAs<T, Batch>) {
                collect_bind_in_using_params(stmt.using_parameters, result);
                for (const auto& mod : stmt.statements) {
                    visit(mod.value, [&](const auto& m) {
                        using M = RemoveCVRef<decltype(m)>;
                        if constexpr (SameAs<M, Insert>) {
                            collect_bind_variables_insert(engine, m, result, current_keyspace);
                        } else if constexpr (SameAs<M, Update>) {
                            collect_bind_variables_update(engine, m, result, current_keyspace);
                        } else if constexpr (SameAs<M, Delete>) {
                            collect_bind_variables_delete(engine, m, result, current_keyspace);
                        }
                    });
                }
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

    static void bind_in_twi(TermWithIdentifiers& twi, DynamicArray<Term>& bv, U64& idx) {
        if (type_matches_tag<BindMarker>(twi.value)) {
            if (idx < bv.length) {
                twi = TermWithIdentifiers(move(bv[idx++]));
            }
            return;
        }
        visit(twi.value, [&](auto& v) {
            using V = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<V, TOIArithmeticOperation>) {
                visit(v.value, [&](auto& arith) {
                    using AT = RemoveCVRef<decltype(arith)>;
                    if constexpr (SameAs<AT, TOIUnaryMinus>) {
                        bind_in_twi(arith.operand, bv, idx);
                    } else if constexpr (SameAs<AT, TOIBinaryArithmetic>) {
                        bind_in_twi(arith.lhs, bv, idx);
                        bind_in_twi(arith.rhs, bv, idx);
                    }
                });
            } else if constexpr (SameAs<V, ListOrVectorLiteral> || SameAs<V, TupleLiteral>) {
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

    // @note UNSET/null bound to USING TIMESTAMP/TTL collapses to 0 (treated as "not present").
    static void bind_in_using_params(DynamicArray<UpdateParameter>& params, DynamicArray<Term>& bv, U64& idx) {
        for (U64 i = 0; i < params.length; i++) {
            if (!type_matches_tag<BindMarker>(params[i].value)) {
                continue;
            }
            if (idx >= bv.length) {
                idx++;
                continue;
            }
            const auto& b = bv[idx++];
            if (type_matches_tag<Literal>(b.value)) {
                const auto& c = get<Literal>(b.value);
                if (type_matches_tag<S64>(c.value)) {
                    params[i].value = get<S64>(c.value);
                } else if (type_matches_tag<Unset>(c.value) || type_matches_tag<Null>(c.value)) {
                    params[i].value = S64(0);
                }
            }
        }
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
                } else if constexpr (SameAs<R, WhereClause::SubscriptedRelation>) {
                    bind_in_term(rel.subscript, bv, idx);
                    bind_in_term(rel.value, bv, idx);
                }
            });
        }
    }

    void bind_values_to_statement(Statement& stmt, DynamicArray<Term>& bound_values) {
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
                bind_in_using_params(s.using_parameters, bound_values, idx);
            } else if constexpr (SameAs<T, Update>) {
                bind_in_using_params(s.using_parameters, bound_values, idx);
                for (U64 i = 0; i < s.assignments.length; i++) {
                    auto& asgn = s.assignments[i];
                    if (Term* sub = try_subscript_index_term(asgn.target)) {
                        bind_in_term(*sub, bound_values, idx);
                    }
                    bind_in_twi(asgn.value, bound_values, idx);
                }
                bind_in_where(s.where, bound_values, idx);
            } else if constexpr (SameAs<T, Delete>) {
                bind_in_using_params(s.using_parameters, bound_values, idx);
                for (U64 i = 0; i < s.selections.length; i++) {
                    if (Term* sub = try_subscript_index_term(s.selections[i])) {
                        bind_in_term(*sub, bound_values, idx);
                    }
                }
                bind_in_where(s.where, bound_values, idx);
            } else if constexpr (SameAs<T, Batch>) {
                bind_in_using_params(s.using_parameters, bound_values, idx);
                for (U64 mi = 0; mi < s.statements.length; mi++) {
                    visit(s.statements[mi].value, [&](auto& m) {
                        using M = RemoveCVRef<decltype(m)>;
                        if constexpr (SameAs<M, Insert>) {
                            if (type_matches_tag<Insert::NamesValues>(m.insert_clause)) {
                                auto& nv = get<Insert::NamesValues>(m.insert_clause);
                                for (U64 i = 0; i < nv.values.length && idx < bound_values.length; i++) {
                                    if (type_matches_tag<BindMarker>(nv.values[i].value)) {
                                        nv.values[i] = move(bound_values[idx++]);
                                    }
                                }
                            }
                            bind_in_using_params(m.using_parameters, bound_values, idx);
                        } else if constexpr (SameAs<M, Update>) {
                            bind_in_using_params(m.using_parameters, bound_values, idx);
                            for (U64 i = 0; i < m.assignments.length; i++) {
                                auto& asgn = m.assignments[i];
                                if (Term* sub = try_subscript_index_term(asgn.target)) {
                                    bind_in_term(*sub, bound_values, idx);
                                }
                                bind_in_twi(asgn.value, bound_values, idx);
                            }
                            bind_in_where(m.where, bound_values, idx);
                        } else if constexpr (SameAs<M, Delete>) {
                            bind_in_using_params(m.using_parameters, bound_values, idx);
                            for (U64 i = 0; i < m.selections.length; i++) {
                                if (Term* sub = try_subscript_index_term(m.selections[i])) {
                                    bind_in_term(*sub, bound_values, idx);
                                }
                            }
                            bind_in_where(m.where, bound_values, idx);
                        }
                    });
                }
            } else if constexpr (SameAs<T, Select>) {
                if (static_cast<bool>(s.where)) {
                    bind_in_where(*s.where, bound_values, idx);
                }
                if (type_matches_tag<BindMarker>(s.limit.value)) {
                    if (idx < bound_values.length) {
                        const auto& bv = bound_values[idx++];
                        if (type_matches_tag<Literal>(bv.value)) {
                            const auto& c = get<Literal>(bv.value);
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
        if (existing != nullptr && existing->schema_version == engine.schema.version) {
            return {.status = ExecutionStatus::Success, .id = query_hash, .entry = existing};
        }

        auto pr = parsers::parse(query);
        if (!pr.statement) {
            PrepareResult r{.status = ExecutionStatus::SyntaxError};
            r.message_storage = move(pr.err);
            r.message         = (pr.err.length > 0) ? String8(r.message_storage.c_str, r.message_storage.length) : String8("Failed to parse CQL");
            return r;
        }

        // Fresh entry, or a stale one (schema changed since it was derived) refreshed in place.
        auto& entry          = (existing != nullptr) ? *existing : insert(engine.prepared_cache, query_hash);
        entry.query_string   = AutoString8(query);
        entry.bind_variables = collect_bind_variables_with_keyspace(engine, *pr.statement, current_keyspace);
        entry.pk_index       = -1;
        entry.schema_version = engine.schema.version;

        visit(pr.statement->value, [&, current_keyspace](const auto& stmt) {
            using T = RemoveCVRef<decltype(stmt)>;
            if constexpr (SameAs<T, Insert>) {
                String8 ks_name = resolve_ks_name(stmt.table.keyspace_name, current_keyspace);
                entry.keyspace  = AutoString8(ks_name);
                entry.table     = AutoString8(stmt.table.table_name);

                auto ks = schema::read_keyspace(engine.schema, ks_name).value;
                if (ks == nullptr) {
                    return;
                }
                auto tbl = schema::read_table(*ks, stmt.table.table_name).value;
                if (tbl == nullptr) {
                    return;
                }
                for (U64 i = 0; i < entry.bind_variables.length; i++) {
                    if (tbl->partition_key_col_indices.length > 0 && tbl->cols[tbl->partition_key_col_indices[0]].name == entry.bind_variables[i].name) {
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

        auto pr = parsers::parse(String8(entry->query_string));
        if (!pr.statement) {
            co_return ExecutionResult{.status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query"};
        }

        co_return co_await execute(engine, *pr.statement, move(bound_values));
    }

    coroutine::Task<ExecutionResult> execute(Engine& engine, U64 prepared_id, DynamicArray<Term>&& bound_values, AutoString8& current_keyspace) {
        auto* entry = find(engine.prepared_cache, prepared_id);
        if (entry == nullptr) {
            co_return ExecutionResult{.status = ExecutionStatus::Invalid, .message = "Prepared statement not found"};
        }

        auto pr = parsers::parse(String8(entry->query_string));
        if (!pr.statement) {
            co_return ExecutionResult{.status = ExecutionStatus::ServerError, .message = "Failed to re-parse prepared query"};
        }

        co_return co_await execute(engine, *pr.statement, move(bound_values), current_keyspace);
    }
}
