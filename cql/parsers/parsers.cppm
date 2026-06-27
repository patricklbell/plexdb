export module cql.parsers;

import plexdb.base;
import plexdb.plugin;
import plexdb.tagged_union;
import cql.engine.types;
import cql.engine.statements;
import cql.log;

using namespace plexdb;

export namespace cql::parsers {
    struct ParseResult {
        Optional<Statement> statement;
        AutoString8         err;
    };

    ParseResult parse(String8 bytes);
}
