export module docstore.parsers;

import plexdb.base;
import docstore.engine.statements;

using namespace plexdb;

export namespace docstore::parsers::mql {
    Optional<Statement> parse(String8 input);
}
