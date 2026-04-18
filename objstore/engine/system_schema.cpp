module objstore.engine.system_schema;

import objstore.engine.types;

using namespace plexdb;

namespace objstore::engine {
    // Deterministic per-table UUID: high 8 bytes = hash of keyspace name,
    // low 8 bytes = hash of table name. Good enough as a stable fake ID.
    static Array<U8, 16> table_uuid(String8 ks, String8 tbl) {
        U64 h1 = 0xcbf29ce484222325ULL;
        for (U64 i = 0; i < ks.length; i++)
            h1 = (h1 ^ (U8)ks.data[i]) * 0x100000001b3ULL;
        U64 h2 = 0xcbf29ce484222325ULL;
        for (U64 i = 0; i < tbl.length; i++)
            h2 = (h2 ^ (U8)tbl.data[i]) * 0x100000001b3ULL;
        Array<U8, 16> uuid{};
        for (int i = 0; i < 8; i++) uuid[i]   = (U8)(h1 >> (i * 8));
        for (int i = 0; i < 8; i++) uuid[8+i] = (U8)(h2 >> (i * 8));
        // RFC 4122 version 4 / variant bits
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        return uuid;
    }

    static DynamicMap<AutoString8, AutoString8> default_caching() {
        return DynamicMap<AutoString8, AutoString8>{
            Pair<AutoString8, AutoString8>{"keys"_as,               "ALL"_as},
            Pair<AutoString8, AutoString8>{"rows_per_partition"_as, "NONE"_as},
        };
    }
    static DynamicMap<AutoString8, AutoString8> default_compaction() {
        return DynamicMap<AutoString8, AutoString8>{
            Pair<AutoString8, AutoString8>{"class"_as, "org.apache.cassandra.db.compaction.SizeTieredCompactionStrategy"_as},
        };
    }
    static DynamicMap<AutoString8, AutoString8> default_compression() {
        return DynamicMap<AutoString8, AutoString8>{
            Pair<AutoString8, AutoString8>{"chunk_length_in_kb"_as, "64"_as},
            Pair<AutoString8, AutoString8>{"class"_as, "org.apache.cassandra.io.compress.LZ4Compressor"_as},
        };
    }

    // ---- Built-in system virtual table definitions -------------------------
    struct BuiltinCol {
        const char* name;
        CqlType     type;
        const char* kind; // "partition_key", "clustering", or "regular"
        S32         pos;
    };
    struct BuiltinTbl {
        const char*       ks;
        const char*       tbl;
        const BuiltinCol* cols;
        U64               ncols;
    };

    static constexpr BuiltinCol sys_local_cols[] = {
        {"key",                     types::make_native(types::text), "partition_key", 0 },
        {"bootstrapped",            types::make_native(types::text), "regular",       -1},
        {"broadcast_address",       types::make_native(types::text), "regular",       -1},
        {"broadcast_port",          types::make_native(types::int_), "regular",       -1},
        {"cluster_name",            types::make_native(types::text), "regular",       -1},
        {"cql_version",             types::make_native(types::text), "regular",       -1},
        {"data_center",             types::make_native(types::text), "regular",       -1},
        {"host_id",                 types::make_native(types::uuid), "regular",       -1},
        {"listen_address",          types::make_native(types::text), "regular",       -1},
        {"listen_port",             types::make_native(types::int_), "regular",       -1},
        {"native_protocol_version", types::make_native(types::text), "regular",       -1},
        {"partitioner",             types::make_native(types::text), "regular",       -1},
        {"rack",                    types::make_native(types::text), "regular",       -1},
        {"release_version",         types::make_native(types::text), "regular",       -1},
        {"rpc_address",             types::make_native(types::text), "regular",       -1},
        {"rpc_port",                types::make_native(types::int_), "regular",       -1},
        {"schema_version",          types::make_native(types::uuid), "regular",       -1},
        {"tokens",                  types::make_set(types::text),    "regular",       -1},
    };
    static constexpr BuiltinCol sys_peers_cols[] = {
        {"peer",            types::make_native(types::text), "partition_key", 0 },
        {"data_center",     types::make_native(types::text), "regular",       -1},
        {"host_id",         types::make_native(types::uuid), "regular",       -1},
        {"preferred_ip",    types::make_native(types::text), "regular",       -1},
        {"rack",            types::make_native(types::text), "regular",       -1},
        {"release_version", types::make_native(types::text), "regular",       -1},
        {"rpc_address",     types::make_native(types::text), "regular",       -1},
        {"schema_version",  types::make_native(types::uuid), "regular",       -1},
        {"tokens",          types::make_set(types::text),    "regular",       -1},
    };
    static constexpr BuiltinCol sys_peers_v2_cols[] = {
        {"peer",            types::make_native(types::text), "partition_key", 0 },
        {"peer_port",       types::make_native(types::int_), "regular",       -1},
        {"data_center",     types::make_native(types::text), "regular",       -1},
        {"host_id",         types::make_native(types::uuid), "regular",       -1},
        {"native_address",  types::make_native(types::text), "regular",       -1},
        {"native_port",     types::make_native(types::int_), "regular",       -1},
        {"preferred_ip",    types::make_native(types::text), "regular",       -1},
        {"preferred_port",  types::make_native(types::int_), "regular",       -1},
        {"rack",            types::make_native(types::text), "regular",       -1},
        {"release_version", types::make_native(types::text), "regular",       -1},
        {"schema_version",  types::make_native(types::uuid), "regular",       -1},
        {"tokens",          types::make_native(types::text), "regular",       -1},
    };
    static constexpr BuiltinCol ss_keyspaces_cols[] = {
        {"keyspace_name",  types::make_native(types::text),           "partition_key", 0 },
        {"durable_writes", types::make_native(types::boolean),        "regular",       -1},
        {"replication",    types::make_map(types::text, types::text), "regular",       -1},
    };
    static constexpr BuiltinCol ss_tables_cols[] = {
        {"keyspace_name",               types::make_native(types::text),           "partition_key", 0 },
        {"table_name",                  types::make_native(types::text),           "clustering",    0 },
        {"bloom_filter_fp_chance",      types::make_native(types::double_),        "regular",       -1},
        {"caching",                     types::make_map(types::text, types::text), "regular",       -1},
        {"comment",                     types::make_native(types::text),           "regular",       -1},
        {"compaction",                  types::make_map(types::text, types::text), "regular",       -1},
        {"compression",                 types::make_map(types::text, types::text), "regular",       -1},
        {"crc_check_chance",            types::make_native(types::double_),        "regular",       -1},
        {"dclocal_read_repair_chance",  types::make_native(types::double_),        "regular",       -1},
        {"default_time_to_live",        types::make_native(types::int_),           "regular",       -1},
        {"flags",                       types::make_set(types::text),              "regular",       -1},
        {"gc_grace_seconds",            types::make_native(types::int_),           "regular",       -1},
        {"id",                          types::make_native(types::uuid),           "regular",       -1},
        {"max_index_interval",          types::make_native(types::int_),           "regular",       -1},
        {"memtable_flush_period_in_ms", types::make_native(types::int_),           "regular",       -1},
        {"min_index_interval",          types::make_native(types::int_),           "regular",       -1},
        {"read_repair",                 types::make_native(types::text),           "regular",       -1},
        {"read_repair_chance",          types::make_native(types::double_),        "regular",       -1},
        {"speculative_retry",           types::make_native(types::text),           "regular",       -1},
    };
    static constexpr BuiltinCol ss_columns_cols[] = {
        {"keyspace_name",    types::make_native(types::text), "partition_key", 0 },
        {"table_name",       types::make_native(types::text), "clustering",    0 },
        {"column_name",      types::make_native(types::text), "clustering",    1 },
        {"clustering_order", types::make_native(types::text), "regular",       -1},
        {"kind",             types::make_native(types::text), "regular",       -1},
        {"position",         types::make_native(types::int_), "regular",       -1},
        {"type",             types::make_native(types::text), "regular",       -1},
    };
    static constexpr BuiltinCol ss_views_cols[] = {
        {"keyspace_name",               types::make_native(types::text),           "partition_key", 0 },
        {"view_name",                   types::make_native(types::text),           "clustering",    0 },
        {"base_table_id",               types::make_native(types::uuid),           "regular",       -1},
        {"base_table_name",             types::make_native(types::text),           "regular",       -1},
        {"bloom_filter_fp_chance",      types::make_native(types::double_),        "regular",       -1},
        {"caching",                     types::make_map(types::text, types::text), "regular",       -1},
        {"comment",                     types::make_native(types::text),           "regular",       -1},
        {"compaction",                  types::make_map(types::text, types::text), "regular",       -1},
        {"compression",                 types::make_map(types::text, types::text), "regular",       -1},
        {"default_time_to_live",        types::make_native(types::int_),           "regular",       -1},
        {"gc_grace_seconds",            types::make_native(types::int_),           "regular",       -1},
        {"id",                          types::make_native(types::uuid),           "regular",       -1},
        {"include_all_columns",         types::make_native(types::boolean),        "regular",       -1},
        {"max_index_interval",          types::make_native(types::int_),           "regular",       -1},
        {"memtable_flush_period_in_ms", types::make_native(types::int_),           "regular",       -1},
        {"min_index_interval",          types::make_native(types::int_),           "regular",       -1},
        {"read_repair",                 types::make_native(types::text),           "regular",       -1},
        {"read_repair_chance",          types::make_native(types::double_),        "regular",       -1},
        {"speculative_retry",           types::make_native(types::text),           "regular",       -1},
        {"where_clause",                types::make_native(types::text),           "regular",       -1},
    };
    static constexpr BuiltinCol ss_indexes_cols[] = {
        {"keyspace_name", types::make_native(types::text),           "partition_key", 0 },
        {"table_name",    types::make_native(types::text),           "clustering",    0 },
        {"index_name",    types::make_native(types::text),           "clustering",    1 },
        {"kind",          types::make_native(types::text),           "regular",       -1},
        {"options",       types::make_map(types::text, types::text), "regular",       -1},
    };
    static constexpr BuiltinCol ss_triggers_cols[] = {
        {"keyspace_name", types::make_native(types::text),           "partition_key", 0 },
        {"table_name",    types::make_native(types::text),           "clustering",    0 },
        {"trigger_name",  types::make_native(types::text),           "clustering",    1 },
        {"options",       types::make_map(types::text, types::text), "regular",       -1},
    };
    static constexpr BuiltinCol ss_dropped_cols[] = {
        {"keyspace_name", types::make_native(types::text),      "partition_key", 0 },
        {"table_name",    types::make_native(types::text),      "clustering",    0 },
        {"column_name",   types::make_native(types::text),      "clustering",    1 },
        {"dropped_time",  types::make_native(types::timestamp), "regular",       -1},
        {"kind",          types::make_native(types::text),      "regular",       -1},
        {"type",          types::make_native(types::text),      "regular",       -1},
    };
    static constexpr BuiltinCol ss_types_cols[] = {
        {"keyspace_name", types::make_native(types::text), "partition_key", 0 },
        {"type_name",     types::make_native(types::text), "clustering",    0 },
        {"field_names",   types::make_list(types::text),   "regular",       -1},
        {"field_types",   types::make_list(types::text),   "regular",       -1},
    };
    static constexpr BuiltinCol ss_functions_cols[] = {
        {"keyspace_name",        types::make_native(types::text),    "partition_key", 0 },
        {"function_name",        types::make_native(types::text),    "clustering",    0 },
        {"argument_types",       types::make_list(types::text),      "clustering",    1 },
        {"argument_names",       types::make_list(types::text),      "regular",       -1},
        {"body",                 types::make_native(types::text),    "regular",       -1},
        {"called_on_null_input", types::make_native(types::boolean), "regular",       -1},
        {"language",             types::make_native(types::text),    "regular",       -1},
        {"return_type",          types::make_native(types::text),    "regular",       -1},
    };
    static constexpr BuiltinCol ss_aggregates_cols[] = {
        {"keyspace_name",  types::make_native(types::text), "partition_key", 0 },
        {"aggregate_name", types::make_native(types::text), "clustering",    0 },
        {"argument_types", types::make_list(types::text),   "clustering",    1 },
        {"final_func",     types::make_native(types::text), "regular",       -1},
        {"initcond",       types::make_native(types::text), "regular",       -1},
        {"return_type",    types::make_native(types::text), "regular",       -1},
        {"state_func",     types::make_native(types::text), "regular",       -1},
        {"state_type",     types::make_native(types::text), "regular",       -1},
    };

    #define BTBL(ks, tbl, arr) BuiltinTbl{ks, tbl, arr, sizeof(arr)/sizeof((arr)[0])}
    static constexpr BuiltinTbl builtin_tables[] = {
        BTBL("system",        "local",           sys_local_cols),
        BTBL("system",        "peers",           sys_peers_cols),
        BTBL("system",        "peers_v2",        sys_peers_v2_cols),
        BTBL("system_schema", "keyspaces",       ss_keyspaces_cols),
        BTBL("system_schema", "tables",          ss_tables_cols),
        BTBL("system_schema", "columns",         ss_columns_cols),
        BTBL("system_schema", "views",           ss_views_cols),
        BTBL("system_schema", "indexes",         ss_indexes_cols),
        BTBL("system_schema", "triggers",        ss_triggers_cols),
        BTBL("system_schema", "dropped_columns", ss_dropped_cols),
        BTBL("system_schema", "types",           ss_types_cols),
        BTBL("system_schema", "functions",       ss_functions_cols),
        BTBL("system_schema", "aggregates",      ss_aggregates_cols),
    };
    #undef BTBL


    VirtualRows make_schema_keyspaces(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "keyspaces";

        push_back(vr.columns, VirtualColumn{"keyspace_name",  types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"durable_writes", types::make_native(types::boolean)});
        push_back(vr.columns, VirtualColumn{"replication",    types::make_map(types::text, types::text)});

        for (auto ks_name : {"system", "system_schema"}) {
            VirtualRow row;
            push_back(row.values, types::ReadValue{AutoString8(ks_name)});
            push_back(row.values, types::ReadValue{U8(1)});
            push_back(row.values, types::ReadValue{DynamicMap<AutoString8, AutoString8>{
                Pair<AutoString8, AutoString8>{"class"_as, "org.apache.cassandra.locator.LocalStrategy"_as},
            }});
            push_back(vr.rows, move(row));
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) continue;
            VirtualRow row;
            push_back(row.values, types::ReadValue{AutoString8(ks.name)});
            push_back(row.values, types::ReadValue{U8(1)});
            push_back(row.values, types::ReadValue{DynamicMap<AutoString8, AutoString8>{
                Pair<AutoString8, AutoString8>{"class"_as,              "SimpleStrategy"_as},
                Pair<AutoString8, AutoString8>{"replication_factor"_as, "1"_as},
            }});
            push_back(vr.rows, move(row));
        }

        return vr;
    }

    VirtualRows make_schema_tables(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "tables";

        push_back(vr.columns, VirtualColumn{"keyspace_name",               types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"table_name",                  types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"bloom_filter_fp_chance",      types::make_native(types::double_)});
        push_back(vr.columns, VirtualColumn{"caching",                     types::make_map(types::text, types::text)});
        push_back(vr.columns, VirtualColumn{"comment",                     types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"compaction",                  types::make_map(types::text, types::text)});
        push_back(vr.columns, VirtualColumn{"compression",                 types::make_map(types::text, types::text)});
        push_back(vr.columns, VirtualColumn{"crc_check_chance",            types::make_native(types::double_)});
        push_back(vr.columns, VirtualColumn{"dclocal_read_repair_chance",  types::make_native(types::double_)});
        push_back(vr.columns, VirtualColumn{"default_time_to_live",        types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"flags",                       types::make_set(types::text)});
        push_back(vr.columns, VirtualColumn{"gc_grace_seconds",            types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"id",                          types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"max_index_interval",          types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"memtable_flush_period_in_ms", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"min_index_interval",          types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"read_repair",                 types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"read_repair_chance",          types::make_native(types::double_)});
        push_back(vr.columns, VirtualColumn{"speculative_retry",           types::make_native(types::text)});

        for (auto& tbl : builtin_tables) {
            VirtualRow row;
            push_back(row.values, types::ReadValue{AutoString8(tbl.ks)});
            push_back(row.values, types::ReadValue{AutoString8(tbl.tbl)});
            push_back(row.values, types::ReadValue{F64(0.01)});
            push_back(row.values, types::ReadValue{default_caching()});
            push_back(row.values, types::ReadValue{""_as});
            push_back(row.values, types::ReadValue{default_compaction()});
            push_back(row.values, types::ReadValue{default_compression()});
            push_back(row.values, types::ReadValue{F64(1.0)});
            push_back(row.values, types::ReadValue{F64(0.0)});
            push_back(row.values, types::ReadValue{S32(0)});
            push_back(row.values, types::ReadValue{DynamicSet<AutoString8>{"compound"_as}});
            push_back(row.values, types::ReadValue{S32(864000)});
            push_back(row.values, types::ReadValue{table_uuid(String8(tbl.ks), String8(tbl.tbl))});
            push_back(row.values, types::ReadValue{S32(2048)});
            push_back(row.values, types::ReadValue{S32(0)});
            push_back(row.values, types::ReadValue{S32(128)});
            push_back(row.values, types::ReadValue{"BLOCKING"_as});
            push_back(row.values, types::ReadValue{F64(0.0)});
            push_back(row.values, types::ReadValue{"99PERCENTILE"_as});
            push_back(vr.rows, move(row));
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) continue;
            for (auto& tbl : ks.tbls) {
                if (tbl.tombstone) continue;
                VirtualRow row;
                push_back(row.values, types::ReadValue{AutoString8(ks.name)});
                push_back(row.values, types::ReadValue{AutoString8(tbl.name)});
                push_back(row.values, types::ReadValue{F64(0.01)});
                push_back(row.values, types::ReadValue{default_caching()});
                push_back(row.values, types::ReadValue{""_as});
                push_back(row.values, types::ReadValue{default_compaction()});
                push_back(row.values, types::ReadValue{default_compression()});
                push_back(row.values, types::ReadValue{F64(1.0)});
                push_back(row.values, types::ReadValue{F64(0.0)});
                push_back(row.values, types::ReadValue{S32(0)});
                push_back(row.values, types::ReadValue{DynamicSet<AutoString8>{"compound"_as}});
                push_back(row.values, types::ReadValue{S32(864000)});
                push_back(row.values, types::ReadValue{table_uuid(ks.name, tbl.name)});
                push_back(row.values, types::ReadValue{S32(2048)});
                push_back(row.values, types::ReadValue{S32(0)});
                push_back(row.values, types::ReadValue{S32(128)});
                push_back(row.values, types::ReadValue{"BLOCKING"_as});
                push_back(row.values, types::ReadValue{F64(0.0)});
                push_back(row.values, types::ReadValue{"99PERCENTILE"_as});
                push_back(vr.rows, move(row));
            }
        }

        return vr;
    }

    VirtualRows make_schema_columns(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "columns";

        push_back(vr.columns, VirtualColumn{"keyspace_name",    types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"table_name",       types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"column_name",      types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"clustering_order", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"kind",             types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"position",         types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"type",             types::make_native(types::text)});

        for (auto& tbl : builtin_tables) {
            for (U64 ci = 0; ci < tbl.ncols; ci++) {
                auto& col = tbl.cols[ci];
                VirtualRow row;
                push_back(row.values, types::ReadValue{AutoString8(tbl.ks)});
                push_back(row.values, types::ReadValue{AutoString8(tbl.tbl)});
                push_back(row.values, types::ReadValue{AutoString8(col.name)});
                push_back(row.values, types::ReadValue{"none"_as});
                push_back(row.values, types::ReadValue{AutoString8(col.kind)});
                push_back(row.values, types::ReadValue{col.pos});
                push_back(row.values, types::ReadValue{types::to_str(col.type)});
                push_back(vr.rows, move(row));
            }
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) continue;
            for (auto& tbl : ks.tbls) {
                if (tbl.tombstone) continue;
                S32 pos = 0;
                for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                    auto& col = tbl.cols[ci];
                    if (col.tombstone) continue;
                    bool is_pk = (ci == tbl.primary_col_idx);
                    VirtualRow row;
                    push_back(row.values, types::ReadValue{AutoString8(ks.name)});
                    push_back(row.values, types::ReadValue{AutoString8(tbl.name)});
                    push_back(row.values, types::ReadValue{AutoString8(col.name)});
                    push_back(row.values, types::ReadValue{"none"_as});
                    push_back(row.values, types::ReadValue{is_pk ? "partition_key"_as : "regular"_as});
                    push_back(row.values, types::ReadValue{S32(is_pk ? 0 : pos++)});
                    push_back(row.values, types::ReadValue{AutoString8(types::to_str(col.type))});
                    push_back(vr.rows, move(row));
                }
            }
        }

        return vr;
    }

    VirtualRows make_schema_views(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "views";

        push_back(vr.columns, VirtualColumn{"keyspace_name",               types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"view_name",                   types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"base_table_id",               types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"base_table_name",             types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"bloom_filter_fp_chance",      types::make_native(types::double_)});
        push_back(vr.columns, VirtualColumn{"caching",                     types::make_map(types::text, types::text)});
        push_back(vr.columns, VirtualColumn{"comment",                     types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"compaction",                  types::make_map(types::text, types::text)});
        push_back(vr.columns, VirtualColumn{"compression",                 types::make_map(types::text, types::text)});
        push_back(vr.columns, VirtualColumn{"default_time_to_live",        types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"gc_grace_seconds",            types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"id",                          types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"include_all_columns",         types::make_native(types::boolean)});
        push_back(vr.columns, VirtualColumn{"max_index_interval",          types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"memtable_flush_period_in_ms", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"min_index_interval",          types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"read_repair",                 types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"read_repair_chance",          types::make_native(types::double_)});
        push_back(vr.columns, VirtualColumn{"speculative_retry",           types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"where_clause",                types::make_native(types::text)});

        // @todo support materialized views
        (void)schema;
        return vr;
    }

    VirtualRows make_schema_indexes(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "indexes";

        push_back(vr.columns, VirtualColumn{"keyspace_name", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"table_name",    types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"index_name",    types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"kind",          types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"options",       types::make_map(types::text, types::text)});

        // @todo secondary indexes
        (void)schema;
        return vr;
    }

    VirtualRows make_schema_triggers(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "triggers";

        push_back(vr.columns, VirtualColumn{"keyspace_name",  types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"table_name",     types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"trigger_name",   types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"options",        types::make_map(types::text, types::text)});

        // @todo triggers
        (void)schema;
        return vr;
    }

    VirtualRows make_schema_dropped_columns(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "dropped_columns";

        push_back(vr.columns, VirtualColumn{"keyspace_name", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"table_name",    types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"column_name",   types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"dropped_time",  types::make_native(types::timestamp)});
        push_back(vr.columns, VirtualColumn{"kind",          types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"type",          types::make_native(types::text)});

        // @todo track dropped columns
        (void)schema;
        return vr;
    }

    VirtualRows make_schema_types(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "types";

        push_back(vr.columns, VirtualColumn{"keyspace_name", types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"type_name",     types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"field_names",   types::make_list(types::text)});
        push_back(vr.columns, VirtualColumn{"field_types",   types::make_list(types::text)});

        // @todo support UDTs
        (void)schema;
        return vr;
    }

    VirtualRows make_schema_functions(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "functions";

        push_back(vr.columns, VirtualColumn{"keyspace_name",       types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"function_name",       types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"argument_types",      types::make_list(types::text)});
        push_back(vr.columns, VirtualColumn{"argument_names",      types::make_list(types::text)});
        push_back(vr.columns, VirtualColumn{"body",                types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"called_on_null_input", types::make_native(types::boolean)});
        push_back(vr.columns, VirtualColumn{"language",            types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"return_type",         types::make_native(types::text)});

        // @todo support UDFs
        (void)schema;
        return vr;
    }

    VirtualRows make_schema_aggregates(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "aggregates";

        push_back(vr.columns, VirtualColumn{"keyspace_name",   types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"aggregate_name",  types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"argument_types",  types::make_list(types::text)});
        push_back(vr.columns, VirtualColumn{"final_func",      types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"initcond",        types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"return_type",     types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"state_func",      types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"state_type",      types::make_native(types::text)});

        // @todo support UDAs
        (void)schema;
        return vr;
    }
}
