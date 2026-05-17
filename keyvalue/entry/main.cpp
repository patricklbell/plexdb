#include <plexdb/macros/macros.h>
#include <plexdb/support/tracy/tracy.hpp>
#include <coroutine>
#include <signal.h>

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.argparse;
import plexdb.tagged_union;
import plexdb.threads;
import plexdb.arena;

import keyvalue.engine;
import keyvalue.resp;

using namespace plexdb;
using namespace keyvalue;

os::Notifier g_signal_notifier{};
static volatile sig_atomic_t g_signal_count = 0;

static void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number);
static void signal_handler(int);

int main(int argc, char* argv[]) {
    threads::Context main_thread_ctx{.arenas={},.is_main=true};
    threads::equip(&main_thread_ctx);

    set_assert_handler(assert_handler);

    os::signal_ignore_pipe();
    os::signal_ignore_reload();
    os::signal_register_kill(signal_handler);

    auto arg_parser = argparse::create_parser("resp", "Redis serialization protocol (RESP) compatible key-value server");

    U64 port_arg      = argparse::add_option(arg_parser, "--port",      "-p", "TCP port to listen on", "6379");
    U64 no_uring_arg  = argparse::add_flag(arg_parser,   "--no-uring",  "-U", "Disable io_uring and use synchronous socket I/O");
    U64 in_memory_arg = argparse::add_flag(arg_parser,   "--in-memory", "",   "Run without persistence (no database file)");
    U64 path_arg      = argparse::add_positional(arg_parser, "path", "Database file path (required without --in-memory)");

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

    bool in_memory = argparse::has_flag(args, in_memory_arg);
    String8 db_path = argparse::get_positional(args, path_arg);

    if (!in_memory && db_path.length == 0) {
        println("error: database path required (or use --in-memory)");
        argparse::print_help(arg_parser);
        return 1;
    }

    U16 port = u16_from_str(argparse::get_option(args, port_arg));
    bool no_uring = argparse::has_flag(args, no_uring_arg);
    if (port == 0) {
        println("Invalid port");
        return 1;
    }

    os::Poll io_poll{};
    auto on_ready = [&port]() {
        println("listening on port ", to_str(port), " (RESP)");
    };
    auto signal_consumer = aio::create_notifier_consumer(g_signal_notifier, io_poll);

    if (in_memory) {
        engine::InMemoryEngine eng{};
        Optional<String8> err = resp::run(port, eng, on_ready, !no_uring, signal_consumer, io_poll);
        if (err) println(*err);
    } else {
        assert_not_implemented("persistent engine");
    }

    println("shutting down");
    return 0;
}

static void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    println("Assert failed \"", msg, "\" at ", function_name, " in ", file_name, ":", to_str(line_number));
#if PLEXDB_DEBUG
    PLEXDB_TRAP;
#else
    os::process_exit(1);
#endif
}

static void signal_handler(int) {
    g_signal_count += 1;
    if (g_signal_count > 1) os::process_exit(1);
    os::signal_notify_safe(g_signal_notifier);
}
