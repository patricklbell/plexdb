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


    VirtualRows make_schema_keyspaces(schema::Schema& schema) {
        VirtualRows vr;
        vr.keyspace = "system_schema";
        vr.table = "keyspaces";

        push_back(vr.columns, VirtualColumn{"keyspace_name",  types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"durable_writes", types::make_native(types::boolean)});
        push_back(vr.columns, VirtualColumn{"replication",    types::make_map(types::text, types::text)});

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

    VirtualRows make_schema_views(schema::Schema& /*schema*/) {
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
        push_back(vr.columns, VirtualColumn{"id",                         types::make_native(types::uuid)});
        push_back(vr.columns, VirtualColumn{"include_all_columns",         types::make_native(types::boolean)});
        push_back(vr.columns, VirtualColumn{"max_index_interval",          types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"memtable_flush_period_in_ms", types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"min_index_interval",          types::make_native(types::int_)});
        push_back(vr.columns, VirtualColumn{"read_repair",                 types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"read_repair_chance",          types::make_native(types::double_)});
        push_back(vr.columns, VirtualColumn{"speculative_retry",           types::make_native(types::text)});
        push_back(vr.columns, VirtualColumn{"where_clause",                types::make_native(types::text)});

        // @todo support materialized views
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
