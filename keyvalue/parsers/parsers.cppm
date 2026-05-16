export module keyvalue.parsers;

import plexdb.base;
import plexdb.dynamic.containers;
import keyvalue.engine.statements;

using namespace plexdb;

export namespace keyvalue::parsers {
    enum class ParseResult { Ok, Incomplete, Error };

    Pair<ParseResult, Statement> parse(const U8* data, U64 length, U64* consumed);
}
