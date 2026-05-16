export module keyvalue.log;

import plexdb.base;
import plexdb.plugin;

export namespace keyvalue::log {
    void resp_error(const plexdb::String8& msg);
    void resp_info(const plexdb::String8& msg);

    // OTel database metrics (https://opentelemetry.io/docs/specs/semconv/db/database-metrics/)
    void db_query_text(const plexdb::String8& query);
    void db_connection_count(plexdb::S64 active_connections);
    void db_connection_max(plexdb::S64 max_connections);
    void db_operation_duration(plexdb::S64 microseconds);
    void db_response_returned_rows(plexdb::S64 row_count);
}
