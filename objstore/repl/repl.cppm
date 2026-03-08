module;
#include "macros.h"

export module objstore.repl;

import plexdb.base;
import plexdb.os;

import objstore.engine;
import objstore.parsers;

using namespace plexdb;

export namespace objstore::repl {
    void run(os::Handle in_handle, os::Handle out_handle, engine::Engine& eng);
    void run(engine::Engine& eng);
}
