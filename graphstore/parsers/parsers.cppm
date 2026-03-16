export module graphstore.parsers;

import plexdb.base;
import graphstore.engine.statements;

using namespace plexdb;

export namespace graphstore::parsers::sparql {
    Optional<Statement> parse(String8 input);
}
