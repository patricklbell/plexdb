export module cql.parsers;

import plexdb.base;
import plexdb.plugin;
import plexdb.tagged_union;
import cql.engine.types;
import cql.engine.statements;
import cql.log;

using namespace plexdb;

namespace cql::parsers {
    export Optional<Statement> parse(String8 bytes, void (*error_fn)(const String8& error) = &log::cql_parse_error);

    export Optional<String8> check_specific_errors(String8 query);
}
