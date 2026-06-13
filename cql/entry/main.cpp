#include <plexdb/macros/macros.h>
#include <plexdb/support/tracy/tracy.hpp>
#include <coroutine>
#include <signal.h>

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;
import plexdb.os.uring;
import plexdb.tagged_union;
import plexdb.pager;
import plexdb.argparse;
import plexdb.threads;
import plexdb.arena;
import plexdb.aio;

import cql.engine;
import cql.repl;
import cql.native;

using namespace cql;
using namespace plexdb;

os::Notifier                 g_signal_notifier{};
static volatile sig_atomic_t g_signal_count = 0;

static void                                         assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number);
static void                                         signal_handler(int);
static Pair<aio::FileIOContext, aio::EventConsumer> create_file_io_context(bool force_sync, TaggedUnion<uring::Ring, os::AIOContext>& resource, arena::Arena& arena, os::Poll& poll);

int main(int argc, char* argv[]) {
    plexdb::threads::Context main_thread_ctx{.arenas = {}, .is_main = true};
    plexdb::threads::equip(&main_thread_ctx);

    set_assert_handler(assert_handler);

    os::signal_ignore_pipe();
    os::signal_ignore_reload();
    os::signal_register_kill(signal_handler);

    auto arg_parser = argparse::create_parser("cql", "Object store database server");

    U64 db_path_arg             = argparse::add_positional(arg_parser, "db_path", "Path to the database file");
    U64 port_arg                = argparse::add_option(arg_parser, "--port", "-p", "TCP port to listen on", "9042");
    U64 checkpoint_interval_arg = argparse::add_option(arg_parser, "--checkpoint-interval", "-c", "Maximum number of transactions held in-memory before writing to database", "1000");
    U64 repl_arg                = argparse::add_flag(arg_parser, "--repl", "-r", "Run interactive REPL instead of server");
    U64 no_uring_arg            = argparse::add_flag(arg_parser, "--no-uring", "-U", "Disable io_uring and use synchronous socket I/O");
    U64 no_wal_arg              = argparse::add_flag(arg_parser, "--no-wal", "-W", "Disable write-ahead-log (WAL)");

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

    String8 db_path             = argparse::get_positional(args, db_path_arg);
    U16     port                = u16_from_str(argparse::get_option(args, port_arg));
    U64     checkpoint_interval = u64_from_str(argparse::get_option(args, checkpoint_interval_arg));
    bool    run_repl            = argparse::has_flag(args, repl_arg);
    bool    no_uring            = argparse::has_flag(args, no_uring_arg);
    bool    no_wal              = argparse::has_flag(args, no_wal_arg);
    if (port == 0) {
        println("Invalid port");
        return 1;
    }

    String8 pid_file_path = "plexdb_cql_server.pid";
    {
        os::File    pid_file{os::file_open(pid_file_path, os::AccessFlags(os::READ | os::WRITE | os::TRUNCATE))};
        AutoString8 pid_str = to_str(os::get_process_info()->pid);
        os::file_write(pid_file, {.start = 0, .end = pid_str.length}, pid_str.c_str);
    }

    os::Poll io_poll{};
    {
        threads::Scope                           scratch = threads::scratch();
        TaggedUnion<uring::Ring, os::AIOContext> file_io_resource;
        auto [file_io_ctx, file_io_consumer] = create_file_io_context(run_repl, file_io_resource, *scratch.arena, io_poll);

        Optional<os::File> opt_wal_file;
        os::File           db_file{os::file_open(db_path)};

        U64  page_size = 4_kb;
        bool db_create = os::file_get_stats(db_file).byte_count == 0;

        Engine engine;
        engine.port = port;
        Pager pager;

        auto initialize_database = [&]() -> coroutine::Task<> {
            if (db_create) {
                // @todo make pager creation safe?
                auto header = co_await pager::create(file_io_ctx, db_file, page_size);
                if (no_wal) {
                    pager = Pager{&file_io_ctx, static_cast<os::Handle>(db_file), header, checkpoint_interval};
                } else {
                    opt_wal_file = os::File{os::file_open(db_path + ".wal"_as)};
                    co_await pager::init(pager, &file_io_ctx, db_file, static_cast<os::Handle>(*opt_wal_file), header);
                }

                {
                    pager::Transaction tx{&pager};
                    co_await tx.begin();
                    co_await engine::create_database(pager);
                    co_await tx.commit();
                }

                println("created new database \"", db_path, "\"");
            } else {
                if (no_wal) {
                    co_await pager::init(pager, &file_io_ctx, db_file);
                } else {
                    opt_wal_file = os::File{os::file_open(db_path + ".wal"_as)};
                    co_await pager::init(pager, &file_io_ctx, db_file, static_cast<os::Handle>(*opt_wal_file));
                }
            }

            co_await engine::init(engine, &pager);
        };

        aio::drive(initialize_database(), file_io_consumer, io_poll);

        if (run_repl) {
            repl::run(engine);
        } else {
            auto on_ready = [&port, no_uring]() {
                println("listening on port ", to_str(port), no_uring ? " (native protocol, async sockets)" : " (native protocol)");
            };
            auto              signal_consumer = aio::create_notifier_consumer(g_signal_notifier, io_poll);
            Optional<String8> err             = native::run(
                port, engine, on_ready, !no_uring,
                file_io_consumer, signal_consumer, io_poll);
            if (err) {
                println(*err);
            }
        }

        aio::drive(pager::destroy(pager), file_io_consumer, io_poll);
    }

    println("shutting down");

    os::file_delete(pid_file_path);
    return 0;
}

// @todo
#if PLEXDB_OS_LINUX
#include <stdio.h>
#include <unistd.h>
#endif

static void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    // Write to stderr (unbuffered) so the message is visible even when the
    // process is about to crash via PLEXDB_TRAP.
    char buf[512];
    int  n = snprintf(buf, sizeof(buf), "Assert failed \"%s\" at %s in %s:%u\n",
                      msg, function_name, file_name, line_number);
    if (n > 0) {
        write(2, buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1);
    }

#if PLEXDB_DEBUG
    PLEXDB_TRAP;
#else
    os::process_exit(1);
#endif
}

static void signal_handler(int) {
    g_signal_count += 1;
    if (g_signal_count > 1) {
        os::process_exit(1);
    }
    os::signal_notify_safe(g_signal_notifier);
}

static Pair<aio::FileIOContext, aio::EventConsumer> create_file_io_context(bool force_sync, TaggedUnion<uring::Ring, os::AIOContext>& resource, arena::Arena& arena, os::Poll& poll) {
    if (!force_sync) {
        auto* rs = uring::get_ring_settings();
        if (rs->recommended) {
            U64                   page_size = os::get_system_info()->page_size;
            uring::GlobalIOBudget budget    = uring::compute_io_budget(page_size);
            if (budget.file_buffer_count > 0 && budget.file_buffer_size > 0) {
                uring::Ring ring{os::zero_handle(), budget.file_queue_depth, budget.file_buffer_size, budget.file_buffer_count};
                if (ring && !os::is_zero_handle(ring.event_fd)) {
                    resource = plexdb::move(ring);
                    return aio::create_uring_async_file_io_context(&get<uring::Ring>(resource), arena, poll);
                }
            }
        }

        os::Handle notifier = os::aio_notifier_create();
        if (!os::is_zero_handle(notifier)) {
            os::AIOContext aio_ctx{128, notifier};
            if (aio_ctx) {
                resource = plexdb::move(aio_ctx);
                return aio::create_aio_async_file_io_context(&get<os::AIOContext>(resource), arena, poll);
            }
        }
    }

    return {
        aio::create_sync_file_io_context(),
        aio::EventConsumer{0, aio::OnUnblockFunctor{[](const TArrayView<os::PollEvent>&) -> bool { return true; }}}
    };
}
