module objstore.log;

import plexdb.base;
import plexdb.log;

namespace objstore::log {
    plexdb::log::Producer producer{"objstore::parser::cql"};

    void cql_parse_query(plexdb::String8 query) {
        plexdb::log::fire_message(
            producer.id,
            reinterpret_cast<const char*>(query.data),
            query.length
        );
    }

    void cql_parse_error(const char* text, plexdb::U64 len) {
        plexdb::log::fire_message(
            producer.id,
            text,
            len
        );
    }
}
