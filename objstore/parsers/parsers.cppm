export module objstore.parsers;

import plexdb.base;
import plexdb.log;
import plexdb.tagged_union;
import objstore.engine.dtype;
import objstore.engine.statements;
import objstore.log;

using namespace plexdb;

namespace objstore::parsers {
    // ========================================================================
    // cassandra query language (CQL)
    // ========================================================================
    namespace cql {
        export Optional<Statement> parse(String8 bytes, void(*error_fn)(const String8& error) = &log::cql_parse_error);
    }
}
