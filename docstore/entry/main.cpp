#include "macros.h"

import plexdb.base;
import plexdb.os;
import plexdb.pager;
import plexdb.argparse;
import docstore.engine;

using namespace docstore;
using namespace plexdb;

void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    println("assert failed with message \"", msg, "\"");
    #if PLEXDB_DEBUG
        PLEXDB_TRAP;
    #else
        os::signal_exit(1);
    #endif
}

int main(int argc, char* argv[]) {
    os::signal_ignore_pipe();
    set_assert_handler(assert_handler);

    auto arg_parser = argparse::make_parser("docstore", "Document store database server");
    argparse::add_positional(arg_parser, "db_path", "Path to the database file");
    argparse::add_option(arg_parser, "--port", "-p", "TCP port to listen on", "27017");

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
    U16 port = u16_from_str(argparse::get_option(args, 0));
    if (port == 0) {
        println("Invalid port");
        return 1;
    }

    String8 pid_file_path = "docstore.pid";
    {
        os::File pid_file{os::file_open(pid_file_path)};
        AutoString8 pid_str = to_str(os::get_process_info()->pid);
        os::file_write(pid_file, {.start=0,.end=pid_str.length}, pid_str.c_str);
    }

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
        println("docstore server not yet implemented");
    }

    os::file_delete(pid_file_path);
    return 0;
}
