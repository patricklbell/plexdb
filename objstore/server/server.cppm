export module objstore.server;

import plexdb.base;
import plexdb.os;

import objstore.tcp;
import objstore.parser;
import objstore.engine;

using namespace plexdb;
using namespace objstore;

namespace objstore::server {
    template<BufferedString8Flush F>
    void append_json_text(BufferedString8<F>& str, String8 text) {
        append(str, '"');
        for (U64 i = 0; i < text.length; i++) {
            char c = text.data[i];
            switch (c) {
                case '"':  append(str, "\\\""); break;
                case '\\': append(str, "\\\\"); break;
                case '\n': append(str, "\\n"); break;
                case '\r': append(str, "\\r"); break;
                case '\t': append(str, "\\t"); break;
                default:   append(str, c); break;
            }
        }
        append(str, '"');
    }

    template<BufferedString8Flush F>
    void append_json_dtype_value(BufferedString8<F>& str, const dtype::ReadValue& value, DType dtype) {
        switch (dtype) {
            case DType::text:
            case DType::uuid:
            case DType::timestamp:{
                const AutoString8& s = get<AutoString8>(value);
                append_json_text(str, String8(s.c_str, s.length));
            }break;
            case DType::smallint:{
                append(str, get<S16>(value));
            }break;
            case DType::int_:{
                append(str, get<S32>(value));
            }break;
            case DType::counter:
            case DType::bigint:{
                append(str, get<S64>(value));
            }break;
            case DType::boolean:{
                append(str, static_cast<bool>(get<U8>(value)));
            }break;
            case DType::float_:{
                append(str, get<F32>(value));
            }break;
            case DType::double_:{
                append(str, get<F64>(value));
            }break;
        }
    }

    // @note ignores row kind because this needs to be streamed
    template<BufferedString8Flush F>
    void append_json_execution_result(BufferedString8<F>& str, engine::ExecutionResult& exec_result, U64 row_count) {
        switch (exec_result.kind) {
            case engine::ResultKind::Void:{
                append(str, "{\"status\":");
                append_json_text(str, engine::to_str(exec_result.status));
                if (exec_result.message.length) {
                    append(str, ",\"message\":");
                    append_json_text(str, exec_result.message);
                }
                append(str, "}");
            }break;
            case engine::ResultKind::Rows:{
                if (row_count == 0) {
                    append(str, "{\"status\":\"SUCCESS\",\"kind\":\"ROWS\",\"rows\":[]}");
                } else {
                    append(str, "]}");
                }
                return;
            }break;
            case engine::ResultKind::SchemaChange:{
                append(str, "{\"status\":");
                append_json_text(str, engine::to_str(exec_result.status));
                if (exec_result.keyspace.length > 0) {
                    append(str, ",\"keyspace\":");
                    append_json_text(str, exec_result.keyspace);
                }
                if (exec_result.table.length > 0) {
                    append(str, ",\"table\":");
                    append_json_text(str, exec_result.table);
                }
                append(str, "}");
            }break;
        }
        
    }

    int get_http_status_for_execution_status(engine::ExecutionStatus status);
}

export namespace objstore::server {
    template<typename F>
    concept OnReady = requires(F f) { f(); };

    void run(int port, int signal_fd, volatile bool& should_exit, engine::Engine& engine, OnReady auto&& on_ready) {
        tcp::Pool pool{port, signal_fd};
        on_ready();
        
        MapFixedSentinel<int, parser::http::Parser, 2*tcp::MAX_CONNECTIONS> req_id_to_http_parser;
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

            // @todo make this write to the buffer directly
            constexpr U64 RESPONSE_BUFFER_SIZE = 64 * 1_kb;
            char response_buffer[RESPONSE_BUFFER_SIZE + 1];
            const auto flush = [](const char*, U64) {
                // @todo chunked http response
                assert_not_implemented("large reponses that require flushing are not implemented");
            };

            BufferedString8 reponse{TArrayView<char>{response_buffer, RESPONSE_BUFFER_SIZE}, flush};

            bool row_count = 0;
            const auto on_value = [&reponse,&row_count](const schema::Table& tbl, const schema::Column& col, U64 col_count, U64 row_idx, U64 col_idx, const dtype::ReadValue& value) {
                if (col_idx == 0) {
                    append(reponse, (row_idx == 0) ? "{\"status\":\"SUCCESS\",\"kind\":\"ROWS\",\"rows\":[" : ",");
                }

                append(reponse, (col_idx == 0) ? '[' : ',');
                append_json_dtype_value(reponse, value, col.dtype);
                if (col_idx == col_count - 1) {
                    append(reponse, ']');
                }

                row_count = row_idx + 1;
            };

            engine::ExecutionResult exec_result = engine::execute(engine, *cql_opt, on_value);

            append_json_execution_result(reponse, exec_result, row_count);

            int http_status = get_http_status_for_execution_status(exec_result.status);

            // @todo flushing
            response_buffer[reponse.length] = '\0';
            String8 body{response_buffer, reponse.length};

            if (http_status / 100 == 2) {
                tcp::return_http_success(req, body);
            } else {
                tcp::return_http_fail(req, http_status, body);
            }
            
            reset(http_parser);
            return tcp::RequestStatus::Handled;
        };

        const auto on_close = [&req_id_to_http_parser](int req_id) {
            try_remove(req_id_to_http_parser, req_id);
        };

        tcp::Stats stats{};

        tcp::listen(on_chunk, on_close, pool, stats, should_exit);
    }
}
