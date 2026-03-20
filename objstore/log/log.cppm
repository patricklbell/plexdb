export module objstore.log;

import plexdb.base;
import plexdb.log;

export namespace objstore::log {
    void db_query_text(const plexdb::String8& query);
    void cql_parse_error(const plexdb::String8& error);

    // OTel database metrics (https://opentelemetry.io/docs/specs/semconv/db/database-metrics/)
    void db_connection_count(plexdb::S64 active_connections);
    void db_connection_max(plexdb::S64 max_connections);
    void db_operation_duration(plexdb::S64 microseconds);
    void db_response_returned_rows(plexdb::S64 row_count);
}
