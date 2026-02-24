export module objstore.repl;

import plexdb.base;
import plexdb.os;

import objstore.engine;
import objstore.parser;

using namespace plexdb;

export namespace objstore::repl {
    // Run an interactive CQL REPL, reading from in_fd and writing to out_fd.
    // Returns when EOF is read from in_fd.
    void run(int in_fd, int out_fd, engine::Engine& engine);
}
