module objstore.server;

import plexdb.base;
import plexdb.os;

import objstore.tcp;
import objstore.parser;
import objstore.engine;

using namespace plexdb;

namespace objstore::server {
    static void return_execution_result(tcp::Request& req, engine::ExecutionResult result, const String8& value) {
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

    void run(int port, int signal_fd, volatile bool& should_exit, engine::Engine& engine) {
        tcp::Pool pool{port, signal_fd};
        tcp::Stats stats{};
        
        MapFixedSentinel<int, parser::http::Parser, 2*tcp::MAX_CONNECTIONS> req_id_to_http_parser;

        println("listening on port ", to_str(port));

        const auto on_chunk = [&engine, &req_id_to_http_parser](tcp::Request& req) -> tcp::RequestStatus {
            parser::http::Parser& http_parser = find_or_insert(req_id_to_http_parser, req.id);
            
            tcp::Chunk& chunk = *back(*req.chunks);
            execute(http_parser, chunk.data.ptr, chunk.data.length);
            
            if (parser::http::has_error(http_parser)) {
                tcp::return_http_fail(req, 400, "Bad request: Malformed HTTP", true);
                reset(http_parser);
                return tcp::RequestStatus::Close;
            }
            if (!parser::http::is_complete(http_parser)) {
                return tcp::RequestStatus::Pending;
            }
            
            const parser::http::Request& http_req = get_request(http_parser);

            auto cql_opt = parser::cql::parse(http_req.body);
            if (!cql_opt) {
                tcp::return_http_fail(req, 400, "Bad request: Failed to parse CQL");
                reset(http_parser);
                return tcp::RequestStatus::Handled;
            }

            constexpr U64 SCRATCHPAD_SIZE = 4_kb;
            Array<U8, SCRATCHPAD_SIZE> scratchpad;
            engine::ExecutionContext execution_ctx = {
                .write_buffer = TArrayView<U8>(scratchpad.values, SCRATCHPAD_SIZE),
            };

            AutoString8 value{""};
            const auto flush = [&execution_ctx, &value](U64 length) {
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

        tcp::listen(on_chunk, on_close, pool, stats, should_exit);

        println("shutting down...");
    }
}
