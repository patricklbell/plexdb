export module keyvalue.parsers;

import plexdb.base;

import keyvalue.engine.statements;

using namespace plexdb;

export namespace keyvalue::parsers {
    Optional<Statement> parse(const U8* data, U64 length, U64* consumed);
}
