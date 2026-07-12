module;
#include <plexdb/macros/macros.h>
#include <coroutine>

module cql.engine.schema;

import plexdb.threads;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;
import plexdb.os;
import plexdb.support.sort;

import cql.engine.schema.types;
import cql.engine.types;
import cql.engine.clustering_compare;
import cql.log;

using namespace plexdb;

// ============================================================================
// small blob helpers
// ============================================================================
namespace cql::schema {
    static coroutine::Task<void> get_bytes(blob::BlobDynamicPaged& blob, AutoString8& dst, U64* offset) {
        co_await blob::get(blob, reinterpret_cast<U8*>(dst.c_str), dst.length, *offset);
        *offset += dst.length;
    }
    static coroutine::Task<void> put_bytes(blob::BlobDynamicPaged& blob, String8 src, U64* offset) {
        co_await blob::update(blob, reinterpret_cast<const U8*>(src.data), src.length, *offset);
        *offset += src.length;
    }
}

// ============================================================================
// load / create_schema
// ============================================================================
namespace cql::schema {
    static void rebuild_table_indices(Table& tbl) {
        clear(tbl.partition_key_col_indices);
        clear(tbl.clustering_key_col_indices);
        clear(tbl.static_col_indices);
        clear(tbl.regular_col_indices);
        clear(tbl.select_star_col_indices);
        clear(tbl.cols_by_name);
        for (U64 ci = 0; ci < tbl.cols.length; ci++) {
            const Column& col = tbl.cols[ci];
            if (col.tombstone) {
                continue;
            }
            insert(tbl.cols_by_name, col.name, ci);
            if (col.key_kind == KeyKind::PartitionKey) {
                push_back(tbl.partition_key_col_indices, ci);
            } else if (col.key_kind == KeyKind::ClusteringKey) {
                push_back(tbl.clustering_key_col_indices, ci);
            } else if (col.is_static) {
                push_back(tbl.static_col_indices, ci);
            } else {
                push_back(tbl.regular_col_indices, ci);
            }
        }
        auto by_key_position = [&tbl](U64 a, U64 b) {
            return tbl.cols[a].key_position < tbl.cols[b].key_position;
        };
        auto by_name = [&tbl](U64 a, U64 b) {
            const String8& na  = tbl.cols[a].name;
            const String8& nb  = tbl.cols[b].name;
            U64            lim = min(na.length, nb.length);
            S32            cmp = lim > 0 ? os::memory_compare(na.data, nb.data, lim) : 0;
            return cmp != 0 ? cmp < 0 : na.length < nb.length;
        };
        support::sort::small_sort<U64, U64>(tbl.partition_key_col_indices, by_key_position);
        support::sort::small_sort<U64, U64>(tbl.clustering_key_col_indices, by_key_position);
        support::sort::small_sort<U64, U64>(tbl.static_col_indices, by_name);
        support::sort::sort<U64, U64>(tbl.regular_col_indices, by_name);

        // @note non-Basic (frozen collection) CK types store a COUNT sentinel; the comparator
        // asserts on use, not here, matching today's behavior for these still-unsupported types.
        clear(tbl.clustering_key_specs);
        for (U64 ci : tbl.clustering_key_col_indices) {
            const Column& col   = tbl.cols[ci];
            type::Basic   dtype = type_matches_tag<type::Basic>(col.type.value) ? get<type::Basic>(col.type.value) : type::Basic::COUNT;
            push_back(tbl.clustering_key_specs, clustering_compare::ClusteringColumnSpec{dtype, col.clustering_order});
        }

        reserve(tbl.select_star_col_indices, tbl.partition_key_col_indices.length + tbl.clustering_key_col_indices.length + tbl.static_col_indices.length + tbl.regular_col_indices.length);
        for (U64 ci : tbl.partition_key_col_indices) {
            push_back(tbl.select_star_col_indices, ci);
        }
        for (U64 ci : tbl.clustering_key_col_indices) {
            push_back(tbl.select_star_col_indices, ci);
        }
        for (U64 ci : tbl.static_col_indices) {
            push_back(tbl.select_star_col_indices, ci);
        }
        for (U64 ci : tbl.regular_col_indices) {
            push_back(tbl.select_star_col_indices, ci);
        }
    }

    coroutine::Task<> load(Schema& schema, Pager* in_pager, U64 page) {
        co_await blob::load(schema.schema_blob, in_pager, page, sizeof(SchemaHeader));

        SchemaHeader schema_header{};
        co_await blob::tget(schema.schema_blob, &schema_header);

        co_await blob::load(schema.keyspaces_blob, in_pager, schema_header.keyspaces_page);
        co_await blob::load(schema.tables_blob, in_pager, schema_header.tables_page);
        co_await blob::load(schema.columns_blob, in_pager, schema_header.columns_page);
        co_await blob::load(schema.udts_blob, in_pager, schema_header.udts_page);
        co_await blob::load(schema.indexes_blob, in_pager, schema_header.indexes_page);

        //
        // keyspaces
        //
        for (U64 off = 0; off < schema.keyspaces_blob.size_bytes;) {
            KeyspaceStorage ks_storage{};
            ks_storage.offset_in_blob_bytes = off;
            co_await blob::tget(schema.keyspaces_blob, &ks_storage.header, &off);
            ks_storage.name = AutoString8{ks_storage.header.name_length};
            co_await get_bytes(schema.keyspaces_blob, ks_storage.name, &off);
            push_back(schema.storage.keyspaces, move(ks_storage));
        }

        //
        // tables (link into keyspaces.tables)
        //
        for (U64 off = 0; off < schema.tables_blob.size_bytes;) {
            TableStorage tbl_storage{};
            tbl_storage.offset_in_blob_bytes = off;
            co_await blob::tget(schema.tables_blob, &tbl_storage.header, &off);
            tbl_storage.name = AutoString8{tbl_storage.header.name_length};
            co_await get_bytes(schema.tables_blob, tbl_storage.name, &off);
            assert_true(tbl_storage.header.keyspace_idx < schema.storage.keyspaces.length, "table references invalid keyspace_idx");
            push_back(schema.storage.keyspaces[tbl_storage.header.keyspace_idx].tables, schema.storage.tables.length);
            push_back(schema.storage.tables, move(tbl_storage));
        }

        //
        // build runtime Keyspace / Table views (without columns / udts yet)
        //
        reserve(schema.keyspaces, schema.storage.keyspaces.length);
        for (U64 ks_idx = 0; ks_idx < schema.storage.keyspaces.length; ks_idx++) {
            const KeyspaceStorage& ks_storage = schema.storage.keyspaces[ks_idx];
            Keyspace               ks{};
            ks.idx                = ks_idx;
            ks.tombstone          = ks_storage.header.tombstone != 0;
            ks.name               = String8{ks_storage.name.c_str, ks_storage.name.length};
            ks.replication_class  = ks_storage.header.replication_class;
            ks.replication_factor = ks_storage.header.replication_factor;
            ks.do_durable_writes  = ks_storage.header.do_durable_writes != 0;

            for (U64 tbl_storage_idx : ks_storage.tables) {
                const TableStorage& tbl_storage = schema.storage.tables[tbl_storage_idx];
                Table               tbl{};
                tbl.idx       = tbl_storage_idx;
                tbl.tombstone = tbl_storage.header.tombstone != 0;
                tbl.name      = String8{tbl_storage.name.c_str, tbl_storage.name.length};
                tbl.options   = tbl_storage.header.options;
                tbl.btree     = PartitionBTree{
                    in_pager, tbl_storage.header.btree_page,
                    btree::FixedKeyPolicy<S64>{}, btree::FixedValuePolicy<sizeof(PartitionEntry)>{}
                };
                push_back(ks.tbls, move(tbl));
            }
            push_back(schema.keyspaces, move(ks));
        }
        for (U64 ks_idx = 0; ks_idx < schema.keyspaces.length; ks_idx++) {
            Keyspace& ks = schema.keyspaces[ks_idx];
            if (!ks.tombstone) {
                insert(schema.keyspaces_by_name, ks.name, ks_idx);
            }
            for (U64 ti = 0; ti < ks.tbls.length; ti++) {
                if (!ks.tbls[ti].tombstone) {
                    insert(ks.tbls_by_name, ks.tbls[ti].name, ti);
                }
            }
        }

        //
        // UDTs: single-pass walk over udts_blob. UDT-decl records precede their field records,
        // so by the time a field record's type is read all referenced UDTs are bound.
        //
        // record_to_storage_idx[record_idx] = storage.udts index for that record (always pushed,
        // even if tombstoned, to keep indices aligned with parent_udt_record_idx).
        //
        DynamicArray<U64> record_to_storage_idx;
        for (U64 off = 0; off < schema.udts_blob.size_bytes;) {
            UdtRecordHeader header{};
            U64             header_off = off;
            co_await blob::tget(schema.udts_blob, &header, &off);
            if (header.is_field == 0) {
                UdtStorage udt_storage{};
                udt_storage.offset_in_blob_bytes = header_off;
                udt_storage.header               = header;
                udt_storage.keyspace             = AutoString8{header.keyspace_length};
                co_await get_bytes(schema.udts_blob, udt_storage.keyspace, &off);
                udt_storage.name = AutoString8{header.name_length};
                co_await get_bytes(schema.udts_blob, udt_storage.name, &off);

                push_back(record_to_storage_idx, schema.storage.udts.length);

                bool live = header.tombstone == 0;
                if (live) {
                    String8 ks_name{udt_storage.keyspace.c_str, udt_storage.keyspace.length};
                    auto    ks_res = read_keyspace(schema, ks_name);
                    assert_true(ks_res.error == Error::None, "UDT references unknown keyspace on load");
                    type::UDT* deque_udt = &emplace_back(schema.udts, type::UDT{})->value;
                    deque_udt->keyspace  = String8{udt_storage.keyspace.c_str, udt_storage.keyspace.length};
                    deque_udt->name      = String8{udt_storage.name.c_str, udt_storage.name.length};
                    insert(ks_res.value->udts_by_name, deque_udt->name, deque_udt);
                }

                push_back(schema.storage.udts, move(udt_storage));
            } else {
                assert_true(header.parent_udt_record_idx < record_to_storage_idx.length, "udt-field references out-of-range parent");
                U64         storage_idx = record_to_storage_idx[header.parent_udt_record_idx];
                UdtStorage& parent      = schema.storage.udts[storage_idx];
                AutoString8 field_name{header.name_length};
                co_await get_bytes(schema.udts_blob, field_name, &off);

                String8 ks_name{parent.keyspace.c_str, parent.keyspace.length};
                auto    ks_res = read_keyspace(schema, ks_name);
                assert_true(ks_res.error == Error::None, "UDT-field references unknown keyspace on load");

                auto type_res = co_await read_type(schema.udts_blob, ks_res.value, &off);
                assert_true(type_res.error == Error::None, "UDT-field type failed to resolve on load (blob corruption?)");

                bool live = (header.tombstone == 0) && (parent.header.tombstone == 0);
                if (live) {
                    push_back(parent.field_record_offsets, header_off);
                    push_back(parent.field_names, move(field_name));
                    push_back(parent.field_types, move(type_res.value));

                    // refresh String8 / Type views in the deque UDT — owning AutoString8 buffers
                    // do not move when the parent array grows.
                    type::UDT* const* deque_ptr = find(ks_res.value->udts_by_name, String8{parent.name.c_str, parent.name.length});
                    assert_true(deque_ptr != nullptr, "UDT pool missing entry for live udt");
                    type::UDT* du = *deque_ptr;
                    clear(du->field_names);
                    clear(du->field_types);
                    for (U64 i = 0; i < parent.field_names.length; i++) {
                        push_back(du->field_names, String8{parent.field_names[i].c_str, parent.field_names[i].length});
                        push_back(du->field_types, parent.field_types[i]);
                    }
                }
            }
        }

        //
        // columns: types are resolved against the column's parent keyspace's udt pool
        //
        for (U64 off = 0; off < schema.columns_blob.size_bytes;) {
            ColumnStorage col_storage{};
            col_storage.offset_in_blob_bytes = off;
            co_await blob::tget(schema.columns_blob, &col_storage.header, &off);
            col_storage.name = AutoString8{col_storage.header.name_length};
            co_await get_bytes(schema.columns_blob, col_storage.name, &off);

            assert_true(col_storage.header.table_idx < schema.storage.tables.length, "column references invalid table_idx");
            TableStorage& tbl_storage = schema.storage.tables[col_storage.header.table_idx];
            U64           ks_idx      = tbl_storage.header.keyspace_idx;
            assert_true(ks_idx < schema.keyspaces.length, "column references invalid keyspace_idx via table");
            Keyspace* ks = &schema.keyspaces[ks_idx];

            auto type_res = co_await read_type(schema.columns_blob, ks, &off);
            assert_true(type_res.error == Error::None, "column type failed to resolve on load (blob corruption?)");
            col_storage.type = move(type_res.value);

            push_back(tbl_storage.columns, schema.storage.columns.length);

            // attach to runtime Table.cols
            bool attached = false;
            for (auto& tbl : ks->tbls) {
                if (tbl.idx == col_storage.header.table_idx) {
                    Column col{};
                    col.tombstone        = col_storage.header.tombstone != 0;
                    col.is_static        = col_storage.header.is_static != 0;
                    col.name             = String8{col_storage.name.c_str, col_storage.name.length};
                    col.type             = col_storage.type;
                    col.key_kind         = col_storage.header.key_kind;
                    col.key_position     = col_storage.header.key_position;
                    col.clustering_order = col_storage.header.clustering_order;
                    push_back(tbl.cols, move(col));
                    attached = true;
                    break;
                }
            }
            assert_true(attached, "could not attach column to runtime table");

            push_back(schema.storage.columns, move(col_storage));
        }

        // rebuild per-table key/static indices and cols_by_name
        for (auto& ks : schema.keyspaces) {
            for (auto& tbl : ks.tbls) {
                rebuild_table_indices(tbl);
            }
        }

        //
        // indexes
        //
        for (U64 off = 0; off < schema.indexes_blob.size_bytes;) {
            IndexStorage idx_storage{};
            idx_storage.offset_in_blob_bytes = off;
            co_await blob::tget(schema.indexes_blob, &idx_storage.header, &off);
            idx_storage.name = AutoString8{idx_storage.header.name_length};
            co_await get_bytes(schema.indexes_blob, idx_storage.name, &off);

            assert_true(idx_storage.header.table_idx < schema.storage.tables.length, "index references invalid table_idx");
            const TableStorage& tbl_storage = schema.storage.tables[idx_storage.header.table_idx];
            U64                 ks_idx      = tbl_storage.header.keyspace_idx;
            assert_true(ks_idx < schema.keyspaces.length, "index references invalid keyspace_idx via table");
            Keyspace& ks = schema.keyspaces[ks_idx];

            U64 idx_storage_i = schema.storage.indexes.length;
            for (auto& tbl : ks.tbls) {
                if (tbl.idx == idx_storage.header.table_idx) {
                    Index idx{};
                    idx.idx       = idx_storage_i;
                    idx.tombstone = idx_storage.header.tombstone != 0;
                    idx.name      = String8{idx_storage.name.c_str, idx_storage.name.length};
                    idx.col_idx   = idx_storage.header.col_idx;
                    idx.kind      = idx_storage.header.kind;
                    idx.key_specs = make_index_key_specs(tbl, idx.col_idx, idx.kind);
                    idx.btree     = IndexBTree{
                        in_pager, idx_storage.header.btree_page,
                        make_index_key_policy(idx), btree::FixedValuePolicy<sizeof(IndexEntry)>{}
                    };
                    if (!idx.tombstone) {
                        insert(tbl.indexes_by_name, idx.name, tbl.indexes.length);
                    }
                    push_back(tbl.indexes, move(idx));
                    break;
                }
            }
            push_back(schema.storage.indexes, move(idx_storage));
        }
    }

    coroutine::Task<U64> create_schema(Pager& pager) {
        U64 schema_page = co_await blob::create_paged_static(pager, sizeof(SchemaHeader));

        SchemaHeader header{
            .keyspaces_page = co_await blob::create_paged_dynamic(pager, 0),
            .tables_page    = co_await blob::create_paged_dynamic(pager, 0),
            .columns_page   = co_await blob::create_paged_dynamic(pager, 0),
            .udts_page      = co_await blob::create_paged_dynamic(pager, 0),
            .indexes_page   = co_await blob::create_paged_dynamic(pager, 0),
        };

        blob::BlobStaticPaged schema_blob;
        co_await blob::load(schema_blob, &pager, schema_page, sizeof(SchemaHeader));
        co_await blob::tupdate(schema_blob, &header);

        co_return schema_page;
    }
}

// ============================================================================
// lookups
// ============================================================================
namespace cql::schema {
    Result<Keyspace*> read_keyspace(Schema& schema, String8 name) {
        const U64* p = find(schema.keyspaces_by_name, name);
        if (p == nullptr) {
            return {nullptr, Error::MissingKeyspace};
        }
        return {&schema.keyspaces[*p]};
    }
    Result<Table*> read_table([[maybe_unused]] Schema& schema, Keyspace& ks, String8 name) {
        const U64* p = find(ks.tbls_by_name, name);
        if (p == nullptr) {
            return {nullptr, Error::MissingTable};
        }
        return {&ks.tbls[*p]};
    }
    Result<Column*> read_column([[maybe_unused]] Schema& schema, Table& tbl, String8 name) {
        const U64* p = find(tbl.cols_by_name, name);
        if (p == nullptr) {
            return {nullptr, Error::MissingColumn};
        }
        return {&tbl.cols[*p]};
    }
    Result<Index*> read_index([[maybe_unused]] Schema& schema, Table& tbl, String8 name) {
        const U64* p = find(tbl.indexes_by_name, name);
        if (p == nullptr) {
            return {nullptr, Error::MissingIndex};
        }
        return {&tbl.indexes[*p]};
    }
    Result<type::UDT*> read_udt([[maybe_unused]] Schema& schema, Keyspace& ks, String8 name) {
        type::UDT* const* p = find(ks.udts_by_name, name);
        if (p == nullptr) {
            return {nullptr, Error::MissingType};
        }
        return {*p};
    }
}

// ============================================================================
// keyspace option parsing
// ============================================================================
namespace cql::schema {
    [[nodiscard]] static Result<void> parse_replication_option(KeyspaceOptionsParsed& out, const OptionValue& v) {
        if (!type_matches_tag<MapLiteral>(v)) {
            return {Error::InvalidOptions, "replication option is not a map literal"};
        }
        const auto& map = get<MapLiteral>(v);

        // @note Cassandra rejects duplicate keys in the replication map as a SyntaxException.
        for (U64 i = 0; i < map.key_values.length; i++) {
            const auto& ki = map.key_values[i].first;
            if (!type_matches_tag<Literal>(ki.value)) {
                continue;
            }
            const auto& kic = get<Literal>(ki.value);
            if (!type_matches_tag<AutoString8>(kic.value)) {
                continue;
            }
            const auto& ki_str = get<AutoString8>(kic.value);
            for (U64 j = i + 1; j < map.key_values.length; j++) {
                const auto& kj = map.key_values[j].first;
                if (!type_matches_tag<Literal>(kj.value)) {
                    continue;
                }
                const auto& kjc = get<Literal>(kj.value);
                if (!type_matches_tag<AutoString8>(kjc.value)) {
                    continue;
                }
                if (ki_str == get<AutoString8>(kjc.value)) {
                    return {Error::SyntaxOptions, "Multiple definitions for replication option"};
                }
            }
        }

        for (const auto& [key_term, value_term] : map.key_values) {
            if (!type_matches_tag<Literal>(key_term.value)) {
                return {Error::InvalidOptions, "replication option key should be a string"};
            }
            const auto& kc = get<Literal>(key_term.value);
            if (!type_matches_tag<AutoString8>(kc.value)) {
                return {Error::InvalidOptions, "replication option key should be a string"};
            }
            const auto& key = get<AutoString8>(kc.value);

            if (key == "class") {
                if (!type_matches_tag<Literal>(value_term.value)) {
                    return {Error::InvalidOptions, "replication class should be a string"};
                }
                const auto& vc = get<Literal>(value_term.value);
                if (!type_matches_tag<AutoString8>(vc.value)) {
                    return {Error::InvalidOptions, "replication class should be a string"};
                }
                const auto& value = get<AutoString8>(vc.value);

                if (value == "SimpleStrategy" || value == "org.apache.cassandra.locator.SimpleStrategy") {
                    out.replication_class = ReplicationClass::SimpleStrategy;
                } else if (value == "NetworkTopologyStrategy" || value == "org.apache.cassandra.locator.NetworkTopologyStrategy") {
                    out.replication_class = ReplicationClass::NetworkTopologyStrategy;
                } else {
                    return {Error::InvalidOptions, "unknown replication class"};
                }
            } else if (key == "replication_factor") {
                if (!type_matches_tag<Literal>(value_term.value)) {
                    return {Error::InvalidOptions, "replication factor should be an integer"};
                }
                const auto& vc    = get<Literal>(value_term.value);
                S64         value = 0;
                if (type_matches_tag<S64>(vc.value)) {
                    value = get<S64>(vc.value);
                } else if (type_matches_tag<AutoString8>(vc.value)) {
                    value = s64_from_str(get<AutoString8>(vc.value));
                    if (value == 0) {
                        return {Error::InvalidOptions, "replication factor should be an integer"};
                    }
                } else {
                    return {Error::InvalidOptions, "replication factor should be an integer"};
                }
                if (value <= 0) {
                    return {Error::InvalidOptions, "replication factor must be strictly positive"};
                }
                out.replication_factor = static_cast<U64>(value);
            } else {
                // NetworkTopologyStrategy: non-class keys are DC names.
                // The current single-node build only knows "datacenter1".
                if (out.replication_class == ReplicationClass::NetworkTopologyStrategy) {
                    if (key != "datacenter1") {
                        thread_local AutoString8 msg;
                        msg = AutoString8("Unrecognized datacenter name '") + String8(key) + AutoString8("'");
                        return {Error::InvalidOptions, String8(msg)};
                    }
                }
            }
        }
        return {};
    }

    Result<KeyspaceOptionsParsed> parse_keyspace_options(const Options& opts) {
        KeyspaceOptionsParsed out{};
        for (const auto& [key, value] : opts.identifier_values) {
            if (key == "replication") {
                if (auto r = parse_replication_option(out, value); r.error != Error::None) {
                    return {{}, r.error, r.message};
                }
            } else if (key == "durable_writes") {
                if (!type_matches_tag<Literal>(value)) {
                    return {{}, Error::InvalidOptions, "durable write option should be a boolean"};
                }
                const auto& vc = get<Literal>(value);
                if (!type_matches_tag<bool>(vc.value)) {
                    return {{}, Error::InvalidOptions, "durable write option should be a boolean"};
                }
                out.do_durable_writes = get<bool>(vc.value);
            } else {
                return {{}, Error::InvalidOptions, "Unknown option in keyspace WITH"};
            }
        }
        return {out};
    }
}

// ============================================================================
// table option parsing
// ============================================================================
namespace cql::schema {
    static Optional<S32> s32_from_option_value(const OptionValue& v) {
        if (!type_matches_tag<Literal>(v)) {
            return {};
        }
        const auto& c = get<Literal>(v);
        if (type_matches_tag<S64>(c.value)) {
            return Optional<S32>{static_cast<S32>(get<S64>(c.value))};
        }
        if (type_matches_tag<AutoString8>(c.value)) {
            S32 vv = s32_from_str(get<AutoString8>(c.value));
            return Optional<S32>{vv};
        }
        return {};
    }
    static Optional<S64> s64_seconds_from_option_value(const OptionValue& v) {
        if (!type_matches_tag<Literal>(v)) {
            return {};
        }
        const auto& c = get<Literal>(v);
        if (type_matches_tag<S64>(c.value)) {
            return Optional<S64>{get<S64>(c.value)};
        }
        if (type_matches_tag<AutoString8>(c.value)) {
            return Optional<S64>{s64_from_str(get<AutoString8>(c.value))};
        }
        return {};
    }

    static bool is_obsolete_table_option(String8 key) {
        return key == "default_validation" || key == "key_validation" || key == "row_cache_size"
            || key == "key_cache_size" || key == "index_interval" || key == "replicate_on_write";
    }

    Result<TableOptionsParsed> parse_table_options(const CreateTable::TableOptions& opts) {
        TableOptionsParsed out{};
        for (const auto& opt : opts.value) {
            Result<TableOptionsParsed> err{};
            visit(opt, [&](const auto& o) {
                using O = RemoveCVRef<decltype(o)>;
                if constexpr (SameAs<O, CreateTable::CompactStorage>) {
                    log::schema_info("ignoring COMPACT STORAGE (single-node no-op)");
                } else if constexpr (SameAs<O, CreateTable::ClusteringOrder>) {
                    // consumed by create_table when assigning per-CK directions
                } else if constexpr (SameAs<O, OptionPair>) {
                    String8 key = o.first;
                    if (is_obsolete_table_option(key)) {
                        err = Result<TableOptionsParsed>{{}, Error::InvalidOptions, "Unknown property"};
                    } else if (key == "default_time_to_live") {
                        if (auto vv = s64_seconds_from_option_value(o.second)) {
                            out.options.default_ttl_ms = (*vv) * 1000;
                        }
                    } else if (key == "gc_grace_seconds") {
                        if (auto vv = s32_from_option_value(o.second)) {
                            out.options.gc_grace_seconds = *vv;
                        }
                    } else if (key == "min_index_interval") {
                        if (auto vv = s32_from_option_value(o.second)) {
                            out.options.min_index_interval = *vv;
                        }
                    } else if (key == "max_index_interval") {
                        if (auto vv = s32_from_option_value(o.second)) {
                            out.options.max_index_interval = *vv;
                        }
                    } else if (key == "memtable_flush_period_in_ms") {
                        if (auto vv = s32_from_option_value(o.second)) {
                            out.options.memtable_flush_period_in_ms = *vv;
                        }
                    } else if (key == "read_repair" || key == "speculative_retry" || key == "additional_write_policy") {
                        log::schema_info("ignoring multi-node table option (single-node no-op)");
                    } else if (key == "bloom_filter_fp_chance" || key == "caching" || key == "compaction" || key == "compression" || key == "crc_check_chance" || key == "extensions" || key == "comment") {
                        log::schema_info("table option not yet implemented, ignoring");
                    }
                } else {
                    static_assert(!SameAs<O, O>, "unhandled table option variant");
                }
            });
            if (err.error != Error::None) {
                return err;
            }
        }
        return {out};
    }
}

// ============================================================================
// keyspace DDL
// ============================================================================
namespace cql::schema {
    coroutine::Task<Result<Keyspace*>> create_keyspace(Schema& schema, const CreateKeyspace& create) {
        bump_version(schema);
        assert_true(read_keyspace(schema, create.name).error == Error::MissingKeyspace, "keyspace already exists");

        auto popts = parse_keyspace_options(create.options);
        if (popts.error != Error::None) {
            co_return Result<Keyspace*>{nullptr, popts.error, popts.message};
        }

        U64             offset_bytes = schema.keyspaces_blob.size_bytes;
        KeyspaceStorage ks_storage{};
        ks_storage.offset_in_blob_bytes      = offset_bytes;
        ks_storage.header.tombstone          = 0;
        ks_storage.header.replication_class  = popts.value.replication_class;
        ks_storage.header.do_durable_writes  = popts.value.do_durable_writes ? 1 : 0;
        ks_storage.header.replication_factor = popts.value.replication_factor;
        ks_storage.header.name_length        = create.name.length;
        ks_storage.name                      = AutoString8(create.name);

        KeyspaceStorage& ks_ref = push_back(schema.storage.keyspaces, move(ks_storage));

        co_await blob::resize(schema.keyspaces_blob, offset_bytes + sizeof(KeyspaceHeader) + ks_ref.name.length);
        co_await blob::tupdate(schema.keyspaces_blob, &ks_ref.header, &offset_bytes);
        co_await put_bytes(schema.keyspaces_blob, String8{ks_ref.name.c_str, ks_ref.name.length}, &offset_bytes);

        Keyspace ks{};
        ks.idx                = schema.storage.keyspaces.length - 1;
        ks.tombstone          = false;
        ks.name               = String8{ks_ref.name.c_str, ks_ref.name.length};
        ks.replication_class  = popts.value.replication_class;
        ks.replication_factor = popts.value.replication_factor;
        ks.do_durable_writes  = popts.value.do_durable_writes;
        Keyspace& ks_runtime  = push_back(schema.keyspaces, move(ks));
        insert(schema.keyspaces_by_name, ks_runtime.name, schema.keyspaces.length - 1);
        co_return Result<Keyspace*>{&ks_runtime};
    }

    coroutine::Task<Result<void>> alter_keyspace(Schema& schema, Keyspace& ks, const Options& opts) {
        bump_version(schema);
        auto popts = parse_keyspace_options(opts);
        if (popts.error != Error::None) {
            co_return Result<void>{popts.error, popts.message};
        }
        KeyspaceStorage& ks_storage          = schema.storage.keyspaces[ks.idx];
        ks_storage.header.replication_class  = popts.value.replication_class;
        ks_storage.header.replication_factor = popts.value.replication_factor;
        ks_storage.header.do_durable_writes  = popts.value.do_durable_writes ? 1 : 0;
        ks.replication_class                 = popts.value.replication_class;
        ks.replication_factor                = popts.value.replication_factor;
        ks.do_durable_writes                 = popts.value.do_durable_writes;
        U64 off                              = ks_storage.offset_in_blob_bytes;
        co_await blob::tupdate(schema.keyspaces_blob, &ks_storage.header, &off);
        co_return Result<void>{};
    }

    coroutine::Task<Result<void>> delete_keyspace(Schema& schema, String8 name) {
        bump_version(schema);
        auto ks_res = read_keyspace(schema, name);
        if (ks_res.error != Error::None) {
            co_return Result<void>{ks_res.error, ks_res.message};
        }
        Keyspace* ks  = ks_res.value;
        ks->tombstone = true;
        remove(schema.keyspaces_by_name, ks->name);
        KeyspaceStorage& storage = schema.storage.keyspaces[ks->idx];
        storage.header.tombstone = 1;
        U64 off                  = storage.offset_in_blob_bytes + offsetof(KeyspaceHeader, tombstone);
        co_await blob::update(schema.keyspaces_blob, &storage.header.tombstone, sizeof(storage.header.tombstone), off);
        co_return Result<void>{};
    }
}

// ============================================================================
// table DDL
// ============================================================================
namespace cql::schema {
    static bool ast_type_is_counter(const type::ast::Type& t) {
        return type_matches_tag<type::Basic>(t.value) && get<type::Basic>(t.value) == type::Basic::counter;
    }

    static bool ast_type_contains_counter(const type::ast::Type& t) {
        return visit(t.value, [&](const auto& v) -> bool {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                return v == type::Basic::counter;
            } else if constexpr (SameAs<T, type::ast::ListAst>) {
                return ast_type_contains_counter(v.element);
            } else if constexpr (SameAs<T, type::ast::SetAst>) {
                return ast_type_contains_counter(v.key);
            } else if constexpr (SameAs<T, type::ast::MapAst>) {
                return ast_type_contains_counter(v.key) || ast_type_contains_counter(v.value);
            } else if constexpr (SameAs<T, type::ast::VectorAst>) {
                return ast_type_contains_counter(v.element);
            } else if constexpr (SameAs<T, type::ast::TupleAst>) {
                for (const auto& e : v.elements) {
                    if (ast_type_contains_counter(e)) {
                        return true;
                    }
                }
                return false;
            } else {
                static_assert(SameAs<T, type::ast::UdtRef>, "unhandled ast::Type variant in ast_type_contains_counter");
                return false;
            }
        });
    }

    struct PrimaryKeyInfo {
        DynamicArray<U64> partition_col_def_indices;
        DynamicArray<U64> clustering_col_def_indices;
    };

    static Optional<PrimaryKeyInfo> get_primary_key_info(const CreateTable& create) {
        PrimaryKeyInfo info{};
        for (U64 i = 0; i < create.column_definitions.length; i++) {
            if (create.column_definitions[i].primary_key) {
                push_back(info.partition_col_def_indices, i);
                return info;
            }
        }
        if (create.primary_key) {
            const auto& pk               = *create.primary_key;
            auto        find_col_def_idx = [&](String8 name) -> Optional<U64> {
                for (U64 i = 0; i < create.column_definitions.length; i++) {
                    if (create.column_definitions[i].name.identifier == name) {
                        return i;
                    }
                }
                return {};
            };
            if (type_matches_tag<ColumnName>(pk.partition_key.column_or_columns)) {
                String8 nm = get<ColumnName>(pk.partition_key.column_or_columns).identifier;
                if (auto i = find_col_def_idx(nm)) {
                    push_back(info.partition_col_def_indices, *i);
                }
            } else {
                for (const auto& cn : get<DynamicArray<ColumnName>>(pk.partition_key.column_or_columns)) {
                    if (auto i = find_col_def_idx(cn.identifier)) {
                        push_back(info.partition_col_def_indices, *i);
                    }
                }
            }
            for (const auto& cn : pk.clustering_columns) {
                if (auto i = find_col_def_idx(cn.identifier)) {
                    push_back(info.clustering_col_def_indices, *i);
                }
            }
        }
        if (info.partition_col_def_indices.length == 0) {
            return {};
        }
        return info;
    }

    coroutine::Task<Result<Table*>> create_table(Schema& schema, Keyspace& ks, const CreateTable& create) {
        bump_version(schema);
        assert_true(read_table(schema, ks, create.name.table_name).error == Error::MissingTable, "table already exists");

        // sanity-check primary key declarations
        U64 inline_pk_count = 0;
        for (U64 i = 0; i < create.column_definitions.length; i++) {
            if (create.column_definitions[i].primary_key) {
                inline_pk_count++;
            }
        }
        if (inline_pk_count > 1 || (inline_pk_count == 1 && create.primary_key)) {
            co_return Result<Table*>{nullptr, Error::InvalidOptions, "Multiple PRIMARY KEYs specified"};
        }

        auto pk_info_opt = get_primary_key_info(create);
        if (!pk_info_opt) {
            co_return Result<Table*>{nullptr, Error::MissingPrimaryKey, "table needs a primary key"};
        }
        PrimaryKeyInfo& pk_info = *pk_info_opt;

        auto is_pk_def = [&](U64 i) -> bool {
            for (U64 x : pk_info.partition_col_def_indices) {
                if (x == i) {
                    return true;
                }
            }
            for (U64 x : pk_info.clustering_col_def_indices) {
                if (x == i) {
                    return true;
                }
            }
            return false;
        };

        // counter constraints
        for (U64 i : pk_info.partition_col_def_indices) {
            if (ast_type_is_counter(create.column_definitions[i].type)) {
                co_return Result<Table*>{nullptr, Error::InvalidOptions, "counter type is not supported for PRIMARY KEY"};
            }
        }
        for (U64 i : pk_info.clustering_col_def_indices) {
            if (ast_type_is_counter(create.column_definitions[i].type)) {
                co_return Result<Table*>{nullptr, Error::InvalidOptions, "counter type is not supported for PRIMARY KEY"};
            }
        }
        for (const auto& col : create.column_definitions) {
            if (!ast_type_is_counter(col.type) && ast_type_contains_counter(col.type)) {
                co_return Result<Table*>{nullptr, Error::InvalidOptions, "counter type is not allowed inside collections"};
            }
        }
        bool any_counter = false, any_non_counter = false;
        for (U64 i = 0; i < create.column_definitions.length; i++) {
            if (is_pk_def(i)) {
                continue;
            }
            if (ast_type_is_counter(create.column_definitions[i].type)) {
                any_counter = true;
            } else {
                any_non_counter = true;
            }
        }
        if (any_counter && any_non_counter) {
            co_return Result<Table*>{nullptr, Error::InvalidOptions, "Cannot mix counter and non counter columns in the same table"};
        }

        // resolve clustering order directives
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
                    for (U64 i = 0; i < o.column_orders.length; i++) {
                        if (i >= ck_orders.length) {
                            order_err = Error::InvalidOptions;
                            return;
                        }
                        U64     ck_def_idx = pk_info.clustering_col_def_indices[i];
                        String8 ck_name(create.column_definitions[ck_def_idx].name.identifier.c_str, create.column_definitions[ck_def_idx].name.identifier.length);
                        String8 dir_name(o.column_orders[i].column.identifier.c_str, o.column_orders[i].column.identifier.length);
                        if (ck_name != dir_name) {
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

        auto popts = parse_table_options(create.options);
        if (popts.error != Error::None) {
            co_return Result<Table*>{nullptr, popts.error, popts.message};
        }

        U64 btree_page = co_await btree::create_paged(
            *schema.tables_blob.pager,
            btree::FixedKeyPolicy<S64>{}, btree::FixedValuePolicy<sizeof(PartitionEntry)>{}
        );

        U64          off = schema.tables_blob.size_bytes;
        TableStorage ts{};
        ts.offset_in_blob_bytes = off;
        ts.header.tombstone     = 0;
        ts.header.keyspace_idx  = ks.idx;
        ts.header.btree_page    = btree_page;
        ts.header.options       = popts.value.options;
        ts.header.name_length   = create.name.table_name.length;
        ts.name                 = AutoString8(create.name.table_name);
        TableStorage& ts_ref    = push_back(schema.storage.tables, move(ts));

        co_await blob::resize(schema.tables_blob, off + sizeof(TableHeader) + ts_ref.name.length);
        co_await blob::tupdate(schema.tables_blob, &ts_ref.header, &off);
        co_await put_bytes(schema.tables_blob, String8{ts_ref.name.c_str, ts_ref.name.length}, &off);

        push_back(schema.storage.keyspaces[ks.idx].tables, schema.storage.tables.length - 1);

        Table tbl{};
        tbl.idx       = schema.storage.tables.length - 1;
        tbl.tombstone = false;
        tbl.name      = String8{ts_ref.name.c_str, ts_ref.name.length};
        tbl.options   = popts.value.options;
        tbl.btree     = PartitionBTree{
            schema.tables_blob.pager, btree_page,
            btree::FixedKeyPolicy<S64>{}, btree::FixedValuePolicy<sizeof(PartitionEntry)>{}
        };
        Table& tbl_ref = push_back(ks.tbls, move(tbl));
        insert(ks.tbls_by_name, tbl_ref.name, ks.tbls.length - 1);

        // create columns (resolving types against the parent keyspace)
        for (U64 ci = 0; ci < create.column_definitions.length; ci++) {
            const auto& col_def = create.column_definitions[ci];

            if (read_column(schema, tbl_ref, col_def.name.identifier).error != Error::MissingColumn) {
                co_await delete_table(schema, ks, create.name.table_name);
                co_return Result<Table*>{nullptr, Error::ColumnNameCollision};
            }
            auto resolved = resolve_type_ast(schema, ks.name, col_def.type);
            if (resolved.error != Error::None) {
                co_await delete_table(schema, ks, create.name.table_name);
                co_return Result<Table*>{nullptr, resolved.error, resolved.message};
            }

            KeyKind kk = KeyKind::None;
            U16     kp = 0;
            Sort    so = Sort::ASC;
            for (U64 i = 0; i < pk_info.partition_col_def_indices.length; i++) {
                if (pk_info.partition_col_def_indices[i] == ci) {
                    kk = KeyKind::PartitionKey;
                    kp = static_cast<U16>(i);
                    break;
                }
            }
            if (kk == KeyKind::None) {
                for (U64 i = 0; i < pk_info.clustering_col_def_indices.length; i++) {
                    if (pk_info.clustering_col_def_indices[i] == ci) {
                        kk = KeyKind::ClusteringKey;
                        kp = static_cast<U16>(i);
                        so = ck_orders[i];
                        break;
                    }
                }
            }
            assert_true_not_implemented(!col_def.mask, "column MASKED WITH is not implemented");
            if (col_def._static) {
                if (tbl_ref.clustering_key_col_indices.length == 0 && pk_info.clustering_col_def_indices.length == 0) {
                    co_await delete_table(schema, ks, create.name.table_name);
                    co_return Result<Table*>{nullptr, Error::InvalidOptions, "static columns require at least one clustering column"};
                }
                if (kk != KeyKind::None) {
                    co_await delete_table(schema, ks, create.name.table_name);
                    co_return Result<Table*>{nullptr, Error::InvalidOptions, "primary key columns cannot be static"};
                }
            }
            auto res = co_await create_column(schema, tbl_ref, String8{col_def.name.identifier.c_str, col_def.name.identifier.length}, move(resolved.value), col_def._static, kk, kp, so);
            if (res.error != Error::None) {
                co_await delete_table(schema, ks, create.name.table_name);
                co_return Result<Table*>{nullptr, res.error, res.message};
            }
        }
        rebuild_table_indices(tbl_ref);
        co_return Result<Table*>{&tbl_ref};
    }

    coroutine::Task<Result<void>> set_table_options(Schema& schema, Table& tbl, const TableOptions& options) {
        TableStorage& ts  = schema.storage.tables[tbl.idx];
        ts.header.options = options;
        tbl.options       = options;
        U64 off           = ts.offset_in_blob_bytes + offsetof(TableHeader, options);
        co_await blob::update(schema.tables_blob, reinterpret_cast<const U8*>(&ts.header.options), sizeof(ts.header.options), off);
        co_return Result<void>{};
    }

    coroutine::Task<Result<void>> delete_table(Schema& schema, Keyspace& ks, String8 name) {
        bump_version(schema);
        auto tbl_res = read_table(schema, ks, name);
        if (tbl_res.error != Error::None) {
            co_return Result<void>{tbl_res.error, tbl_res.message};
        }
        Table* tbl     = tbl_res.value;
        tbl->tombstone = true;
        remove(ks.tbls_by_name, tbl->name);

        TableStorage& ts    = schema.storage.tables[tbl->idx];
        ts.header.tombstone = 1;
        U64 off             = ts.offset_in_blob_bytes + offsetof(TableHeader, tombstone);
        co_await blob::update(schema.tables_blob, &ts.header.tombstone, sizeof(ts.header.tombstone), off);
        co_return Result<void>{};
    }
}

// ============================================================================
// column DDL
// ============================================================================
namespace cql::schema {
    coroutine::Task<Result<Column*>> create_column(Schema& schema, Table& tbl, String8 name, type::Type type, bool is_static, KeyKind key_kind, U16 key_position, Sort clustering_order) {
        bump_version(schema);
        assert_true(read_column(schema, tbl, name).error == Error::MissingColumn, "column already exists");

        U64           off = schema.columns_blob.size_bytes;
        ColumnStorage cs{};
        cs.offset_in_blob_bytes    = off;
        cs.header.tombstone        = 0;
        cs.header.is_static        = is_static ? 1 : 0;
        cs.header.key_kind         = key_kind;
        cs.header.clustering_order = clustering_order;
        cs.header.key_position     = key_position;
        cs.header.table_idx        = tbl.idx;
        cs.header.name_length      = name.length;
        cs.name                    = AutoString8(name);
        cs.type                    = move(type);
        ColumnStorage& cs_ref      = push_back(schema.storage.columns, move(cs));

        U64 type_size = serialized_type_size(cs_ref.type);
        co_await blob::resize(schema.columns_blob, off + sizeof(ColumnHeader) + cs_ref.name.length + type_size);
        co_await blob::tupdate(schema.columns_blob, &cs_ref.header, &off);
        co_await put_bytes(schema.columns_blob, String8{cs_ref.name.c_str, cs_ref.name.length}, &off);
        co_await write_type(schema.columns_blob, cs_ref.type, &off);

        push_back(schema.storage.tables[tbl.idx].columns, schema.storage.columns.length - 1);

        Column col{};
        col.tombstone        = false;
        col.is_static        = is_static;
        col.name             = String8{cs_ref.name.c_str, cs_ref.name.length};
        col.type             = cs_ref.type;
        col.key_kind         = key_kind;
        col.key_position     = key_position;
        col.clustering_order = clustering_order;
        Column& col_ref      = push_back(tbl.cols, move(col));
        rebuild_table_indices(tbl);
        co_return Result<Column*>{&col_ref};
    }

    coroutine::Task<Result<void>> delete_column(Schema& schema, Table& tbl, String8 name) {
        bump_version(schema);
        auto col_res = read_column(schema, tbl, name);
        if (col_res.error != Error::None) {
            co_return Result<void>{col_res.error, col_res.message};
        }
        Column* col    = col_res.value;
        col->tombstone = true;

        for (auto& cs : schema.storage.columns) {
            if (!cs.header.tombstone && cs.header.table_idx == tbl.idx
                && String8(cs.name.c_str, cs.name.length) == name) {
                cs.header.tombstone = 1;
                U64 off             = cs.offset_in_blob_bytes + offsetof(ColumnHeader, tombstone);
                co_await blob::update(schema.columns_blob, &cs.header.tombstone, sizeof(cs.header.tombstone), off);
                break;
            }
        }
        rebuild_table_indices(tbl);
        co_return Result<void>{};
    }
}

// ============================================================================
// index DDL
// ============================================================================
namespace cql::schema {
    coroutine::Task<Result<Index*>> create_index(Schema& schema, Table& tbl, U64 col_idx, String8 index_name, IndexKind kind) {
        bump_version(schema);
        DynamicArray<clustering_compare::ClusteringColumnSpec> key_specs      = make_index_key_specs(tbl, col_idx, kind);
        auto                                                   key_specs_view = TArrayView<const clustering_compare::ClusteringColumnSpec, U64>(key_specs.ptr, key_specs.length);
        U64                                                    btree_page     = co_await btree::create_paged(
            *schema.indexes_blob.pager,
            ClusteringKeyPolicy{clustering_compare::ClusteringKeyComparator{key_specs_view}}, btree::FixedValuePolicy<sizeof(IndexEntry)>{}
        );

        U64          off = schema.indexes_blob.size_bytes;
        IndexStorage is{};
        is.offset_in_blob_bytes = off;
        is.header.tombstone     = 0;
        is.header.kind          = kind;
        is.header.table_idx     = tbl.idx;
        is.header.col_idx       = col_idx;
        is.header.btree_page    = btree_page;
        is.header.name_length   = index_name.length;
        is.name                 = AutoString8(index_name);
        IndexStorage& is_ref    = push_back(schema.storage.indexes, move(is));

        co_await blob::resize(schema.indexes_blob, off + sizeof(IndexHeader) + is_ref.name.length);
        co_await blob::tupdate(schema.indexes_blob, &is_ref.header, &off);
        co_await put_bytes(schema.indexes_blob, String8{is_ref.name.c_str, is_ref.name.length}, &off);

        Index idx{};
        idx.idx       = schema.storage.indexes.length - 1;
        idx.tombstone = false;
        idx.name      = String8{is_ref.name.c_str, is_ref.name.length};
        idx.col_idx   = col_idx;
        idx.kind      = kind;
        idx.key_specs = move(key_specs);
        idx.btree     = IndexBTree{
            schema.indexes_blob.pager, btree_page,
            make_index_key_policy(idx), btree::FixedValuePolicy<sizeof(IndexEntry)>{}
        };
        Index& idx_ref = push_back(tbl.indexes, move(idx));
        insert(tbl.indexes_by_name, idx_ref.name, tbl.indexes.length - 1);
        co_return Result<Index*>{&idx_ref};
    }

    coroutine::Task<Result<void>> delete_index(Schema& schema, Table& tbl, String8 name) {
        bump_version(schema);
        auto idx_res = read_index(schema, tbl, name);
        if (idx_res.error != Error::None) {
            co_return Result<void>{idx_res.error, idx_res.message};
        }
        Index* idx     = idx_res.value;
        idx->tombstone = true;
        remove(tbl.indexes_by_name, idx->name);
        IndexStorage& is    = schema.storage.indexes[idx->idx];
        is.header.tombstone = 1;
        U64 off             = is.offset_in_blob_bytes + offsetof(IndexHeader, tombstone);
        co_await blob::update(schema.indexes_blob, &is.header.tombstone, sizeof(is.header.tombstone), off);
        co_return Result<void>{};
    }
}

// ============================================================================
// UDT DDL
// ============================================================================
namespace cql::schema {
    // @note storage.udts is append-only, so the storage index equals the record index in udts_blob.
    static U64 udt_record_idx_for(const Schema& schema, U64 udt_storage_idx) {
        assert_true(udt_storage_idx < schema.storage.udts.length, "invalid udt storage idx");
        return udt_storage_idx;
    }

    static U64 find_udt_storage_idx(const Schema& schema, String8 ks_name, String8 name) {
        for (U64 i = 0; i < schema.storage.udts.length; i++) {
            const auto& s = schema.storage.udts[i];
            if (s.header.tombstone != 0) {
                continue;
            }
            if (String8(s.keyspace.c_str, s.keyspace.length) != ks_name) {
                continue;
            }
            if (String8(s.name.c_str, s.name.length) != name) {
                continue;
            }
            return i;
        }
        return U64{} - 1;
    }

    static coroutine::Task<void> append_udt_decl_record(Schema& schema, UdtStorage& udt) {
        U64 off                  = schema.udts_blob.size_bytes;
        udt.offset_in_blob_bytes = off;
        co_await blob::resize(schema.udts_blob, off + sizeof(UdtRecordHeader) + udt.keyspace.length + udt.name.length);
        co_await blob::tupdate(schema.udts_blob, &udt.header, &off);
        co_await put_bytes(schema.udts_blob, String8{udt.keyspace.c_str, udt.keyspace.length}, &off);
        co_await put_bytes(schema.udts_blob, String8{udt.name.c_str, udt.name.length}, &off);
    }

    static coroutine::Task<U64> append_udt_field_record(Schema& schema, U64 parent_udt_record_idx, String8 field_name, const type::Type& field_type) {
        UdtRecordHeader h{};
        h.tombstone             = 0;
        h.is_field              = 1;
        h.parent_udt_record_idx = parent_udt_record_idx;
        h.keyspace_length       = 0;
        h.name_length           = field_name.length;
        U64 off                 = schema.udts_blob.size_bytes;
        U64 type_size           = serialized_type_size(field_type);
        co_await blob::resize(schema.udts_blob, off + sizeof(UdtRecordHeader) + field_name.length + type_size);
        co_await blob::tupdate(schema.udts_blob, &h, &off);
        co_await put_bytes(schema.udts_blob, field_name, &off);
        co_await write_type(schema.udts_blob, field_type, &off);
        co_return schema.udts_blob.size_bytes - sizeof(UdtRecordHeader) - field_name.length - type_size;
    }

    static coroutine::Task<void> tombstone_record(Schema& schema, U64 record_offset) {
        U8  one = 1;
        U64 off = record_offset + offsetof(UdtRecordHeader, tombstone);
        co_await blob::update(schema.udts_blob, &one, sizeof(one), off);
    }

    static void rebind_udt_views(UdtStorage& storage, type::UDT* du) {
        du->keyspace = String8{storage.keyspace.c_str, storage.keyspace.length};
        du->name     = String8{storage.name.c_str, storage.name.length};
        clear(du->field_names);
        clear(du->field_types);
        for (U64 i = 0; i < storage.field_names.length; i++) {
            push_back(du->field_names, String8{storage.field_names[i].c_str, storage.field_names[i].length});
            push_back(du->field_types, storage.field_types[i]);
        }
    }

    coroutine::Task<Result<type::UDT*>> create_udt(Schema& schema, Keyspace& ks, String8 name, DynamicArray<AutoString8>&& field_names, DynamicArray<type::Type>&& field_types) {
        bump_version(schema);
        if (read_udt(schema, ks, name).error == Error::None) {
            co_return Result<type::UDT*>{nullptr, Error::InvalidOptions, "type already exists"};
        }
        if (field_names.length != field_types.length || field_names.length == 0) {
            co_return Result<type::UDT*>{nullptr, Error::InvalidOptions, "user-defined type must have at least one field"};
        }

        // append UDT-decl record
        UdtStorage udt{};
        udt.header.tombstone             = 0;
        udt.header.is_field              = 0;
        udt.header.parent_udt_record_idx = 0;
        udt.header.keyspace_length       = ks.name.length;
        udt.header.name_length           = name.length;
        udt.keyspace                     = AutoString8(ks.name);
        udt.name                         = AutoString8(name);

        U64         new_storage_idx = schema.storage.udts.length;
        UdtStorage& ref             = push_back(schema.storage.udts, move(udt));
        co_await append_udt_decl_record(schema, ref);

        // bind in deque + map
        type::UDT* du = &emplace_back(schema.udts, type::UDT{})->value;
        du->keyspace  = String8{ref.keyspace.c_str, ref.keyspace.length};
        du->name      = String8{ref.name.c_str, ref.name.length};
        insert(ks.udts_by_name, du->name, du);

        // append fields
        U64 record_idx = udt_record_idx_for(schema, new_storage_idx);
        for (U64 i = 0; i < field_names.length; i++) {
            U64 field_off = co_await append_udt_field_record(schema, record_idx, String8{field_names[i].c_str, field_names[i].length}, field_types[i]);
            push_back(ref.field_record_offsets, field_off);
            push_back(ref.field_names, move(field_names[i]));
            push_back(ref.field_types, move(field_types[i]));
        }
        rebind_udt_views(ref, du);
        co_return Result<type::UDT*>{du};
    }

    coroutine::Task<Result<void>> alter_udt_add_field(Schema& schema, Keyspace& ks, String8 name, AutoString8 field_name, type::Type field_type) {
        bump_version(schema);
        auto udt_res = read_udt(schema, ks, name);
        if (udt_res.error != Error::None) {
            co_return Result<void>{Error::MissingType, "user-defined type does not exist"};
        }

        // forbid ADD when the UDT is used by a partition or clustering key
        for (const auto& tbl : ks.tbls) {
            if (tbl.tombstone) {
                continue;
            }
            for (const auto& col : tbl.cols) {
                if (col.tombstone || col.key_kind == KeyKind::None) {
                    continue;
                }
                bool refs = false;
                auto walk = [&](const auto& self, const type::Type& t) -> void {
                    visit(t.value, [&](const auto& v) {
                        using T = RemoveCVRef<decltype(v)>;
                        if constexpr (SameAs<T, type::Basic>) {
                            // skip
                        } else if constexpr (SameAs<T, type::List> || SameAs<T, type::Vector>) {
                            self(self, v.element);
                        } else if constexpr (SameAs<T, type::Set>) {
                            self(self, v.key);
                        } else if constexpr (SameAs<T, type::Map>) {
                            self(self, v.key);
                            self(self, v.value);
                        } else if constexpr (SameAs<T, type::Tuple>) {
                            for (const auto& e : v.elements) {
                                self(self, e);
                            }
                        } else {
                            static_assert(SameAs<T, type::UDT*>, "unhandled Type variant in alter ADD UDT walk");
                            if (v == udt_res.value) {
                                refs = true;
                            } else if (v != nullptr) {
                                for (const auto& ft : v->field_types) {
                                    self(self, ft);
                                }
                            }
                        }
                    });
                };
                walk(walk, col.type);
                if (refs) {
                    co_return Result<void>{Error::InvalidOptions, "Cannot add new field to a user type used in a primary key"};
                }
            }
        }

        U64 storage_idx = find_udt_storage_idx(schema, ks.name, name);
        assert_true(storage_idx != U64{} - 1, "alter ADD: UDT storage missing");
        UdtStorage& ref     = schema.storage.udts[storage_idx];
        U64         rec_idx = udt_record_idx_for(schema, storage_idx);

        U64 field_off = co_await append_udt_field_record(schema, rec_idx, String8{field_name.c_str, field_name.length}, field_type);
        push_back(ref.field_record_offsets, field_off);
        push_back(ref.field_names, move(field_name));
        push_back(ref.field_types, move(field_type));
        rebind_udt_views(ref, udt_res.value);
        co_return Result<void>{};
    }

    coroutine::Task<Result<void>> alter_udt_rename_fields(Schema& schema, Keyspace& ks, String8 name, const DynamicArray<Pair<ColumnName, ColumnName>>& renames) {
        bump_version(schema);
        auto udt_res = read_udt(schema, ks, name);
        if (udt_res.error != Error::None) {
            co_return Result<void>{Error::MissingType, "user-defined type does not exist"};
        }
        U64 storage_idx = find_udt_storage_idx(schema, ks.name, name);
        assert_true(storage_idx != U64{} - 1, "alter RENAME: UDT storage missing");
        UdtStorage& ref = schema.storage.udts[storage_idx];

        // apply renames in memory and to storage's owning AutoString8 buffers
        for (U64 i = 0; i < ref.field_names.length; i++) {
            String8 cur{ref.field_names[i].c_str, ref.field_names[i].length};
            for (const auto& [old_n, new_n] : renames) {
                if (cur == String8{old_n.identifier.c_str, old_n.identifier.length}) {
                    ref.field_names[i] = AutoString8(new_n.identifier);
                    break;
                }
            }
        }

        // tombstone all existing field records, then append fresh ones in current order
        for (U64 fo : ref.field_record_offsets) {
            co_await tombstone_record(schema, fo);
        }
        clear(ref.field_record_offsets);
        U64 rec_idx = udt_record_idx_for(schema, storage_idx);
        for (U64 i = 0; i < ref.field_names.length; i++) {
            U64 new_off = co_await append_udt_field_record(schema, rec_idx, String8{ref.field_names[i].c_str, ref.field_names[i].length}, ref.field_types[i]);
            push_back(ref.field_record_offsets, new_off);
        }
        rebind_udt_views(ref, udt_res.value);
        co_return Result<void>{};
    }

    coroutine::Task<Result<void>> delete_udt(Schema& schema, Keyspace& ks, String8 name) {
        bump_version(schema);
        auto udt_res = read_udt(schema, ks, name);
        if (udt_res.error != Error::None) {
            co_return Result<void>{Error::MissingType, "user-defined type does not exist"};
        }
        auto deps = find_udt_dependents(schema, ks, name);
        if (deps.length > 0) {
            co_return Result<void>{Error::TypeInUse, "Cannot drop user type currently in use"};
        }
        U64 storage_idx = find_udt_storage_idx(schema, ks.name, name);
        assert_true(storage_idx != U64{} - 1, "delete: UDT storage missing");
        UdtStorage& ref = schema.storage.udts[storage_idx];

        // tombstone UDT-decl record + all field records
        co_await tombstone_record(schema, ref.offset_in_blob_bytes);
        for (U64 fo : ref.field_record_offsets) {
            co_await tombstone_record(schema, fo);
        }
        ref.header.tombstone = 1;
        remove(ks.udts_by_name, name);
        co_return Result<void>{};
    }

    // ========================================================================
    // dependent enumeration
    // ========================================================================
    static bool type_references_udt(const type::Type& t, type::UDT* target) {
        return visit(t.value, [&](const auto& v) -> bool {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                return false;
            } else if constexpr (SameAs<T, type::List> || SameAs<T, type::Vector>) {
                return type_references_udt(v.element, target);
            } else if constexpr (SameAs<T, type::Set>) {
                return type_references_udt(v.key, target);
            } else if constexpr (SameAs<T, type::Map>) {
                return type_references_udt(v.key, target) || type_references_udt(v.value, target);
            } else if constexpr (SameAs<T, type::Tuple>) {
                for (const auto& e : v.elements) {
                    if (type_references_udt(e, target)) {
                        return true;
                    }
                }
                return false;
            } else {
                static_assert(SameAs<T, type::UDT*>, "unhandled Type variant in type_references_udt");
                if (v == target) {
                    return true;
                }
                if (v == nullptr) {
                    return false;
                }
                for (const auto& ft : v->field_types) {
                    if (type_references_udt(ft, target)) {
                        return true;
                    }
                }
                return false;
            }
        });
    }

    DynamicArray<UdtDependent> find_udt_dependents([[maybe_unused]] Schema& schema, const Keyspace& ks, String8 name) {
        DynamicArray<UdtDependent> out;
        type::UDT* const*          target_p = find(ks.udts_by_name, name);
        if (target_p == nullptr) {
            return out;
        }
        type::UDT* target = *target_p;
        for (const auto& tbl : ks.tbls) {
            if (tbl.tombstone) {
                continue;
            }
            for (const auto& col : tbl.cols) {
                if (col.tombstone) {
                    continue;
                }
                if (type_references_udt(col.type, target)) {
                    UdtDependent dep{};
                    dep.description = AutoString8(tbl.name) + "." + AutoString8(col.name);
                    push_back(out, move(dep));
                }
            }
        }
        // other UDTs in same keyspace
        for (auto it = ks.udts_by_name.begin(); it != ks.udts_by_name.end(); ++it) {
            type::UDT* udt = (*it).second;
            if (udt == target) {
                continue;
            }
            for (const auto& ft : udt->field_types) {
                if (type_references_udt(ft, target)) {
                    UdtDependent dep{};
                    dep.description = "type "_as + AutoString8(udt->name);
                    push_back(out, move(dep));
                    break;
                }
            }
        }
        return out;
    }
}
