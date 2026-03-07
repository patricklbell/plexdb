import plexdb.base;
import plexdb.os;
import plexdb.pager;
import plexdb.argparse;

import objstore.engine;
import objstore.repl;
import objstore.server;

using namespace objstore;
using namespace plexdb;

void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    println(msg);
    os::signal_exit(1);
}

os::Notifier g_signal_notifier{};
volatile bool g_should_stop = false;

void signal_handler(int) {
    g_should_stop = true;
    os::signal_notify_safe(g_signal_notifier);
}

int main(int argc, char* argv[]) {
    os::signal_ignore_pipe();
    set_assert_handler(assert_handler);
    os::signal_register_kill(signal_handler);

    auto arg_parser = argparse::make_parser("objstore_server", "Object store database server");
    argparse::add_positional(arg_parser, "db_path", "Path to the database file");
    argparse::add_positional(arg_parser, "port", "TCP port to listen on");
    argparse::add_flag(arg_parser, "--repl", "-r", "Run interactive REPL instead of HTTP server");

    auto args = argparse::parse(arg_parser, argc, argv);
    if (args.help_requested) {
        argparse::print_help(arg_parser);
        return 0;
    }
    if (!args.ok) {
        println(args.error);
        argparse::print_help(arg_parser);
        return 1;
    }

    String8 db_path = argparse::get_positional(args, 0);
    U16 port = u16_from_str(argparse::get_positional(args, 1));
    bool run_repl = argparse::has_flag(args, 0);

    U64 page_size = 4_kb;
    bool db_create = !os::file_exists(db_path);
    os::File db_file{os::file_open(db_path)};

    if (db_create) {
        println("created new database \"", db_path, "\"");
        pager::create(db_file, page_size);
    }
    Pager pager{db_file};

    String8 pid_file_path = "objstore_server.pid";
    {
        os::File pid_file{os::file_open(pid_file_path)};
        AutoString8 pid_str = to_str(os::get_process_info()->pid);
        os::file_write(pid_file, {.start=0,.end=pid_str.length}, pid_str.c_str);
    }

    {
        if (db_create) {
            engine::create_database(pager);
        }
        engine::Engine engine{&pager};
    
        if (run_repl) {
            repl::run(engine);
        } else {
            server::run(port, g_signal_notifier, g_should_stop, engine, [&port]() { println("listening on port ", to_str(port));});
            println("shutting down...");
        }
    }

    os::file_delete(pid_file_path);
    return 0;
}
