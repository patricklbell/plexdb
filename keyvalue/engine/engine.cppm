export module keyvalue.engine;

import plexdb.base;
import plexdb.dynamic.containers;

import keyvalue.engine.statements;
import keyvalue.store;

using namespace plexdb;

export namespace keyvalue::engine {
    // @note response bytes are appended to `out`.
    // @note returns false if the connection should be closed (QUIT command).
    bool execute(store::Store& store, const Statement& statement, DynamicArray<U8>& out);
}
