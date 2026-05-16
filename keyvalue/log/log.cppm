export module keyvalue.log;

import plexdb.base;
import plexdb.plugin;

using namespace plexdb;

export namespace keyvalue::log {
    void resp_error(const String8& msg);
    void resp_info(const String8& msg);

    // OTel database metrics (https://opentelemetry.io/docs/specs/semconv/db/database-metrics/)
    void db_query_text(const String8& query);
    void db_connection_count(S64 active_connections);
    void db_connection_max(S64 max_connections);
    void db_operation_duration(S64 microseconds);
    void db_response_returned_rows(S64 row_count);
}
