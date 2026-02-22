#include <stdlib.h>
#include <signal.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;

import objstore.engine;
import objstore.server;

using namespace objstore;
using namespace plexdb;

void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    println(msg);
    exit(1);
}

int signal_pipe[2];
volatile bool exit_signal = false;

void signal_handler(int signal) {
    exit_signal = true;

    // unblocks blocking calls
    char dummy = 0;
    write(signal_pipe[1], &dummy, 1);
}

int init_signal_handlers() {
    int res = pipe(signal_pipe);
    assert_true(res == 0, "failed to create pipe");
    
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    res = sigaction(SIGUSR1, &sa, nullptr); assert_true(res == 0, "sigaction failed");
    res = sigaction(SIGTERM, &sa, nullptr); assert_true(res == 0, "sigaction failed");

    return signal_pipe[0];
}

int main(int argc, char* argv[]) {
    String8 pid_file_path = "objstore_server.pid";
    {
        os::File pid_file{os::file_open(pid_file_path)};

        AutoString8 pid_str = to_str(getpid());
        os::file_write(pid_file, (Rng1U64){0,pid_str.length}, pid_str.c_str);
    }

    set_assert_handler(assert_handler);
    int signal_fd = init_signal_handlers();
    
    int port = 8080;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // @todo detect sys info including permissions
    // @todo cli
    String8 db_path = "test.db";
    U64 page_size = 4_kb;

    bool db_create = !os::file_exists(db_path);
    os::File db_file{os::file_open(db_path)};

    if (db_create) {
        println("created new database \"", db_path, "\"");
        pager::create(db_file, page_size);
    }
    Pager pager{db_file};

    {
        if (db_create) {
            engine::create_database(pager);
        }
        engine::Engine engine{&pager};
    
        server::run(port, signal_fd, exit_signal, engine);
    }

    
    os::file_delete(pid_file_path);
    return 0;
}