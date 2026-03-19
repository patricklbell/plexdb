export module objstore.parsers;

import plexdb.base;
import plexdb.log;
import plexdb.tagged_union;
import objstore.engine.dtype;
import objstore.engine.statements;

using namespace plexdb;

namespace objstore::parsers {
    // ========================================================================
    // cassandra query language (CQL)
    // ========================================================================
    namespace cql {
        export Optional<Statement> parse(String8 bytes, bool report_errors=log::enabled);
        export Optional<Statement> parse(String8 bytes, void(*error_fn)(const char*, size_t));
    }
}
