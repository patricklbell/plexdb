module cql.engine.system_schema;

import cql.engine.types;
import cql.engine.evaluator;
import cql.engine.statements;

using namespace plexdb;

namespace cql::engine {
    // Deterministic per-table UUID: high 8 bytes = hash of keyspace name,
    // low 8 bytes = hash of table name. Good enough as a stable fake ID.
    static UUID table_uuid(String8 ks, String8 tbl) {
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
        return {uuid};
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
        Type     type;
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
        {"key",                     create_basic(BasicType::text), "partition_key",  0},
        {"bootstrapped",            create_basic(BasicType::text), "regular",       -1},
        {"broadcast_address",       create_basic(BasicType::inet), "regular",       -1},
        {"broadcast_port",          create_basic(BasicType::int_), "regular",       -1},
        {"cluster_name",            create_basic(BasicType::text), "regular",       -1},
        {"cql_version",             create_basic(BasicType::text), "regular",       -1},
        {"data_center",             create_basic(BasicType::text), "regular",       -1},
        {"host_id",                 create_basic(BasicType::uuid), "regular",       -1},
        {"listen_address",          create_basic(BasicType::inet), "regular",       -1},
        {"listen_port",             create_basic(BasicType::int_), "regular",       -1},
        {"native_protocol_version", create_basic(BasicType::text), "regular",       -1},
        {"partitioner",             create_basic(BasicType::text), "regular",       -1},
        {"rack",                    create_basic(BasicType::text), "regular",       -1},
        {"release_version",         create_basic(BasicType::text), "regular",       -1},
        {"rpc_address",             create_basic(BasicType::inet), "regular",       -1},
        {"rpc_port",                create_basic(BasicType::int_), "regular",       -1},
        {"schema_version",          create_basic(BasicType::uuid), "regular",       -1},
        {"tokens",                  create_set  (BasicType::text),    "regular",    -1},
    };
    static constexpr BuiltinCol sys_peers_cols[] = {
        {"peer",            create_basic(BasicType::inet), "partition_key",  0},
        {"data_center",     create_basic(BasicType::text), "regular",       -1},
        {"host_id",         create_basic(BasicType::uuid), "regular",       -1},
        {"preferred_ip",    create_basic(BasicType::inet), "regular",       -1},
        {"rack",            create_basic(BasicType::text), "regular",       -1},
        {"release_version", create_basic(BasicType::text), "regular",       -1},
        {"rpc_address",     create_basic(BasicType::inet), "regular",       -1},
        {"schema_version",  create_basic(BasicType::uuid), "regular",       -1},
        {"tokens",          create_set  (BasicType::text),    "regular",       -1},
    };
    static constexpr BuiltinCol sys_peers_v2_cols[] = {
        {"peer",            create_basic(BasicType::text), "partition_key",  0},
        {"peer_port",       create_basic(BasicType::int_), "regular",       -1},
        {"data_center",     create_basic(BasicType::text), "regular",       -1},
        {"host_id",         create_basic(BasicType::uuid), "regular",       -1},
        {"native_address",  create_basic(BasicType::text), "regular",       -1},
        {"native_port",     create_basic(BasicType::int_), "regular",       -1},
        {"preferred_ip",    create_basic(BasicType::text), "regular",       -1},
        {"preferred_port",  create_basic(BasicType::int_), "regular",       -1},
        {"rack",            create_basic(BasicType::text), "regular",       -1},
        {"release_version", create_basic(BasicType::text), "regular",       -1},
        {"schema_version",  create_basic(BasicType::uuid), "regular",       -1},
        {"tokens",          create_basic(BasicType::text), "regular",       -1},
    };
    static constexpr BuiltinCol ss_keyspaces_cols[] = {
        {"keyspace_name",  create_basic(BasicType::text),                     "partition_key", 0},
        {"durable_writes", create_basic(BasicType::boolean),                  "regular",      -1},
        {"replication",    create_map  (BasicType::text, BasicType::text),    "regular",      -1},
    };
    static constexpr BuiltinCol ss_tables_cols[] = {
        {"keyspace_name",               create_basic(BasicType::text),                  "partition_key",   0},
        {"table_name",                  create_basic(BasicType::text),                  "clustering",      0},
        {"bloom_filter_fp_chance",      create_basic(BasicType::double_),               "regular",        -1},
        {"caching",                     create_map  (BasicType::text, BasicType::text), "regular",        -1},
        {"comment",                     create_basic(BasicType::text),                  "regular",        -1},
        {"compaction",                  create_map  (BasicType::text, BasicType::text), "regular",        -1},
        {"compression",                 create_map  (BasicType::text, BasicType::text), "regular",        -1},
        {"crc_check_chance",            create_basic(BasicType::double_),               "regular",        -1},
        {"dclocal_read_repair_chance",  create_basic(BasicType::double_),               "regular",        -1},
        {"default_time_to_live",        create_basic(BasicType::int_),                  "regular",        -1},
        {"flags",                       create_set  (BasicType::text),                  "regular",        -1},
        {"gc_grace_seconds",            create_basic(BasicType::int_),                  "regular",        -1},
        {"id",                          create_basic(BasicType::uuid),                  "regular",        -1},
        {"max_index_interval",          create_basic(BasicType::int_),                  "regular",        -1},
        {"memtable_flush_period_in_ms", create_basic(BasicType::int_),                  "regular",        -1},
        {"min_index_interval",          create_basic(BasicType::int_),                  "regular",        -1},
        {"read_repair",                 create_basic(BasicType::text),                  "regular",        -1},
        {"read_repair_chance",          create_basic(BasicType::double_),               "regular",        -1},
        {"speculative_retry",           create_basic(BasicType::text),                  "regular",        -1},
    };
    static constexpr BuiltinCol ss_columns_cols[] = {
        {"keyspace_name",    create_basic(BasicType::text), "partition_key", 0},
        {"table_name",       create_basic(BasicType::text), "clustering",    0},
        {"column_name",      create_basic(BasicType::text), "clustering",    1},
        {"clustering_order", create_basic(BasicType::text), "regular",      -1},
        {"kind",             create_basic(BasicType::text), "regular",      -1},
        {"position",         create_basic(BasicType::int_), "regular",      -1},
        {"type",             create_basic(BasicType::text), "regular",      -1},
    };
    static constexpr BuiltinCol ss_views_cols[] = {
        {"keyspace_name",               create_basic(BasicType::text),                  "partition_key",  0},
        {"view_name",                   create_basic(BasicType::text),                  "clustering",     0},
        {"base_table_id",               create_basic(BasicType::uuid),                  "regular",       -1},
        {"base_table_name",             create_basic(BasicType::text),                  "regular",       -1},
        {"bloom_filter_fp_chance",      create_basic(BasicType::double_),               "regular",       -1},
        {"caching",                     create_map  (BasicType::text, BasicType::text), "regular",       -1},
        {"comment",                     create_basic(BasicType::text),                  "regular",       -1},
        {"compaction",                  create_map  (BasicType::text, BasicType::text), "regular",       -1},
        {"compression",                 create_map  (BasicType::text, BasicType::text), "regular",       -1},
        {"default_time_to_live",        create_basic(BasicType::int_),                  "regular",       -1},
        {"gc_grace_seconds",            create_basic(BasicType::int_),                  "regular",       -1},
        {"id",                          create_basic(BasicType::uuid),                  "regular",       -1},
        {"include_all_columns",         create_basic(BasicType::boolean),               "regular",       -1},
        {"max_index_interval",          create_basic(BasicType::int_),                  "regular",       -1},
        {"memtable_flush_period_in_ms", create_basic(BasicType::int_),                  "regular",       -1},
        {"min_index_interval",          create_basic(BasicType::int_),                  "regular",       -1},
        {"read_repair",                 create_basic(BasicType::text),                  "regular",       -1},
        {"read_repair_chance",          create_basic(BasicType::double_),               "regular",       -1},
        {"speculative_retry",           create_basic(BasicType::text),                  "regular",       -1},
        {"where_clause",                create_basic(BasicType::text),                  "regular",       -1},
    };
    static constexpr BuiltinCol ss_indexes_cols[] = {
        {"keyspace_name", create_basic(BasicType::text),                  "partition_key", 0},
        {"table_name",    create_basic(BasicType::text),                  "clustering",    0},
        {"index_name",    create_basic(BasicType::text),                  "clustering",    1},
        {"kind",          create_basic(BasicType::text),                  "regular",      -1},
        {"options",       create_map  (BasicType::text, BasicType::text), "regular",      -1},
    };
    static constexpr BuiltinCol ss_triggers_cols[] = {
        {"keyspace_name", create_basic(BasicType::text),                  "partition_key",  0},
        {"table_name",    create_basic(BasicType::text),                  "clustering",     0},
        {"trigger_name",  create_basic(BasicType::text),                  "clustering",     1},
        {"options",       create_map  (BasicType::text, BasicType::text), "regular",       -1},
    };
    static constexpr BuiltinCol ss_dropped_cols[] = {
        {"keyspace_name", create_basic(BasicType::text),      "partition_key", 0},
        {"table_name",    create_basic(BasicType::text),      "clustering",    0},
        {"column_name",   create_basic(BasicType::text),      "clustering",    1},
        {"dropped_time",  create_basic(BasicType::timestamp), "regular",      -1},
        {"kind",          create_basic(BasicType::text),      "regular",      -1},
        {"type",          create_basic(BasicType::text),      "regular",      -1},
    };
    static constexpr BuiltinCol ss_types_cols[] = {
        {"keyspace_name", create_basic(BasicType::text),   "partition_key",  0},
        {"type_name",     create_basic(BasicType::text),   "clustering",     0},
        {"field_names",   create_list (BasicType::text),   "regular",       -1},
        {"field_types",   create_list (BasicType::text),   "regular",       -1},
    };
    static constexpr BuiltinCol ss_functions_cols[] = {
        {"keyspace_name",        create_basic(BasicType::text),    "partition_key", 0},
        {"function_name",        create_basic(BasicType::text),    "clustering",    0},
        {"argument_types",       create_list (BasicType::text),    "clustering",    1},
        {"argument_names",       create_list (BasicType::text),    "regular",      -1},
        {"body",                 create_basic(BasicType::text),    "regular",      -1},
        {"called_on_null_input", create_basic(BasicType::boolean), "regular",      -1},
        {"language",             create_basic(BasicType::text),    "regular",      -1},
        {"return_type",          create_basic(BasicType::text),    "regular",      -1},
    };
    static constexpr BuiltinCol ss_aggregates_cols[] = {
        {"keyspace_name",  create_basic(BasicType::text),   "partition_key",  0},
        {"aggregate_name", create_basic(BasicType::text),   "clustering",     0},
        {"argument_types", create_list (BasicType::text),   "clustering",     1},
        {"final_func",     create_basic(BasicType::text),   "regular",       -1},
        {"initcond",       create_basic(BasicType::text),   "regular",       -1},
        {"return_type",    create_basic(BasicType::text),   "regular",       -1},
        {"state_func",     create_basic(BasicType::text),   "regular",       -1},
        {"state_type",     create_basic(BasicType::text),   "regular",       -1},
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

    VirtualRows create_schema_keyspaces(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "keyspaces";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name",  create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"durable_writes", create_basic(BasicType::boolean)});
        emplace_back(vr.columns, VirtualColumn{"replication",    create_map  (BasicType::text, BasicType::text)});

        for (auto ks_name : {"system", "system_schema"}) {
            VirtualRow row;
            emplace_back(row.values, AutoString8(ks_name));
            emplace_back(row.values, U8(1));
            emplace_back(row.values, DynamicMap<AutoString8, AutoString8>{
                Pair<AutoString8, AutoString8>{"class"_as, "org.apache.cassandra.locator.LocalStrategy"_as},
            });
            emplace_back(vr.rows, move(row));
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) continue;
            VirtualRow row;
            emplace_back(row.values, AutoString8(ks.name));
            emplace_back(row.values, U8(1));
            emplace_back(row.values, DynamicMap<AutoString8, AutoString8>{
                Pair<AutoString8, AutoString8>{"class"_as,              "SimpleStrategy"_as},
                Pair<AutoString8, AutoString8>{"replication_factor"_as, "1"_as},
            });
            emplace_back(vr.rows, move(row));
        }

        return vr;
    }

    VirtualRows create_schema_tables(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "tables";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name",               create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"table_name",                  create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"bloom_filter_fp_chance",      create_basic(BasicType::double_)});
        emplace_back(vr.columns, VirtualColumn{"caching",                     create_map(BasicType::text, BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"comment",                     create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"compaction",                  create_map(BasicType::text, BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"compression",                 create_map(BasicType::text, BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"crc_check_chance",            create_basic(BasicType::double_)});
        emplace_back(vr.columns, VirtualColumn{"dclocal_read_repair_chance",  create_basic(BasicType::double_)});
        emplace_back(vr.columns, VirtualColumn{"default_time_to_live",        create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"flags",                       create_set(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"gc_grace_seconds",            create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"id",                          create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"max_index_interval",          create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"memtable_flush_period_in_ms", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"min_index_interval",          create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"read_repair",                 create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"read_repair_chance",          create_basic(BasicType::double_)});
        emplace_back(vr.columns, VirtualColumn{"speculative_retry",           create_basic(BasicType::text)});

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
            emplace_back(row.values, DynamicSet<AutoString8>{"compound"_as});
            emplace_back(row.values, S32(864000));
            emplace_back(row.values, table_uuid(String8(tbl.ks), String8(tbl.tbl)));
            emplace_back(row.values, S32(2048));
            emplace_back(row.values, S32(0));
            emplace_back(row.values, S32(128));
            emplace_back(row.values, "BLOCKING"_as);
            emplace_back(row.values, F64(0.0));
            emplace_back(row.values, "99PERCENTILE"_as);
            emplace_back(vr.rows, move(row));
        }

        for (auto& ks : schema.keyspaces) {
            if (ks.tombstone) continue;
            for (auto& tbl : ks.tbls) {
                if (tbl.tombstone) continue;
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
                emplace_back(row.values, S32(0));
                emplace_back(row.values, DynamicSet<AutoString8>{"compound"_as});
                emplace_back(row.values, S32(864000));
                emplace_back(row.values, table_uuid(ks.name, tbl.name));
                emplace_back(row.values, S32(2048));
                emplace_back(row.values, S32(0));
                emplace_back(row.values, S32(128));
                emplace_back(row.values, "BLOCKING"_as);
                emplace_back(row.values, F64(0.0));
                emplace_back(row.values, "99PERCENTILE"_as);
                emplace_back(vr.rows, move(row));
            }
        }

        return vr;
    }

    VirtualRows create_schema_columns(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "columns";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name",    create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"table_name",       create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"column_name",      create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"clustering_order", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"kind",             create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"position",         create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"type",             create_basic(BasicType::text)});

        for (auto& tbl : builtin_tables) {
            for (U64 ci = 0; ci < tbl.ncols; ci++) {
                auto& col = tbl.cols[ci];
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
            if (ks.tombstone) continue;
            for (auto& tbl : ks.tbls) {
                if (tbl.tombstone) continue;
                S32 pos = 0;
                for (U64 ci = 0; ci < tbl.cols.length; ci++) {
                    auto& col = tbl.cols[ci];
                    if (col.tombstone) continue;
                    bool is_pk = (ci == tbl.primary_col_idx);
                    VirtualRow row;
                    emplace_back(row.values, AutoString8(ks.name));
                    emplace_back(row.values, AutoString8(tbl.name));
                    emplace_back(row.values, AutoString8(col.name));
                    emplace_back(row.values, "none"_as);
                    emplace_back(row.values, is_pk ? "partition_key"_as : "regular"_as);
                    emplace_back(row.values, S32(is_pk ? 0 : pos++));
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
        vr.table = "views";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name",               create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"view_name",                   create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"base_table_id",               create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"base_table_name",             create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"bloom_filter_fp_chance",      create_basic(BasicType::double_)});
        emplace_back(vr.columns, VirtualColumn{"caching",                     create_map(BasicType::text, BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"comment",                     create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"compaction",                  create_map(BasicType::text, BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"compression",                 create_map(BasicType::text, BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"default_time_to_live",        create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"gc_grace_seconds",            create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"id",                          create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"include_all_columns",         create_basic(BasicType::boolean)});
        emplace_back(vr.columns, VirtualColumn{"max_index_interval",          create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"memtable_flush_period_in_ms", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"min_index_interval",          create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"read_repair",                 create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"read_repair_chance",          create_basic(BasicType::double_)});
        emplace_back(vr.columns, VirtualColumn{"speculative_retry",           create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"where_clause",                create_basic(BasicType::text)});

        // @todo support materialized views
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_indexes(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "indexes";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"table_name",    create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"index_name",    create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"kind",          create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"options",       create_map(BasicType::text, BasicType::text)});

        // @todo secondary indexes
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_triggers(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "triggers";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name",  create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"table_name",     create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"trigger_name",   create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"options",        create_map(BasicType::text, BasicType::text)});

        // @todo triggers
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_dropped_columns(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "dropped_columns";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"table_name",    create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"column_name",   create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"dropped_time",  create_basic(BasicType::timestamp)});
        emplace_back(vr.columns, VirtualColumn{"kind",          create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"type",          create_basic(BasicType::text)});

        // @todo track dropped columns
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_types(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "types";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"type_name",     create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"field_names",   create_list(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"field_types",   create_list(BasicType::text)});

        // @todo support UDTs
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_functions(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "functions";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name",        create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"function_name",        create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"argument_types",       create_list(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"argument_names",       create_list(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"body",                 create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"called_on_null_input", create_basic(BasicType::boolean)});
        emplace_back(vr.columns, VirtualColumn{"language",             create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"return_type",          create_basic(BasicType::text)});

        // @todo support UDFs
        (void)schema;
        return vr;
    }

    VirtualRows create_schema_aggregates(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "aggregates";

        emplace_back(vr.columns, VirtualColumn{"keyspace_name",   create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"aggregate_name",  create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"argument_types",  create_list(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"final_func",      create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"initcond",        create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"return_type",     create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"state_func",      create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"state_type",      create_basic(BasicType::text)});

        // @todo support UDAs
        (void)schema;
        return vr;
    }

    VirtualRows create_system_local() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table = "local";

        emplace_back(vr.columns, VirtualColumn{"key", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"bootstrapped", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"broadcast_address", create_basic(BasicType::inet)});
        emplace_back(vr.columns, VirtualColumn{"broadcast_port", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"cluster_name", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"cql_version", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"data_center", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"host_id", create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"listen_address", create_basic(BasicType::inet)});
        emplace_back(vr.columns, VirtualColumn{"listen_port", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"native_protocol_version", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"partitioner", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"rack", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"release_version", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"rpc_address", create_basic(BasicType::inet)});
        emplace_back(vr.columns, VirtualColumn{"rpc_port", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"schema_version", create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"tokens", create_set(BasicType::text)});

        auto create_loopback_ipv4 = []() {
            Blob b;
            push_back(b.value, U8(127)); push_back(b.value, U8(0));
            push_back(b.value, U8(0));   push_back(b.value, U8(1));
            return b;
        };

        VirtualRow row;
        emplace_back(row.values, "local"_as);
        emplace_back(row.values, "COMPLETED"_as);
        emplace_back(row.values, create_loopback_ipv4());  // broadcast_address
        emplace_back(row.values, S32(7000));
        emplace_back(row.values, "cql"_as);
        emplace_back(row.values, "3.4.7"_as);
        emplace_back(row.values, "datacenter1"_as);
        emplace_back(row.values, UUID{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}});
        emplace_back(row.values, create_loopback_ipv4());  // listen_address
        emplace_back(row.values, S32(7000));
        emplace_back(row.values, "4"_as);
        emplace_back(row.values, "org.apache.cassandra.dht.Murmur3Partitioner"_as);
        emplace_back(row.values, "rack1"_as);
        emplace_back(row.values, "3.11.19"_as); // @note last version in 3.x, before system_virtual
        emplace_back(row.values, create_loopback_ipv4());  // rpc_address
        emplace_back(row.values, S32(9042));
        emplace_back(row.values, UUID{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}});
        emplace_back(row.values, DynamicSet<AutoString8>{{"0"_as}});
        emplace_back(vr.rows, move(row));

        return vr;
    }

    VirtualRows create_system_peers() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table = "peers";

        emplace_back(vr.columns, VirtualColumn{"peer", create_basic(BasicType::inet)});
        emplace_back(vr.columns, VirtualColumn{"data_center", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"host_id", create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"preferred_ip", create_basic(BasicType::inet)});
        emplace_back(vr.columns, VirtualColumn{"rack", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"release_version", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"rpc_address", create_basic(BasicType::inet)});
        emplace_back(vr.columns, VirtualColumn{"schema_version", create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"tokens", create_set(BasicType::text)});

        return vr;
    }

    VirtualRows create_system_peers_v2() {
        VirtualRows vr;
        vr.keyspace = "system";
        vr.table = "peers_v2";

        emplace_back(vr.columns, VirtualColumn{"peer", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"peer_port", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"data_center", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"host_id", create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"native_address", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"native_port", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"preferred_ip", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"preferred_port", create_basic(BasicType::int_)});
        emplace_back(vr.columns, VirtualColumn{"rack", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"release_version", create_basic(BasicType::text)});
        emplace_back(vr.columns, VirtualColumn{"schema_version", create_basic(BasicType::uuid)});
        emplace_back(vr.columns, VirtualColumn{"tokens", create_basic(BasicType::text)});

        return vr;
    }
}
