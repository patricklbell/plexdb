module cql.engine.system_schema;

import cql.engine.column_value;
import cql.engine.schema;
import cql.engine.types;
import cql.engine.evaluator;
import cql.engine.statements;

using namespace plexdb;

namespace cql::engine {
    // Deterministic per-table UUID: high 8 bytes = hash of keyspace name,
    // low 8 bytes = hash of table name. Good enough as a stable fake ID.
    static UUID table_uuid(String8 ks, String8 tbl) {
        U64 h1 = 0xcbf29ce484222325ULL;
        for (U64 i = 0; i < ks.length; i++) {
            h1 = (h1 ^ (U8)ks.data[i]) * 0x100000001b3ULL;
        }
        U64 h2 = 0xcbf29ce484222325ULL;
        for (U64 i = 0; i < tbl.length; i++) {
            h2 = (h2 ^ (U8)tbl.data[i]) * 0x100000001b3ULL;
        }
        Array<U8, 16> uuid{};
        for (int i = 0; i < 8; i++) {
            uuid[i] = (U8)(h1 >> (i * 8));
        }
        for (int i = 0; i < 8; i++) {
            uuid[8 + i] = (U8)(h2 >> (i * 8));
        }
        // RFC 4122 version 4 / variant bits
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        return {uuid};
    }

    static NestedColumnValue ncv(AutoString8 s) {
        return NestedColumnValue{ColumnValue{move(s)}};
    }

    static DynamicMap<NestedColumnValue, NestedColumnValue> default_caching() {
        DynamicMap<NestedColumnValue, NestedColumnValue> m{};
        insert(m, ncv("keys"_as), ncv("ALL"_as));
        insert(m, ncv("rows_per_partition"_as), ncv("NONE"_as));
        return m;
    }
    static DynamicMap<NestedColumnValue, NestedColumnValue> default_compaction() {
        DynamicMap<NestedColumnValue, NestedColumnValue> m{};
        insert(m, ncv("class"_as), ncv("org.apache.cassandra.db.compaction.SizeTieredCompactionStrategy"_as));
        return m;
    }
    static DynamicMap<NestedColumnValue, NestedColumnValue> default_compression() {
        DynamicMap<NestedColumnValue, NestedColumnValue> m{};
        insert(m, ncv("chunk_length_in_kb"_as), ncv("64"_as));
        insert(m, ncv("class"_as), ncv("org.apache.cassandra.io.compress.LZ4Compressor"_as));
        return m;
    }

    // ---- Built-in system virtual table definitions -------------------------
    struct BuiltinCol {
        const char* name;
        type::Type  type;
        const char* kind; // "partition_key", "clustering", or "regular"
        S32         pos;
    };
    struct BuiltinTbl {
        const char*       ks;
        const char*       tbl;
        const BuiltinCol* cols;
        U64               ncols;
    };

    static const BuiltinCol sys_local_cols[] = {
        {"key", type::create_basic(type::Basic::text), "partition_key", 0},
        {"bootstrapped", type::create_basic(type::Basic::text), "regular", -1},
        {"broadcast_address", type::create_basic(type::Basic::inet), "regular", -1},
        {"broadcast_port", type::create_basic(type::Basic::int_), "regular", -1},
        {"cluster_name", type::create_basic(type::Basic::text), "regular", -1},
        {"cql_version", type::create_basic(type::Basic::text), "regular", -1},
        {"data_center", type::create_basic(type::Basic::text), "regular", -1},
        {"host_id", type::create_basic(type::Basic::uuid), "regular", -1},
        {"listen_address", type::create_basic(type::Basic::inet), "regular", -1},
        {"listen_port", type::create_basic(type::Basic::int_), "regular", -1},
        {"native_protocol_version", type::create_basic(type::Basic::text), "regular", -1},
        {"partitioner", type::create_basic(type::Basic::text), "regular", -1},
        {"rack", type::create_basic(type::Basic::text), "regular", -1},
        {"release_version", type::create_basic(type::Basic::text), "regular", -1},
        {"rpc_address", type::create_basic(type::Basic::inet), "regular", -1},
        {"rpc_port", type::create_basic(type::Basic::int_), "regular", -1},
        {"schema_version", type::create_basic(type::Basic::uuid), "regular", -1},
        {"tokens", type::create_set(type::Basic::text), "regular", -1},
        {"extensions", type::create_map(type::Basic::text, type::Basic::blob), "regular", -1},
    };
    static const BuiltinCol sys_peers_cols[] = {
        {           "peer", type::create_basic(type::Basic::inet), "partition_key",  0},
        {    "data_center", type::create_basic(type::Basic::text),       "regular", -1},
        {        "host_id", type::create_basic(type::Basic::uuid),       "regular", -1},
        {   "preferred_ip", type::create_basic(type::Basic::inet),       "regular", -1},
        {           "rack", type::create_basic(type::Basic::text),       "regular", -1},
        {"release_version", type::create_basic(type::Basic::text),       "regular", -1},
        {    "rpc_address", type::create_basic(type::Basic::inet),       "regular", -1},
        { "schema_version", type::create_basic(type::Basic::uuid),       "regular", -1},
        {         "tokens",   type::create_set(type::Basic::text),       "regular", -1},
    };
    static const BuiltinCol sys_peers_v2_cols[] = {
        {           "peer", type::create_basic(type::Basic::text), "partition_key",  0},
        {      "peer_port", type::create_basic(type::Basic::int_),       "regular", -1},
        {    "data_center", type::create_basic(type::Basic::text),       "regular", -1},
        {        "host_id", type::create_basic(type::Basic::uuid),       "regular", -1},
        { "native_address", type::create_basic(type::Basic::text),       "regular", -1},
        {    "native_port", type::create_basic(type::Basic::int_),       "regular", -1},
        {   "preferred_ip", type::create_basic(type::Basic::text),       "regular", -1},
        { "preferred_port", type::create_basic(type::Basic::int_),       "regular", -1},
        {           "rack", type::create_basic(type::Basic::text),       "regular", -1},
        {"release_version", type::create_basic(type::Basic::text),       "regular", -1},
        { "schema_version", type::create_basic(type::Basic::uuid),       "regular", -1},
        {         "tokens", type::create_basic(type::Basic::text),       "regular", -1},
    };
    static const BuiltinCol ss_keyspaces_cols[] = {
        {"keyspace_name", type::create_basic(type::Basic::text), "partition_key", 0},
        {"durable_writes", type::create_basic(type::Basic::boolean), "regular", -1},
        {"replication", create_map(type::Basic::text, type::Basic::text), "regular", -1},
    };
    static const BuiltinCol ss_tables_cols[] = {
        {"keyspace_name", type::create_basic(type::Basic::text), "partition_key", 0},
        {"table_name", type::create_basic(type::Basic::text), "clustering", 0},
        {"bloom_filter_fp_chance", type::create_basic(type::Basic::double_), "regular", -1},
        {"caching", create_map(type::Basic::text, type::Basic::text), "regular", -1},
        {"comment", type::create_basic(type::Basic::text), "regular", -1},
        {"compaction", create_map(type::Basic::text, type::Basic::text), "regular", -1},
        {"compression", create_map(type::Basic::text, type::Basic::text), "regular", -1},
        {"crc_check_chance", type::create_basic(type::Basic::double_), "regular", -1},
        {"dclocal_read_repair_chance", type::create_basic(type::Basic::double_), "regular", -1},
        {"default_time_to_live", type::create_basic(type::Basic::int_), "regular", -1},
        {"flags", type::create_set(type::Basic::text), "regular", -1},
        {"gc_grace_seconds", type::create_basic(type::Basic::int_), "regular", -1},
        {"id", type::create_basic(type::Basic::uuid), "regular", -1},
        {"max_index_interval", type::create_basic(type::Basic::int_), "regular", -1},
        {"memtable_flush_period_in_ms", type::create_basic(type::Basic::int_), "regular", -1},
        {"min_index_interval", type::create_basic(type::Basic::int_), "regular", -1},
        {"read_repair", type::create_basic(type::Basic::text), "regular", -1},
        {"read_repair_chance", type::create_basic(type::Basic::double_), "regular", -1},
        {"speculative_retry", type::create_basic(type::Basic::text), "regular", -1},
        {"extensions", create_map(type::Basic::text, type::Basic::blob), "regular", -1},
    };
    static const BuiltinCol ss_columns_cols[] = {
        {   "keyspace_name", type::create_basic(type::Basic::text), "partition_key",  0},
        {      "table_name", type::create_basic(type::Basic::text),    "clustering",  0},
        {     "column_name", type::create_basic(type::Basic::text),    "clustering",  1},
        {"clustering_order", type::create_basic(type::Basic::text),       "regular", -1},
        {            "kind", type::create_basic(type::Basic::text),       "regular", -1},
        {        "position", type::create_basic(type::Basic::int_),       "regular", -1},
        {            "type", type::create_basic(type::Basic::text),       "regular", -1},
    };
    static const BuiltinCol ss_views_cols[] = {
        {"keyspace_name", type::create_basic(type::Basic::text), "partition_key", 0},
        {"view_name", type::create_basic(type::Basic::text), "clustering", 0},
        {"base_table_id", type::create_basic(type::Basic::uuid), "regular", -1},
        {"base_table_name", type::create_basic(type::Basic::text), "regular", -1},
        {"bloom_filter_fp_chance", type::create_basic(type::Basic::double_), "regular", -1},
        {"caching", create_map(type::Basic::text, type::Basic::text), "regular", -1},
        {"comment", type::create_basic(type::Basic::text), "regular", -1},
        {"compaction", create_map(type::Basic::text, type::Basic::text), "regular", -1},
        {"compression", create_map(type::Basic::text, type::Basic::text), "regular", -1},
        {"default_time_to_live", type::create_basic(type::Basic::int_), "regular", -1},
        {"gc_grace_seconds", type::create_basic(type::Basic::int_), "regular", -1},
        {"id", type::create_basic(type::Basic::uuid), "regular", -1},
        {"include_all_columns", type::create_basic(type::Basic::boolean), "regular", -1},
        {"max_index_interval", type::create_basic(type::Basic::int_), "regular", -1},
        {"memtable_flush_period_in_ms", type::create_basic(type::Basic::int_), "regular", -1},
        {"min_index_interval", type::create_basic(type::Basic::int_), "regular", -1},
        {"read_repair", type::create_basic(type::Basic::text), "regular", -1},
        {"read_repair_chance", type::create_basic(type::Basic::double_), "regular", -1},
        {"speculative_retry", type::create_basic(type::Basic::text), "regular", -1},
        {"where_clause", type::create_basic(type::Basic::text), "regular", -1},
        {"extensions", create_map(type::Basic::text, type::Basic::blob), "regular", -1},
    };
    static const BuiltinCol ss_indexes_cols[] = {
        {"keyspace_name", type::create_basic(type::Basic::text), "partition_key", 0},
        {"table_name", type::create_basic(type::Basic::text), "clustering", 0},
        {"index_name", type::create_basic(type::Basic::text), "clustering", 1},
        {"kind", type::create_basic(type::Basic::text), "regular", -1},
        {"options", create_map(type::Basic::text, type::Basic::text), "regular", -1},
        {"extensions", create_map(type::Basic::text, type::Basic::blob), "regular", -1},
    };
    static const BuiltinCol ss_triggers_cols[] = {
        {"keyspace_name", type::create_basic(type::Basic::text), "partition_key", 0},
        {"table_name", type::create_basic(type::Basic::text), "clustering", 0},
        {"trigger_name", type::create_basic(type::Basic::text), "clustering", 1},
        {"options", create_map(type::Basic::text, type::Basic::text), "regular", -1},
    };
    static const BuiltinCol ss_dropped_cols[] = {
        {"keyspace_name",      type::create_basic(type::Basic::text), "partition_key",  0},
        {   "table_name",      type::create_basic(type::Basic::text),    "clustering",  0},
        {  "column_name",      type::create_basic(type::Basic::text),    "clustering",  1},
        { "dropped_time", type::create_basic(type::Basic::timestamp),       "regular", -1},
        {         "kind",      type::create_basic(type::Basic::text),       "regular", -1},
        {         "type",      type::create_basic(type::Basic::text),       "regular", -1},
    };
    static const BuiltinCol ss_types_cols[] = {
        {"keyspace_name", type::create_basic(type::Basic::text), "partition_key",  0},
        {    "type_name", type::create_basic(type::Basic::text),    "clustering",  0},
        {  "field_names",        create_list(type::Basic::text),       "regular", -1},
        {  "field_types",        create_list(type::Basic::text),       "regular", -1},
    };
    static const BuiltinCol ss_functions_cols[] = {
        {       "keyspace_name",    type::create_basic(type::Basic::text), "partition_key",  0},
        {       "function_name",    type::create_basic(type::Basic::text),    "clustering",  0},
        {      "argument_types",           create_list(type::Basic::text),    "clustering",  1},
        {      "argument_names",           create_list(type::Basic::text),       "regular", -1},
        {                "body",    type::create_basic(type::Basic::text),       "regular", -1},
        {"called_on_null_input", type::create_basic(type::Basic::boolean),       "regular", -1},
        {            "language",    type::create_basic(type::Basic::text),       "regular", -1},
        {         "return_type",    type::create_basic(type::Basic::text),       "regular", -1},
    };
    static const BuiltinCol ss_aggregates_cols[] = {
        { "keyspace_name", type::create_basic(type::Basic::text), "partition_key",  0},
        {"aggregate_name", type::create_basic(type::Basic::text),    "clustering",  0},
        {"argument_types",        create_list(type::Basic::text),    "clustering",  1},
        {    "final_func", type::create_basic(type::Basic::text),       "regular", -1},
        {      "initcond", type::create_basic(type::Basic::text),       "regular", -1},
        {   "return_type", type::create_basic(type::Basic::text),       "regular", -1},
        {    "state_func", type::create_basic(type::Basic::text),       "regular", -1},
        {    "state_type", type::create_basic(type::Basic::text),       "regular", -1},
    };

#define BTBL(ks, tbl, arr)                           \
    BuiltinTbl {                                     \
        ks, tbl, arr, sizeof(arr) / sizeof((arr)[0]) \
    }
    static const BuiltinTbl builtin_tables[] = {
        BTBL("system", "local", sys_local_cols),
        BTBL("system", "peers", sys_peers_cols),
        BTBL("system", "peers_v2", sys_peers_v2_cols),
        BTBL("system_schema", "keyspaces", ss_keyspaces_cols),
        BTBL("system_schema", "tables", ss_tables_cols),
        BTBL("system_schema", "columns", ss_columns_cols),
        BTBL("system_schema", "views", ss_views_cols),
        BTBL("system_schema", "indexes", ss_indexes_cols),
        BTBL("system_schema", "triggers", ss_triggers_cols),
        BTBL("system_schema", "dropped_columns", ss_dropped_cols),
        BTBL("system_schema", "types", ss_types_cols),
        BTBL("system_schema", "functions", ss_functions_cols),
        BTBL("system_schema", "aggregates", ss_aggregates_cols),
    };
#undef BTBL

    static DynamicArray<VirtualColumn> columns_from(const BuiltinCol* cols, U64 ncols) {
        DynamicArray<VirtualColumn> out;
        for (U64 i = 0; i < ncols; i++) {
            emplace_back(out, VirtualColumn{cols[i].name, cols[i].type});
        }
        return out;
    }
#define BCOLS(arr) columns_from(arr, sizeof(arr) / sizeof((arr)[0]))

    VirtualRows create_schema_keyspaces(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "keyspaces";

        vr.columns = BCOLS(ss_keyspaces_cols);

        const char* builtin_keyspaces[] = {"system", "system_schema"};
        for (auto ks_name : builtin_keyspaces) {
            VirtualRow row;
            emplace_back(row.values, AutoString8(ks_name));
            emplace_back(row.values, U8(1));
            {
                DynamicMap<NestedColumnValue, NestedColumnValue> rep{};
                insert(rep, ncv("class"_as), ncv("org.apache.cassandra.locator.LocalStrategy"_as));
                emplace_back(row.values, move(rep));
            }
            emplace_back(vr.rows, move(row));
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) {
                continue;
            }
            VirtualRow row;
            emplace_back(row.values, AutoString8(ks.name));
            emplace_back(row.values, U8(1));
            {
                DynamicMap<NestedColumnValue, NestedColumnValue> rep{};
                insert(rep, ncv("class"_as), ncv("SimpleStrategy"_as));
                insert(rep, ncv("replication_factor"_as), ncv("1"_as));
                emplace_back(row.values, move(rep));
            }
            emplace_back(vr.rows, move(row));
        }

        return vr;
    }

    VirtualRows create_schema_tables(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "tables";

        vr.columns = BCOLS(ss_tables_cols);

        for (auto& tbl : builtin_tables) {
            VirtualRow row;
            emplace_back(row.values, AutoString8(tbl.ks));
            emplace_back(row.values, AutoString8(tbl.tbl));
            emplace_back(row.values, F64(0.01));
            emplace_back(row.values, default_caching());
            emplace_back(row.values, ""_as);
            emplace_back(row.values, default_compaction());
            emplace_back(row.values, default_compression());
            emplace_back(row.values, F64(1.0));
            emplace_back(row.values, F64(0.0));
            emplace_back(row.values, S32(0));
            {
                DynamicSet<NestedColumnValue> s{};
                insert(s, ncv("compound"_as));
                emplace_back(row.values, move(s));
            }
            emplace_back(row.values, S32(864000));
            emplace_back(row.values, table_uuid(String8(tbl.ks), String8(tbl.tbl)));
            emplace_back(row.values, S32(2048));
            emplace_back(row.values, S32(0));
            emplace_back(row.values, S32(128));
            emplace_back(row.values, "BLOCKING"_as);
            emplace_back(row.values, F64(0.0));
            emplace_back(row.values, "99PERCENTILE"_as);
            emplace_back(row.values, DynamicMap<NestedColumnValue, NestedColumnValue>{});
            emplace_back(vr.rows, move(row));
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) {
                continue;
            }
            for (auto& tbl : ks.tbls) {
                if (tbl.tombstone) {
                    continue;
                }
                VirtualRow row;
                emplace_back(row.values, AutoString8(ks.name));
                emplace_back(row.values, AutoString8(tbl.name));
                emplace_back(row.values, F64(0.01));
                emplace_back(row.values, default_caching());
                emplace_back(row.values, ""_as);
                emplace_back(row.values, default_compaction());
                emplace_back(row.values, default_compression());
                emplace_back(row.values, F64(1.0));
                emplace_back(row.values, F64(0.0));
                emplace_back(row.values, S32(tbl.options.default_ttl_ms / 1000));
                {
                    DynamicSet<NestedColumnValue> s{};
                    insert(s, ncv("compound"_as));
                    emplace_back(row.values, move(s));
                }
                emplace_back(row.values, tbl.options.gc_grace_seconds);
                emplace_back(row.values, table_uuid(ks.name, tbl.name));
                emplace_back(row.values, tbl.options.max_index_interval);
                emplace_back(row.values, tbl.options.memtable_flush_period_in_ms);
                emplace_back(row.values, tbl.options.min_index_interval);
                emplace_back(row.values, "BLOCKING"_as);
                emplace_back(row.values, F64(0.0));
                emplace_back(row.values, "99PERCENTILE"_as);
                emplace_back(row.values, DynamicMap<NestedColumnValue, NestedColumnValue>{});
                emplace_back(vr.rows, move(row));
            }
        }

        return vr;
    }

    VirtualRows create_schema_columns(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "columns";

        vr.columns = BCOLS(ss_columns_cols);

        for (auto& tbl : builtin_tables) {
            for (U64 ci = 0; ci < tbl.ncols; ci++) {
                auto&      col = tbl.cols[ci];
                VirtualRow row;
                emplace_back(row.values, AutoString8(tbl.ks));
                emplace_back(row.values, AutoString8(tbl.tbl));
                emplace_back(row.values, AutoString8(col.name));
                emplace_back(row.values, "none"_as);
                emplace_back(row.values, AutoString8(col.kind));
                emplace_back(row.values, col.pos);
                emplace_back(row.values, to_str(col.type));
                emplace_back(vr.rows, move(row));
            }
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) {
                continue;
            }
            for (auto& tbl : ks.tbls) {
                if (tbl.tombstone) {
                    continue;
                }
                S32 pos = 0;
                for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                    auto& col = tbl.cols[ci];
                    if (col.tombstone) {
                        continue;
                    }
                    bool       is_k = (col.key_kind == schema::KeyKind::PartitionKey || col.key_kind == schema::KeyKind::ClusteringKey);
                    VirtualRow row;
                    emplace_back(row.values, AutoString8(ks.name));
                    emplace_back(row.values, AutoString8(tbl.name));
                    emplace_back(row.values, AutoString8(col.name));
                    emplace_back(row.values, "none"_as);
                    // @todo check this
                    emplace_back(row.values, is_k ? "partition_key"_as : "regular"_as);
                    emplace_back(row.values, S32(is_k ? 0 : pos++));
                    emplace_back(row.values, AutoString8(to_str(col.type)));
                    emplace_back(vr.rows, move(row));
                }
            }
        }

        return vr;
    }

    VirtualRows create_schema_views(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "views";

        vr.columns = BCOLS(ss_views_cols);

        // @todo support materialized views
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_indexes(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "indexes";

        vr.columns = BCOLS(ss_indexes_cols);

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) {
                continue;
            }
            for (auto& tbl : ks.tbls) {
                if (tbl.tombstone) {
                    continue;
                }
                for (auto& idx : tbl.indexes) {
                    if (idx.tombstone) {
                        continue;
                    }
                    VirtualRow row;
                    emplace_back(row.values, AutoString8(ks.name));
                    emplace_back(row.values, AutoString8(tbl.name));
                    emplace_back(row.values, AutoString8(idx.name));
                    emplace_back(row.values, "COMPOSITES"_as);
                    {
                        DynamicMap<NestedColumnValue, NestedColumnValue> opts{};
                        insert(opts, ncv("target"_as), ncv(AutoString8(tbl.cols[idx.col_idx].name)));
                        emplace_back(row.values, move(opts));
                    }
                    emplace_back(row.values, DynamicMap<NestedColumnValue, NestedColumnValue>{});
                    emplace_back(vr.rows, move(row));
                }
            }
        }
        return vr;
    }

    VirtualRows create_schema_triggers(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "triggers";

        vr.columns = BCOLS(ss_triggers_cols);

        // @todo triggers
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_dropped_columns(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "dropped_columns";

        vr.columns = BCOLS(ss_dropped_cols);

        // @todo track dropped columns
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_types(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "types";

        vr.columns = BCOLS(ss_types_cols);

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) {
                continue;
            }
            for (auto it = ks.udts_by_name.begin(); it != ks.udts_by_name.end(); ++it) {
                type::UDT* udt = (*it).second;
                if (udt == nullptr) {
                    continue;
                }
                VirtualRow row;
                emplace_back(row.values, AutoString8(ks.name));
                emplace_back(row.values, AutoString8(udt->name));
                {
                    DynamicArray<NestedColumnValue> names;
                    for (const auto& fn : udt->field_names) {
                        push_back(names, ncv(AutoString8(fn)));
                    }
                    emplace_back(row.values, move(names));
                }
                {
                    DynamicArray<NestedColumnValue> types;
                    for (const auto& ft : udt->field_types) {
                        push_back(types, ncv(to_str(ft)));
                    }
                    emplace_back(row.values, move(types));
                }
                emplace_back(vr.rows, move(row));
            }
        }
        return vr;
    }

    VirtualRows create_schema_functions(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "functions";

        vr.columns = BCOLS(ss_functions_cols);

        // @todo support UDFs
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_aggregates(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table    = "aggregates";

        vr.columns = BCOLS(ss_aggregates_cols);

        // @todo support UDAs
        (void)schema;
        return vr;
    }

    // @note clients rely on a *changing* UUID to drive their schema metadata refresh,
    // so the value here must be a deterministic function of every persisted schema field.
    static UUID compute_schema_version(schema::Schema& schema) {
        U64  h1  = 14695981039346656037ULL;
        U64  h2  = 1099511628211ULL;
        auto mix = [&](U64 v) {
            h1 ^= v;
            h1 *= 1099511628211ULL;
            h2 ^= v + (h1 << 7) + (h1 >> 3);
            h2 *= 1469598103934665603ULL;
        };
        for (auto& ks : schema.keyspaces) {
            mix(U64(ks.tombstone));
            mix(ks.name.length);
            for (U64 i = 0; i < ks.name.length; i++) {
                mix(U64(U8(ks.name.data[i])));
            }
            for (auto& tbl : ks.tbls) {
                mix(U64(tbl.tombstone));
                mix(tbl.name.length);
                for (U64 i = 0; i < tbl.name.length; i++) {
                    mix(U64(U8(tbl.name.data[i])));
                }
                mix(tbl.cols.length);
                for (auto& col : tbl.cols) {
                    mix(U64(col.tombstone));
                    mix(col.name.length);
                    for (U64 i = 0; i < col.name.length; i++) {
                        mix(U64(U8(col.name.data[i])));
                    }
                }
                for (auto& idx : tbl.indexes) {
                    mix(U64(idx.tombstone));
                    mix(idx.col_idx);
                    mix(idx.name.length);
                    for (U64 i = 0; i < idx.name.length; i++) {
                        mix(U64(U8(idx.name.data[i])));
                    }
                }
            }
        }
        UUID out;
        for (U64 i = 0; i < 8; i++) {
            out.value[i] = U8((h1 >> (i * 8)) & 0xff);
        }
        for (U64 i = 0; i < 8; i++) {
            out.value[i + 8] = U8((h2 >> (i * 8)) & 0xff);
        }
        return out;
    }

    VirtualRows create_system_local(U16 port, schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table    = "local";

        vr.columns = BCOLS(sys_local_cols);

        auto create_loopback_ipv4 = []() {
            Blob b;
            push_back(b.value, U8(127));
            push_back(b.value, U8(0));
            push_back(b.value, U8(0));
            push_back(b.value, U8(1));
            return b;
        };

        VirtualRow row;
        emplace_back(row.values, "local"_as);
        emplace_back(row.values, "COMPLETED"_as);
        emplace_back(row.values, create_loopback_ipv4()); // broadcast_address
        emplace_back(row.values, S32(port));
        emplace_back(row.values, "cql"_as);
        emplace_back(row.values, "3.4.7"_as);
        emplace_back(row.values, "datacenter1"_as);
        emplace_back(row.values, UUID{
                                     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}
        });
        emplace_back(row.values, create_loopback_ipv4()); // listen_address
        emplace_back(row.values, S32(port));
        emplace_back(row.values, "5"_as);
        emplace_back(row.values, "org.apache.cassandra.dht.Murmur3Partitioner"_as);
        emplace_back(row.values, "rack1"_as);
        emplace_back(row.values, "3.11.19"_as);           // @note last version in 3.x, before system_virtual
        emplace_back(row.values, create_loopback_ipv4()); // rpc_address
        emplace_back(row.values, S32(port));
        emplace_back(row.values, compute_schema_version(schema));
        {
            DynamicSet<NestedColumnValue> s{};
            insert(s, ncv("0"_as));
            emplace_back(row.values, move(s));
        }
        emplace_back(row.values, DynamicMap<NestedColumnValue, NestedColumnValue>{});
        emplace_back(vr.rows, move(row));

        return vr;
    }

    VirtualRows create_system_peers() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table    = "peers";

        vr.columns = BCOLS(sys_peers_cols);

        return vr;
    }

    VirtualRows create_system_peers_v2() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table    = "peers_v2";

        vr.columns = BCOLS(sys_peers_v2_cols);

        return vr;
    }
#undef BCOLS
}
