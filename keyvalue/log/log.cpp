module keyvalue.log;

import plexdb.base;
import plexdb.plugin;

namespace keyvalue::log {
    plugin::Producer resp_protocol_producer{"resp::protocol"};

    void resp_error(const String8& msg) {
        plugin::message(resp_protocol_producer, plugin::Level::Error, msg);
    }

    void resp_info(const String8& msg) {
        plugin::message(resp_protocol_producer, plugin::Level::Info, msg);
    }

    plugin::Producer otlp_db_producer{"otlp.db"};

    void db_query_text(const String8& query) {
        plugin::message(otlp_db_producer, plugin::Level::Debug, query, "query.text");
    }

    plugin::Stat stat_connection_count{&otlp_db_producer, "client.connection.count", plugin::StatType::Gauge};
    plugin::Stat stat_connection_max{&otlp_db_producer, "client.connection.max", plugin::StatType::Gauge};
    plugin::Stat stat_operation_duration{&otlp_db_producer, "client.operation.duration", plugin::StatType::Gauge};
    plugin::Stat stat_returned_rows{&otlp_db_producer, "client.response.returned_rows", plugin::StatType::Gauge};

    void db_connection_count(S64 active_connections) {
        plugin::stat(stat_connection_count, active_connections);
    }

    void db_connection_max(S64 max_connections) {
        plugin::stat(stat_connection_max, max_connections);
    }

    void db_operation_duration(S64 microseconds) {
        plugin::stat(stat_operation_duration, microseconds);
    }

    void db_response_returned_rows(S64 row_count) {
        plugin::stat(stat_returned_rows, row_count);
    }
}
