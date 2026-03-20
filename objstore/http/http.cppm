module;
#include "macros.h"
#include "plexdb_coro.h"

export module objstore.http;

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.tagged_union;
import plexdb.coro;

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
    //   Builds a fixed-content-length HTTP response into the caller-supplied
    //   registered io_uring buffer, sends it via CoroConnectionIO::send().
    // ========================================================================
    inline auto make_http_fixed_buffered_str8(tcp::CoroConnectionIO* io, U32 buffer_idx, int code, bool close) {
        U8*  buf         = io->buffer_ptr(buffer_idx);
        U64  buffer_size = io->buffer_size();

        const char* header_fmt =
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %20" PRIu64 "\r\n"
            "%s"
            "\r\n";

        // Measure header size (with a placeholder content-length for sizing).
        U64 header_byte_count = fmt_length(header_fmt, code, get_reason_for_status(code),
                                           static_cast<U64>(0), get_connection_header(close));
        assert_true_not_implemented(buffer_size > header_byte_count,
                                    "not enough space in buffer for http header");

        return BufferedString8(
            TArrayView<char>(
                reinterpret_cast<char*>(buf + header_byte_count),
                buffer_size - header_byte_count
            ),
            [=](const char* /*data*/, U64 length, bool is_final) {
                assert_true_not_implemented(is_final, "multiple async responses from fixed buffer");

                String8 header_view(buf, header_byte_count);
                fmt_raw(header_view, header_fmt, code, get_reason_for_status(code),
                        length, get_connection_header(close));

                io->send(buffer_idx, 0, static_cast<U32>(header_byte_count + length));
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
        MapFixedSentinel<os::Handle, parsers::http::RequestParser, 2*tcp::MAX_CONCURRENT_CONNECTIONS> client_to_http_parser;

        auto on_connection = [&](tcp::CoroConnectionIO& io) -> coro::Task {
            parsers::http::RequestParser& http_parser =
                find_or_insert(client_to_http_parser, io.connection->client);

            while (true) {
                auto recv_result = co_await io.recv();

                if (!recv_result.valid()) {
                    try_remove(client_to_http_parser, io.connection->client);
                    co_return;
                }

                parsers::http::execute(http_parser,
                    reinterpret_cast<char*>(recv_result.data), recv_result.byte_count);

                if (parsers::http::has_error(http_parser)) {
                    auto response_bstr = make_http_fixed_buffered_str8(&io, recv_result.buffer_idx, 400, /*close=*/true);
                    append(response_bstr, "Bad request: Malformed HTTP");
                    reset(http_parser);
                    // buffer_idx in-flight for send; write completion releases it.
                    co_return;
                }

                if (!parsers::http::is_complete(http_parser)) {
                    // Incomplete request — release recv buffer and wait for more data.
                    io.release_recv(recv_result.buffer_idx);
                    continue;
                }

                const parsers::http::Request& http_req = get_request(http_parser);

                auto cql_opt = parsers::cql::parse(http_req.body);
                if (!cql_opt) {
                    auto response_bstr = make_http_fixed_buffered_str8(&io, recv_result.buffer_idx, 400, /*close=*/false);
                    append(response_bstr, "Bad request: Failed to parse CQL");
                    reset(http_parser);
                    continue;
                }

                engine::ExecutionResult exec_result = engine::execute(engine, *cql_opt);

                if (exec_result.rows) {
                    auto response_bstr = make_http_fixed_buffered_str8(&io, recv_result.buffer_idx, 200, /*close=*/false);

                    U64 row_idx = 0;
                    append(response_bstr, "{\"status\":\"SUCCESS\",\"kind\":\"ROWS\",\"rows\":[");
                    for (auto& row = exec_result.rows->begin(); row != exec_result.rows->end(); ++row) {
                        if (row_idx != 0)
                            append(response_bstr, ",");

                        U64 col_idx = 0;
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
                    auto response_bstr = make_http_fixed_buffered_str8(&io, recv_result.buffer_idx, code, /*close=*/false);
                    append_json_execution_result(response_bstr, exec_result);
                }

                reset(http_parser);
                // buffer_idx in-flight for send; do not release here.
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

            auto ring_settings = uring::get_ring_settings();
            if (!ring_settings->recommended)
                return {"io_uring not available"};

            uring::Ring ring{
                socket,
                ring_settings->recommended_queue_depth,
                ring_settings->recommended_buffer_size,
                ring_settings->recommended_buffer_count
            };
            if (!ring)
                return {"failed to create io_uring ring"};

            on_ready_callback();

            tcp::listen(ring, on_connection, signal_pipe, should_exit);

            return {};
        }
    }
}
