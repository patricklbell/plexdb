export module objstore.log;

import plexdb.base;
import plexdb.log;

export namespace objstore::log {
    void cql_parse_query(plexdb::String8 query);
    void cql_parse_error(const char* text, plexdb::U64 len);
}
