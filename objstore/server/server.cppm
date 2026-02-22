export module objstore.server;

import plexdb.base;

import objstore.engine;

export namespace objstore::server {
    void run(int port, int signal_fd, volatile bool& should_exit, engine::Engine& engine);
}
