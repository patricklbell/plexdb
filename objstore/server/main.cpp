#include <stdlib.h>
#include <signal.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;

import objstore.tcp;
import objstore.parser;
import objstore.engine;

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


void static return_execution_result(tcp::Request& req, engine::ExecutionResult result, const String8& value) {
    switch (result) {
        case engine::ExecutionResult::BadRequest:{
            tcp::return_http_fail(req, 400, value);
        }break;
        case engine::ExecutionResult::NotImplemented:{
            tcp::return_http_fail(req, 501, value);
        }
        case engine::ExecutionResult::Success:{
            tcp::return_http_success(req, value);
        }
    }
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
    
        tcp::Pool pool{port, signal_fd};
        tcp::Stats stats{};
        
        // fd -> parser state
        MapFixedSentinel<int, parser::http::Parser, 2*tcp::MAX_CONNECTIONS> req_id_to_http_parser;
    
        println("listening on port ", to_str(port));
    
        const auto on_chunk = [&stats, &engine, &req_id_to_http_parser](tcp::Request& req) -> tcp::RequestStatus {
            parser::http::Parser& http_parser = find_or_insert(req_id_to_http_parser, req.id);
            
            // @note assumes last chunk is always the only new chunk
            tcp::Chunk& chunk = *back(*req.chunks);
            execute(http_parser, chunk.data.ptr, chunk.data.length);
            
            if (parser::http::has_error(http_parser)) {
                tcp::return_http_fail(req, 400, "Bad request: Malformed HTTP", /*close*/ true);

                reset(http_parser);
                return tcp::RequestStatus::Close;
            }
            if (!parser::http::is_complete(http_parser)) {
                return tcp::RequestStatus::Pending;
            }
            
            const parser::http::Request& http_req = get_request(http_parser);

            // @todo multi statement request
            auto cql_opt = parser::cql::parse(http_req.body);
            if (!cql_opt) {
                // @todo return parse error message?
                tcp::return_http_fail(req, 400, "Bad request: Failed to parse CQL");

                reset(http_parser);
                return tcp::RequestStatus::Handled;
            }
    
            // @todo use scratchpad for http, in future this should be write buffer itself to avoid copying,
            // this requires chunked encoding
            constexpr U64 SCRATCHPAD_SIZE = 4_kb;
            Array<U8, SCRATCHPAD_SIZE> scratchpad;
            engine::ExecutionContext execution_ctx = {
                .write_buffer = TArrayView<U8>(scratchpad.values, SCRATCHPAD_SIZE),
            };

            AutoString8 value{""};
            const auto flush = [&execution_ctx,&value](U64 length) {
                // @todo chunked encoding for large responses with streaming
                append(value, (char*)execution_ctx.write_buffer.ptr, (char*)execution_ctx.write_buffer.ptr + length);
            };
            
            engine::ExecutionResult exec_result = engine::execute(engine, *cql_opt, flush, execution_ctx);
            return_execution_result(req, exec_result, value);

            reset(http_parser);
            return tcp::RequestStatus::Handled;
        };

        const auto on_close = [&req_id_to_http_parser](int req_id) {
            try_remove(req_id_to_http_parser, req_id);
        };
        
        // @todo: add connection close hook to tcp layer to clean up parser state

        tcp::listen(on_chunk, on_close, pool, stats, exit_signal);

        println("shutting down...");
    }

    
    os::file_delete(pid_file_path);
    return 0;
}