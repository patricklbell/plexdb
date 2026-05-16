module;
#include <plexdb/macros/macros.h>

export module cql.repl;

import plexdb.base;
import plexdb.os;

import cql.engine;
import cql.parsers;

using namespace plexdb;

export namespace cql::repl {
    void run(os::Handle in_handle, os::Handle out_handle, engine::Engine& eng);
    void run(engine::Engine& eng);
}
