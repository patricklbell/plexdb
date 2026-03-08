export module objstore.log;

import plexdb.base;
import plexdb.log;

export namespace objstore::log {
    void cql_parse(plexdb::String8 query);
}
