module;
#include "macros.h"

export module objstore.http;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

import objstore.tcp;
import objstore.parsers;
import objstore.engine;

using namespace plexdb;
using namespace objstore;

namespace objstore::http {
    // ========================================================================
    // http
    // ========================================================================
    int get_http_status_for_execution_status(engine::ExecutionStatus status);
    const char* get_reason_for_status(int code);
    const char* get_connection_header(bool close);

    // ========================================================================
    // buffering
    // ========================================================================

    // @todo async
    // struct HttpChunkedEncodingMetadata {
    //     U64 buffer_size;
    //     U32 prefix_max_byte_count;
    //     String8 suffix;
    // };
    
    // HttpChunkedEncodingMetadata create_http_chunked_encoding(U64 buffer_size);

    // // @note asusmes buffer is correctly aligned and sized for http chunk
    // // @note requires small chunk
    // struct HttpChunkedEncodingFlushFunctor {
    //     tcp::Connection* connection;
    //     tcp::Chunk* chunk;

    //     HttpChunkedEncodingMetadata* meta;
    //     tcp::AsyncWriteFunctor* write;

    //     HttpChunkedEncodingFlushFunctor(tcp::Connection* in_connection, tcp::Chunk* in_chunk, HttpChunkedEncodingMetadata* in_meta, tcp::AsyncWriteFunctor* in_write);
    //     void operator ()(const char* data, U64 length, bool is_final);
    // };

    // inline BufferedString8<HttpChunkedEncodingFlushFunctor> make_http_chunked_encoding_buffered_str8(tcp::Connection* in_connection, tcp::Chunk* in_chunk, HttpChunkedEncodingMetadata* in_meta, tcp::AsyncWriteFunctor* in_write) {
    //     // @note relies on guaranteed copy elision
    //     return BufferedString8(
    //         TArrayView<char>(
    //             reinterpret_cast<char*>(in_chunk->data.ptr) + in_meta->prefix_max_byte_count,
    //             in_chunk->data.length - (in_meta->prefix_max_byte_count + in_meta->suffix.length)
    //         ),
    //         HttpChunkedEncodingFlushFunctor(in_connection, in_chunk, in_meta, in_write)
    //     );
    // }

    inline auto make_http_fixed_buffered_str8(tcp::Connection* in_connection, tcp::Chunk* in_chunk, tcp::AsyncWriteFunctor* in_write, int code, bool close) {
        assert_true(in_chunk->buffer_idx >= 0, "cannot use large chunk for buffering");

        U64 chunk_size = in_connection->chunk_chain.chunk_size;

        const char* header_fmt = ""
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %20" PRIu64 "\r\n"
            "%s"
            "\r\n";

        U64 header_byte_count = fmt_length(header_fmt, code, get_reason_for_status(code), in_chunk->data.length, get_connection_header(close)); 
        assert_true_not_implemented(chunk_size > header_byte_count, "not enough space in buffer for http header");

        return BufferedString8(
            TArrayView<char>(
                reinterpret_cast<char*>(in_chunk->data.ptr + header_byte_count),
                chunk_size - header_byte_count
            ),
            [=](const char* data, U64 length, bool is_final) {
                assert_true_not_implemented(is_final, "multiple async reponses from fixed buffer");

                String8 header_view(in_chunk->data.ptr, header_byte_count);
                fmt_raw(header_view, header_fmt, code, get_reason_for_status(code), length, get_connection_header(close));

                (*in_write)(in_connection, in_chunk->buffer_idx, 0, header_byte_count + length);
            }
        );
    }

    // ========================================================================
    // json
    // ========================================================================
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

    template<typename T> concept IsHttpDA = IsDynamicArray<T>;
    template<typename T> concept IsHttpDS = IsDynamicSet<T>;
    template<typename T> concept IsHttpDM = IsDynamicMap<T>;

    // Serialize a single native collection element as a JSON value
    template<BufferedString8Flush F, typename T>
    void append_json_native_element(BufferedString8<F>& str, const T& v) {
        using TT = Decay<T>;
        if constexpr (SameAs<TT, AutoString8>) {
            append_json_text(str, String8(v.c_str, v.length));
        } else if constexpr (SameAs<TT, S64>) {
            append(str, v);
        } else if constexpr (SameAs<TT, S32>) {
            append(str, v);
        } else if constexpr (SameAs<TT, S16>) {
            append(str, v);
        } else if constexpr (SameAs<TT, U8>) {
            append(str, static_cast<bool>(v));
        } else if constexpr (SameAs<TT, F32>) {
            append(str, v);
        } else if constexpr (SameAs<TT, F64>) {
            append(str, v);
        }
    }

    template<BufferedString8Flush F>
    void append_json_dtype_value(BufferedString8<F>& str, const dtype::ReadValue& value, CDType cdtype) {
        if (cdtype.base.ctype != CType::native) {
            visit(value, [&](const auto& v) {
                using T = Decay<decltype(v)>;
                if constexpr (IsHttpDM<T>) {
                    // map → JSON object (numeric keys are quoted as strings)
                    append(str, '{');
                    bool first = true;
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        if (!first) append(str, ',');
                        first = false;
                        using Key = Decay<decltype((*it).first)>;
                        if constexpr (SameAs<Key, AutoString8>) {
                            append_json_text(str, String8((*it).first.c_str, (*it).first.length));
                        } else {
                            append(str, '"');
                            append(str, (*it).first);
                            append(str, '"');
                        }
                        append(str, ':');
                        append_json_native_element(str, (*it).second);
                    }
                    append(str, '}');
                } else if constexpr (IsHttpDS<T> || IsHttpDA<T>) {
                    // list/set → JSON array
                    append(str, '[');
                    bool first = true;
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        if (!first) append(str, ',');
                        first = false;
                        append_json_native_element(str, *it);
                    }
                    append(str, ']');
                }
            });
            return;
        }
        switch (cdtype.native.value_dtype) {
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
    // @todo avoid chunking the small json responses which will fit in the buffer
    template<BufferedString8Flush F>
    void append_json_execution_result(BufferedString8<F>& str, engine::ExecutionResult& exec_result) {
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
            case engine::ResultKind::SchemaChange:
            case engine::ResultKind::UseKeyspace:{
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
            case engine::ResultKind::Rows:{
                assert_true(false, "unexpected execution result");
            }break;
            case engine::ResultKind::VirtualRows:{
                assert_true(false, "unexpected execution result");
            }break;
        }
    }
}

export namespace objstore::http {
    template<typename F>
    concept OnReady = requires(F f) { f(); };

    Optional<String8> run(U16 port, os::Notifier& signal_pipe, volatile bool& should_exit, engine::Engine& engine, OnReady auto&& on_ready_callback) {
        // @todo not compile time
        MapFixedSentinel<os::Handle, parsers::http::RequestParser, 2*tcp::MAX_CONCURRENT_CONNECTIONS> client_to_http_parser;

        const auto on_chunk = [&engine, &client_to_http_parser](const tcp::Request& req) -> tcp::RequestStatus {
            parsers::http::RequestParser& http_parser = find_or_insert(client_to_http_parser, req.connection->client);
            
            tcp::Chunk& chunk = *front(req.connection->chunk_chain.chunks);
            parsers::http::execute(http_parser, reinterpret_cast<char*>(chunk.data.ptr), chunk.data.length);
            
            if (parsers::http::has_error(http_parser)) {
                auto response_bstr = make_http_fixed_buffered_str8(req.connection, &chunk, req.write, 400, /*close=*/true);
                append(response_bstr, "Bad request: Malformed HTTP");
                reset(http_parser);
                return tcp::RequestStatus::Close;
            }
            if (!parsers::http::is_complete(http_parser)) {
                return tcp::RequestStatus::Pending;
            }
            
            const parsers::http::Request& http_req = get_request(http_parser);

            auto cql_opt = parsers::cql::parse(http_req.body);
            if (!cql_opt) {
                auto response_bstr = make_http_fixed_buffered_str8(req.connection, &chunk, req.write, 400, /*close=*/false);
                append(response_bstr, "Bad request: Failed to parse CQL");
                reset(http_parser);
                return tcp::RequestStatus::Handled;
            }
            
            engine::ExecutionResult exec_result = engine::execute(engine, *cql_opt);
            
            if (exec_result.rows) {
                auto response_bstr = make_http_fixed_buffered_str8(req.connection, &chunk, req.write, 200, /*close=*/false);

                U64 row_idx = 0;
                append(response_bstr, "{\"status\":\"SUCCESS\",\"kind\":\"ROWS\",\"rows\":[");
                for (auto& row = exec_result.rows->begin(); row != exec_result.rows->end(); ++row) {
                    if (row_idx != 0)
                        append(response_bstr, ",");
                    
                    U64 col_idx = 0;
                    U64 col_count = column_count(columns_begin(row));
                    append(response_bstr, "[");
                    for (auto col = columns_begin(row); col != columns_end(row); ++col) {
                        if (col_idx != 0)
                            append(response_bstr, ',');

                        append_json_dtype_value(response_bstr, read_value(col), column(col).type);
                        ++col_idx;
                    }
                    append(response_bstr, ']');
                    ++row_idx;
                }
                append(response_bstr, "]}");
            } else {
                U64 code = get_http_status_for_execution_status(exec_result.status);
                auto response_bstr = make_http_fixed_buffered_str8(req.connection, &chunk, req.write, code, /*close=*/false);
                append_json_execution_result(response_bstr, exec_result);
            }
            
            reset(http_parser);
            return tcp::RequestStatus::Handled;
        };

        const auto on_open = [](tcp::Connection*) {};

        const auto on_close = [&client_to_http_parser](tcp::Connection* connection) {
            if (connection != nullptr) {
                try_remove(client_to_http_parser, connection->client);
            }
        };

        {
            os::Socket socket{os::socket_open()};
            if (!socket)
                return {"failed to open server socket"};
            if (!os::socket_set_option(socket, os::SocketOption::Reuse, true))
                return {"failed to set reuse on server socket"};
            if (!os::socket_bind(socket, port))
                return {"failed to bind server socket"};
            if (!os::socket_listen(socket, 128))
                return {"failed to listen on server socket"};
    
            on_ready_callback();
    
            tcp::listen(
                socket,
                on_chunk, on_open, on_close,
                signal_pipe, should_exit
            );

            return {};
        }
    }
}
