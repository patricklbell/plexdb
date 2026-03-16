export module sqlstore.parsers;

import plexdb.base;
import sqlstore.engine.statements;

using namespace plexdb;

export namespace sqlstore::parsers::sql {
    Optional<Statement> parse(String8 input);
}
