#include <plexdb/macros/macros.h>
#include <plexdb/support/tracy/tracy.hpp>
#include <coroutine>
#include <signal.h>

import plexdb.base;
import plexdb.coroutine;
import plexdb.debug;
import plexdb.os;
import plexdb.os.uring;
import plexdb.tagged_union;
import plexdb.pager;
import plexdb.argparse;
import plexdb.threads;
import plexdb.arena;
import plexdb.aio;

import cql.engine;
import cql.native;

using namespace cql;
using namespace cql::column_types;
using namespace plexdb;

os::Notifier                 g_signal_notifier{};
static volatile sig_atomic_t g_signal_count = 0;

static void                                         assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number);
static void                                         crash_handler(int signal_number);
static void                                         signal_handler(int);
static Optional<String8>                            load_credentials(String8 path, native::ServerConfig& config);
static Pair<aio::FileIOContext, aio::EventConsumer> create_file_io_context(TaggedUnion<uring::Ring, os::AIOContext>& resource, arena::Arena& arena, os::Poll& poll, uring::IOBudgetOverride file_budget_override = {});

int main(int argc, char* argv[]) {
    plexdb::threads::Context main_thread_ctx{.arenas = {}, .is_main = true};
    plexdb::threads::equip(&main_thread_ctx);

    set_assert_handler(assert_handler);

    os::signal_register_crash(crash_handler);
    os::signal_register_kill(signal_handler);
    os::signal_ignore_pipe();
    os::signal_ignore_reload();

    auto arg_parser = argparse::create_parser("cql", "Object store database server");

    U64 db_path_arg             = argparse::add_positional(arg_parser, "db_path", "Path to the database file");
    U64 port_arg                = argparse::add_option(arg_parser, "--port", "-p", "TCP port to listen on", "9042");
    U64 checkpoint_interval_arg = argparse::add_option(arg_parser, "--checkpoint-interval", "-c", "Maximum number of transactions held in-memory before writing to database", "1000");
    U64 no_uring_arg            = argparse::add_flag(arg_parser, "--no-uring", "-U", "Disable io_uring and use synchronous socket I/O");
    U64 no_wal_arg              = argparse::add_flag(arg_parser, "--no-wal", "-W", "Disable write-ahead-log (WAL)");
    U64 auth_file_arg           = argparse::add_option(arg_parser, "--auth-file", "-a", "Enable PasswordAuthenticator with credentials read from a file of 'username:password' lines", "");

    U64 net_buffer_size_arg   = argparse::add_option(arg_parser, "--net-buffer-size", "", "Override io_uring network ring buffer size in bytes");
    U64 net_buffer_count_arg  = argparse::add_option(arg_parser, "--net-buffer-count", "", "Override io_uring network ring buffer count");
    U64 net_queue_depth_arg   = argparse::add_option(arg_parser, "--net-queue-depth", "", "Override io_uring network ring queue depth");
    U64 file_buffer_size_arg  = argparse::add_option(arg_parser, "--file-buffer-size", "", "Override io_uring file ring buffer size in bytes");
    U64 file_buffer_count_arg = argparse::add_option(arg_parser, "--file-buffer-count", "", "Override io_uring file ring buffer count");
    U64 file_queue_depth_arg  = argparse::add_option(arg_parser, "--file-queue-depth", "", "Override io_uring file ring queue depth");

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
    bool    no_uring            = argparse::has_flag(args, no_uring_arg);
    bool    no_wal              = argparse::has_flag(args, no_wal_arg);
    if (port == 0) {
        println("Invalid port");
        return 1;
    }

    auto opt_u64_option = [&args](U64 option_arg) -> Optional<U64> {
        String8 value = argparse::get_option(args, option_arg);
        return value.length > 0 ? Optional<U64>{u64_from_str(value)} : Optional<U64>{};
    };
    auto opt_u32_option = [&args](U64 option_arg) -> Optional<U32> {
        String8 value = argparse::get_option(args, option_arg);
        return value.length > 0 ? Optional<U32>{u32_from_str(value)} : Optional<U32>{};
    };

    uring::IOBudgetOverride network_budget_override{
        .buffer_size  = opt_u64_option(net_buffer_size_arg),
        .buffer_count = opt_u32_option(net_buffer_count_arg),
        .queue_depth  = opt_u32_option(net_queue_depth_arg),
    };
    uring::IOBudgetOverride file_budget_override{
        .buffer_size  = opt_u64_option(file_buffer_size_arg),
        .buffer_count = opt_u32_option(file_buffer_count_arg),
        .queue_depth  = opt_u32_option(file_queue_depth_arg),
    };

    native::ServerConfig server_config{};
    if (String8 auth_file_path = argparse::get_option(args, auth_file_arg); auth_file_path.length > 0) {
        Optional<String8> err = load_credentials(auth_file_path, server_config);
        if (err) {
            println(*err);
            return 1;
        }
        println("authentication enabled for ", to_str(server_config.credentials.length), " user(s)");
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
        auto [file_io_ctx, file_io_consumer] = create_file_io_context(file_io_resource, *scratch.arena, io_poll, file_budget_override);

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

        auto on_ready = [&port]() {
            println("listening on port ", to_str(port));
        };
        auto              signal_consumer = aio::create_notifier_consumer(g_signal_notifier, io_poll);
        Optional<String8> err             = native::run(
            port, engine, on_ready, !no_uring,
            file_io_consumer, signal_consumer, io_poll, network_budget_override, server_config
        );
        if (err) {
            println(*err);
        }

        aio::drive(pager::destroy(pager), file_io_consumer, io_poll);
    }

    println("shutting down");

    os::file_delete(pid_file_path);
    return 0;
}

static void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    debug::write_stderr("Assert failed \"%s\" at %s in %s:%u\n", msg, function_name, file_name, line_number);
    debug::write_stack_stderr(1);
    debug::write_async_stack_stderr();
#if PLEXDB_DEBUG
    // @note the trap must reach the debugger or core dump, not the crash handler
    os::signal_reset_crash();
    PLEXDB_TRAP;
#else
    os::process_exit(1);
#endif
}

static void crash_handler(int signal_number) {
    // @note protects from deadlock if the crash is inside the handler
    os::process_exit_after(5);
    debug::write_stderr("Fatal signal %d\n", signal_number);
    debug::write_stack_stderr(1);
    debug::write_async_stack_stderr();
    // @note the crash handler is one-shot, so this takes the default action even if the signal
    // was sent rather than raised by a fault
    os::signal_raise(signal_number);
}

static void signal_handler(int) {
    g_signal_count += 1;
    if (g_signal_count > 1) {
        os::process_exit(1);
    }
    os::signal_notify_safe(g_signal_notifier);
}

static Pair<aio::FileIOContext, aio::EventConsumer> create_file_io_context(TaggedUnion<uring::Ring, os::AIOContext>& resource, arena::Arena& arena, os::Poll& poll, uring::IOBudgetOverride file_budget_override) {
    auto* rs = uring::get_ring_settings();
    if (rs->available) {
        U64                 page_size = os::get_system_info()->page_size;
        uring::FileIOBudget budget    = uring::compute_file_io_budget(page_size, file_budget_override);
        if (budget.buffer_count > 0 && budget.buffer_size > 0) {
            uring::Ring ring{os::zero_handle(), budget.queue_depth, budget.buffer_size, budget.buffer_count};
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

    return {
        aio::create_sync_file_io_context(),
        aio::EventConsumer{0, aio::OnUnblockFunctor{[](const TArrayView<os::PollEvent>&) -> bool {
                               return true;
                           }}}
    };
}

// One credential per line as `username:password`; blank lines and lines starting with '#' are skipped.
static Optional<String8> load_credentials(String8 path, native::ServerConfig& config) {
    if (!os::file_exists(path)) {
        return {"auth file does not exist"};
    }
    os::File         file{os::file_open(path, os::AccessFlags(os::READ))};
    U64              size = os::file_get_stats(file).byte_count;
    DynamicArray<U8> bytes{};
    resize(bytes, size);
    if (size > 0) {
        os::file_read(file, {.start = 0, .end = size}, bytes.ptr);
    }

    U64 line_start = 0;
    for (U64 i = 0; i <= size; i++) {
        if (i < size && bytes[i] != '\n') {
            continue;
        }
        U64 line_end = i;
        if (line_end > line_start && bytes[line_end - 1] == '\r') {
            line_end--;
        }
        String8 line(reinterpret_cast<const char*>(bytes.ptr + line_start), line_end - line_start);
        line_start = i + 1;
        if (line.length == 0 || line.data[0] == '#') {
            continue;
        }
        Optional<U64> colon = find(line, ':');
        if (!colon || *colon == 0) {
            return {"auth file lines must be 'username:password'"};
        }
        push_back(config.credentials, native::AuthCredential{
            .username = AutoString8(String8(line.data, *colon)),
            .password = AutoString8(String8(line.data + *colon + 1, line.length - *colon - 1)),
        });
    }
    if (config.credentials.length == 0) {
        return {"auth file contains no credentials"};
    }
    return {};
}
