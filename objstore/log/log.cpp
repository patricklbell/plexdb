module objstore.log;

import plexdb.base;
import plexdb.log;

namespace objstore::log {
    plexdb::log::Producer objstore_parser_cql_producer{"objstore::parser::cql"};

    void cql_parse_error(const plexdb::String8& error) {
        plexdb::log::message(objstore_parser_cql_producer, plexdb::log::Level::Error, error);
    }

    plexdb::log::Producer otlp_db_producer{"otlp.db"};

    void db_query_text(const plexdb::String8& query) {
        plexdb::log::message(otlp_db_producer, plexdb::log::Level::Debug, query, "query.text");
    }

    plexdb::log::Stat stat_connection_count{&otlp_db_producer, "client.connection.count", plexdb::log::StatType::Gauge};
    plexdb::log::Stat stat_connection_max{&otlp_db_producer, "client.connection.max", plexdb::log::StatType::Gauge};
    plexdb::log::Stat stat_operation_duration{&otlp_db_producer, "client.operation.duration", plexdb::log::StatType::Gauge};
    plexdb::log::Stat stat_returned_rows{&otlp_db_producer, "client.response.returned_rows", plexdb::log::StatType::Gauge};

    void db_connection_count(plexdb::S64 active_connections) {
        plexdb::log::stat(stat_connection_count, active_connections);
    }

    void db_connection_max(plexdb::S64 max_connections) {
        plexdb::log::stat(stat_connection_max, max_connections);
    }

    void db_operation_duration(plexdb::S64 microseconds) {
        plexdb::log::stat(stat_operation_duration, microseconds);
    }

    void db_response_returned_rows(plexdb::S64 row_count) {
        plexdb::log::stat(stat_returned_rows, row_count);
    }
}
