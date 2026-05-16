module keyvalue.log;

import plexdb.base;
import plexdb.plugin;

namespace keyvalue::log {
    plexdb::plugin::Producer resp_protocol_producer{"resp::protocol"};

    void resp_error(const plexdb::String8& msg) {
        plexdb::plugin::message(resp_protocol_producer, plexdb::plugin::Level::Error, msg);
    }

    void resp_info(const plexdb::String8& msg) {
        plexdb::plugin::message(resp_protocol_producer, plexdb::plugin::Level::Info, msg);
    }

    plexdb::plugin::Producer otlp_db_producer{"otlp.db"};

    void db_query_text(const plexdb::String8& query) {
        plexdb::plugin::message(otlp_db_producer, plexdb::plugin::Level::Debug, query, "query.text");
    }

    plexdb::plugin::Stat stat_connection_count{&otlp_db_producer, "client.connection.count", plexdb::plugin::StatType::Gauge};
    plexdb::plugin::Stat stat_connection_max{&otlp_db_producer, "client.connection.max", plexdb::plugin::StatType::Gauge};
    plexdb::plugin::Stat stat_operation_duration{&otlp_db_producer, "client.operation.duration", plexdb::plugin::StatType::Gauge};
    plexdb::plugin::Stat stat_returned_rows{&otlp_db_producer, "client.response.returned_rows", plexdb::plugin::StatType::Gauge};

    void db_connection_count(plexdb::S64 active_connections) {
        plexdb::plugin::stat(stat_connection_count, active_connections);
    }

    void db_connection_max(plexdb::S64 max_connections) {
        plexdb::plugin::stat(stat_connection_max, max_connections);
    }

    void db_operation_duration(plexdb::S64 microseconds) {
        plexdb::plugin::stat(stat_operation_duration, microseconds);
    }

    void db_response_returned_rows(plexdb::S64 row_count) {
        plexdb::plugin::stat(stat_returned_rows, row_count);
    }
}
