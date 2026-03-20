module;
#include "macros.h"
#include "plexdb_coro.h"

export module objstore.native;

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.os.containers;
import plexdb.btree;
import plexdb.tagged_union;
import plexdb.coro;

import objstore.tcp;
import objstore.parsers;
import objstore.engine;
import objstore.engine.statements;

using namespace plexdb;
using namespace objstore;

// ============================================================================
// Private implementation
// ============================================================================
namespace objstore::native {
    constexpr U64 FRAME_HEADER_SIZE = 9;
    constexpr U8  RESPONSE_VERSION  = 0x84;

    // ========================================================================
    // Protocol
    // ========================================================================
    namespace opcode {
        constexpr U8 ERROR         = 0x00;
        constexpr U8 STARTUP       = 0x01;
        constexpr U8 READY         = 0x02;
        constexpr U8 OPTIONS       = 0x05;
        constexpr U8 SUPPORTED     = 0x06;
        constexpr U8 QUERY         = 0x07;
        constexpr U8 RESULT        = 0x08;
        constexpr U8 PREPARE       = 0x09;
        constexpr U8 EXECUTE       = 0x0A;
        constexpr U8 REGISTER      = 0x0B;
        constexpr U8 BATCH         = 0x0D;
        constexpr U8 AUTH_RESPONSE = 0x0F;
    }

    namespace result_kind {
        constexpr S32 VOID          = 0x0001;
        constexpr S32 ROWS          = 0x0002;
        constexpr S32 SET_KEYSPACE  = 0x0003;
        constexpr S32 SCHEMA_CHANGE = 0x0005;
    }

    namespace cql_type {
        constexpr U16 Bigint    = 0x0002;
        constexpr U16 Boolean   = 0x0004;
        constexpr U16 Counter   = 0x0005;
        constexpr U16 Double    = 0x0007;
        constexpr U16 Float     = 0x0008;
        constexpr U16 Int       = 0x0009;
        constexpr U16 Timestamp = 0x000B;
        constexpr U16 Uuid      = 0x000C;
        constexpr U16 Varchar   = 0x000D;
        constexpr U16 Smallint  = 0x0013;
        constexpr U16 List      = 0x0020;
        constexpr U16 Map       = 0x0021;
        constexpr U16 Set       = 0x0022;
    }

    constexpr U16 dtype_to_cql_type(DType dtype) {
        switch (dtype) {
            case DType::text:       return cql_type::Varchar;
            case DType::uuid:       return cql_type::Uuid;
            case DType::timestamp:  return cql_type::Timestamp;
            case DType::smallint:   return cql_type::Smallint;
            case DType::int_:       return cql_type::Int;
            case DType::bigint:     return cql_type::Bigint;
            case DType::counter:    return cql_type::Counter;
            case DType::boolean:    return cql_type::Boolean;
            case DType::float_:     return cql_type::Float;
            case DType::double_:    return cql_type::Double;
        }
        return 0x0000;
    }

    constexpr U16 dtype_to_cql_type(CDType cdtype) {
        if (cdtype.base.ctype != CType::native) return 0x0000;
        return dtype_to_cql_type(cdtype.native.value_dtype);
    }

    template<typename T> concept IsCqlDA = IsDynamicArray<T>;
    template<typename T> concept IsCqlDS = IsDynamicSet<T>;
    template<typename T> concept IsCqlDM = IsDynamicMap<T>;

    // ========================================================================
    // Per-connection state
    // ========================================================================
    struct NativeState {
        // @todo obviously this is wrong
        DynamicArray<U8> recv_buf;
        bool startup_done = false;
    };

    // ========================================================================
    // Read/write helpers
    // ========================================================================
    U16 read_be_u16(const U8* p);
    S16 read_be_s16(const U8* p);
    S32 read_be_s32(const U8* p);

    // Read [long string]: [int] n + n bytes
    String8 read_cql_long_string(const U8*& p, const U8* end);

    template<BufferedString8Flush F>
    inline void append_be_u8(BufferedString8<F>& buf, U8 v) {
        append(buf, static_cast<char>(v));
    }

    template<BufferedString8Flush F>
    inline void append_be_u16(BufferedString8<F>& buf, U16 v) {
        append_be_u8(buf, U8(v >> 8));
        append_be_u8(buf, U8(v));
    }

    template<BufferedString8Flush F>
    inline void append_be_s32(BufferedString8<F>& buf, S32 v) {
        append_be_u8(buf, U8(U32(v) >> 24));
        append_be_u8(buf, U8(U32(v) >> 16));
        append_be_u8(buf, U8(U32(v) >> 8));
        append_be_u8(buf, U8(U32(v)));
    }

    // Write [string]: [short] n + bytes
    template<BufferedString8Flush F>
    inline void append_cql_string(BufferedString8<F>& buf, String8 s) {
        append_be_u16(buf, U16(s.length));
        for (U64 i = 0; i < s.length; i++)
            append_be_u8(buf, U8(s.data[i]));
    }

    // Write [bytes]: [int] n + bytes  (n < 0 = null)
    template<BufferedString8Flush F>
    inline void append_cql_bytes_raw(BufferedString8<F>& buf, const U8* data, S32 n) {
        append_be_s32(buf, n);
        if (n > 0) {
            for (S32 i = 0; i < n; i++)
                append_be_u8(buf, data[i]);
        }
    }

    // Write a single native element as [bytes] (big-endian, includes the [int] length prefix)
    template<BufferedString8Flush F, typename T>
    void append_cql_native_element(BufferedString8<F>& buf, const T& v) {
        using TT = Decay<T>;
        if constexpr (SameAs<TT, AutoString8>) {
            append_cql_bytes_raw(buf, reinterpret_cast<const U8*>(v.c_str), S32(v.length));
        } else if constexpr (SameAs<TT, S64>) {
            S64 vv = v;
            U8 data[8];
            for (int i = 7; i >= 0; i--) { data[i] = U8(vv); vv >>= 8; }
            append_cql_bytes_raw(buf, data, 8);
        } else if constexpr (SameAs<TT, S32>) {
            U8 data[4] = { U8(U32(v) >> 24), U8(U32(v) >> 16), U8(U32(v) >> 8), U8(U32(v)) };
            append_cql_bytes_raw(buf, data, 4);
        } else if constexpr (SameAs<TT, S16>) {
            U8 data[2] = { U8(U16(v) >> 8), U8(v) };
            append_cql_bytes_raw(buf, data, 2);
        } else if constexpr (SameAs<TT, U8>) {
            append_cql_bytes_raw(buf, &v, 1);
        } else if constexpr (SameAs<TT, F32>) {
            U32 bits; os::memory_copy(&bits, &v, sizeof(bits));
            U8 data[4] = { U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits) };
            append_cql_bytes_raw(buf, data, 4);
        } else if constexpr (SameAs<TT, F64>) {
            U64 bits; os::memory_copy(&bits, &v, sizeof(bits));
            U8 data[8];
            for (int i = 7; i >= 0; i--) { data[i] = U8(bits); bits >>= 8; }
            append_cql_bytes_raw(buf, data, 8);
        }
    }

    template<typename T>
    S32 cql_native_element_byte_size(const T& v) {
        using TT = Decay<T>;
        if constexpr (SameAs<TT, AutoString8>) return S32(v.length);
        if constexpr (SameAs<TT, S64>)        return 8;
        if constexpr (SameAs<TT, S32>)        return 4;
        if constexpr (SameAs<TT, S16>)        return 2;
        if constexpr (SameAs<TT, U8>)         return 1;
        if constexpr (SameAs<TT, F32>)        return 4;
        if constexpr (SameAs<TT, F64>)        return 8;
        return 0;
    }

    // Write an [option] for a column type (§4.2.5.2): [short] type_id + optional nested type(s)
    template<BufferedString8Flush F>
    void append_cql_type_option(BufferedString8<F>& buf, CDType cdtype) {
        switch (cdtype.base.ctype) {
            case CType::native:
                append_be_u16(buf, dtype_to_cql_type(cdtype.native.value_dtype));
                break;
            case CType::list:
                append_be_u16(buf, cql_type::List);
                append_be_u16(buf, dtype_to_cql_type(cdtype.list.element_dtype));
                break;
            case CType::set:
                append_be_u16(buf, cql_type::Set);
                append_be_u16(buf, dtype_to_cql_type(cdtype.set.key_dtype));
                break;
            case CType::map:
                append_be_u16(buf, cql_type::Map);
                append_be_u16(buf, dtype_to_cql_type(cdtype.map.key_dtype));
                append_be_u16(buf, dtype_to_cql_type(cdtype.map.value_dtype));
                break;
        }
    }

    // Write a dtype::ReadValue as CQL binary [bytes] (big-endian network order)
    template<BufferedString8Flush F>
    void append_cql_value(BufferedString8<F>& buf, const dtype::ReadValue& value, CDType cdtype) {
        if (cdtype.base.ctype != CType::native) {
            // Serialize collection as [bytes]: [int] body_len | [int] elem_count | n×[bytes]
            visit(value, [&](const auto& v) {
                using T = Decay<decltype(v)>;
                if constexpr (IsCqlDM<T>) {
                    U64 pair_count = length(v);
                    S32 body = 4; // [int] pair count
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        body += 4 + cql_native_element_byte_size((*it).first);
                        body += 4 + cql_native_element_byte_size((*it).second);
                    }
                    append_be_s32(buf, body);
                    append_be_s32(buf, S32(pair_count));
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        append_cql_native_element(buf, (*it).first);
                        append_cql_native_element(buf, (*it).second);
                    }
                } else if constexpr (IsCqlDS<T>) {
                    U64 elem_count = length(v);
                    S32 body = 4;
                    for (auto it = v.begin(); it != v.end(); ++it)
                        body += 4 + cql_native_element_byte_size(*it);
                    append_be_s32(buf, body);
                    append_be_s32(buf, S32(elem_count));
                    for (auto it = v.begin(); it != v.end(); ++it)
                        append_cql_native_element(buf, *it);
                } else if constexpr (IsCqlDA<T>) {
                    S32 body = 4;
                    for (U64 i = 0; i < v.length; i++)
                        body += 4 + cql_native_element_byte_size(v[i]);
                    append_be_s32(buf, body);
                    append_be_s32(buf, S32(v.length));
                    for (U64 i = 0; i < v.length; i++)
                        append_cql_native_element(buf, v[i]);
                }
            });
            return;
        }
        switch (cdtype.native.value_dtype) {
            case DType::text:
            case DType::uuid:
            case DType::timestamp: {
                const AutoString8& s = get<AutoString8>(value);
                append_cql_bytes_raw(buf, reinterpret_cast<const U8*>(s.c_str), S32(s.length));
            } break;
            case DType::smallint: {
                S16 v = get<S16>(value);
                U8 data[2] = { U8(v >> 8), U8(v) };
                append_cql_bytes_raw(buf, data, 2);
            } break;
            case DType::int_: {
                S32 v = get<S32>(value);
                U8 data[4] = { U8(U32(v) >> 24), U8(U32(v) >> 16), U8(U32(v) >> 8), U8(U32(v)) };
                append_cql_bytes_raw(buf, data, 4);
            } break;
            case DType::counter:
            case DType::bigint: {
                S64 v = get<S64>(value);
                U8 data[8];
                for (int i = 7; i >= 0; i--) {
                    data[i] = U8(v);
                    v >>= 8;
                }
                append_cql_bytes_raw(buf, data, 8);
            } break;
            case DType::boolean: {
                U8 v = get<U8>(value);
                append_cql_bytes_raw(buf, &v, 1);
            } break;
            case DType::float_: {
                F32 v = get<F32>(value);
                U32 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                U8 data[4] = { U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits) };
                append_cql_bytes_raw(buf, data, 4);
            } break;
            case DType::double_: {
                F64 v = get<F64>(value);
                U64 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                U8 data[8];
                for (int i = 7; i >= 0; i--) {
                    data[i] = U8(bits);
                    bits >>= 8;
                }
                append_cql_bytes_raw(buf, data, 8);
            } break;
        }
    }

    // ========================================================================
    // Response frame builder
    // ========================================================================
    // Writes the response body into the caller-supplied registered buffer,
    // filling in the 9-byte CQL frame header when the BufferedString8 flushes.
    inline auto make_native_frame(tcp::CoroConnectionIO* io, U32 buffer_idx, U8 op, S16 stream) {
        U8*  buf         = io->buffer_ptr(buffer_idx);
        U64  buffer_size = io->buffer_size();

        return BufferedString8(
            TArrayView<char>(
                reinterpret_cast<char*>(buf + FRAME_HEADER_SIZE),
                buffer_size - FRAME_HEADER_SIZE
            ),
            [=](const char* /*data*/, U64 length, bool is_final) {
                assert_true_not_implemented(is_final, "native response body too large for buffer");

                U8* hdr = buf;
                hdr[0] = RESPONSE_VERSION;
                hdr[1] = 0x00;  // flags
                hdr[2] = U8(U16(stream) >> 8);
                hdr[3] = U8(stream);
                hdr[4] = op;
                U32 body_len = U32(length);
                hdr[5] = U8(body_len >> 24);
                hdr[6] = U8(body_len >> 16);
                hdr[7] = U8(body_len >> 8);
                hdr[8] = U8(body_len);

                io->send(buffer_idx, 0, FRAME_HEADER_SIZE + U32(length));
            }
        );
    }

    // ========================================================================
    // Body writers for each RESULT/ERROR type
    // ========================================================================
    template<BufferedString8Flush F>
    void append_error_body(BufferedString8<F>& buf, engine::ExecutionStatus status, String8 message) {
        append_be_s32(buf, S32(static_cast<U16>(status)));
        append_cql_string(buf, message);
    }

    template<BufferedString8Flush F>
    void append_result_void(BufferedString8<F>& buf) {
        append_be_s32(buf, result_kind::VOID);
    }

    template<BufferedString8Flush F>
    void append_result_set_keyspace(BufferedString8<F>& buf, String8 keyspace) {
        append_be_s32(buf, result_kind::SET_KEYSPACE);
        append_cql_string(buf, keyspace);
    }

    template<BufferedString8Flush F>
    void append_result_schema_change(BufferedString8<F>& buf, String8 change_type, String8 target, String8 keyspace, String8 table) {
        append_be_s32(buf, result_kind::SCHEMA_CHANGE);
        append_cql_string(buf, change_type);
        append_cql_string(buf, target);
        append_cql_string(buf, keyspace);
        if (table.length > 0)
            append_cql_string(buf, table);
    }

    // @note uses btree::size so RowRange is only iterated once (RowRange iterators are not resettable)
    template<BufferedString8Flush F>
    void append_result_rows(BufferedString8<F>& buf, engine::ExecutionResult& result, schema::Table* tbl) {
        append_be_s32(buf, result_kind::ROWS);

        append_be_s32(buf, 0x0001);  // Global_tables_spec flag
        append_be_s32(buf, S32(tbl->cols.length));
        append_cql_string(buf, result.keyspace);
        append_cql_string(buf, result.table);

        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
            append_cql_string(buf, tbl->cols[ci].name);
            append_cql_type_option(buf, tbl->cols[ci].type);
        }

        append_be_s32(buf, S32(btree::size(tbl->btree)));

        for (auto& row = result.rows->begin(); row != result.rows->end(); ++row) {
            for (auto col = engine::columns_begin(row); col != engine::columns_end(row); ++col)
                append_cql_value(buf, engine::read_value(col), engine::column(col).type);
        }
    }

    template<BufferedString8Flush F>
    void append_result_virtual_rows(BufferedString8<F>& buf, engine::VirtualRows& vr) {
        append_be_s32(buf, result_kind::ROWS);

        append_be_s32(buf, 0x0001);  // Global_tables_spec flag
        append_be_s32(buf, S32(vr.columns.length));
        append_cql_string(buf, vr.keyspace);
        append_cql_string(buf, vr.table);

        for (U64 ci = 0; ci < vr.columns.length; ci++) {
            append_cql_string(buf, vr.columns[ci].name);
            append_cql_type_option(buf, vr.columns[ci].type);
        }

        append_be_s32(buf, S32(vr.rows.length));

        for (U64 ri = 0; ri < vr.rows.length; ri++) {
            for (U64 ci = 0; ci < vr.columns.length; ci++)
                append_cql_value(buf, vr.rows[ri].values[ci], vr.columns[ci].type);
        }
    }

    // ========================================================================
    // Frame dispatcher
    // ========================================================================
    void handle_frame(
        NativeState& state,
        engine::Engine& engine,
        tcp::CoroConnectionIO* io,
        U32 buffer_idx,
        const U8* header,
        const U8* body,
        S32 body_len
    ) {
        S16 stream  = read_be_s16(header + 2);
        U8  op      = header[4];

        const U8* body_end = body + body_len;

        switch (op) {
            case opcode::STARTUP: {
                state.startup_done = true;
                auto frame = make_native_frame(io, buffer_idx, opcode::READY, stream);
                // empty body
            } break;

            case opcode::OPTIONS: {
                auto frame = make_native_frame(io, buffer_idx, opcode::SUPPORTED, stream);
                // [string multimap]: n pairs of ([string] key, [string list] values)
                append_be_u16(frame, 1);
                append_cql_string(frame, "CQL_VERSION");
                append_be_u16(frame, 1);
                append_cql_string(frame, "3.0.0");
            } break;

            case opcode::QUERY: {
                const U8* p = body;
                String8 query = read_cql_long_string(p, body_end);
                // Remaining bytes are query parameters (consistency, flags, etc.) - ignored

                auto cql_opt = parsers::cql::parse(query);
                if (!cql_opt) {
                    auto frame = make_native_frame(io, buffer_idx, opcode::ERROR, stream);
                    append_error_body(frame, engine::ExecutionStatus::SyntaxError, "Failed to parse CQL");
                    break;
                }

                engine::ExecutionResult result = engine::execute(engine, *cql_opt);

                if (result.status != engine::ExecutionStatus::Success) {
                    auto frame = make_native_frame(io, buffer_idx, opcode::ERROR, stream);
                    String8 msg = result.message.length ? result.message : engine::to_str(result.status);
                    append_error_body(frame, result.status, msg);
                    break;
                }

                switch (result.kind) {
                    case engine::ResultKind::Void:{
                        auto frame = make_native_frame(io, buffer_idx, opcode::RESULT, stream);
                        append_result_void(frame);
                    }break;
                    case engine::ResultKind::SchemaChange:{
                        String8 change_type = result.message.length ? result.message : "UPDATED";
                        String8 target      = result.table.length   ? "TABLE"        : "KEYSPACE";
                        auto frame = make_native_frame(io, buffer_idx, opcode::RESULT, stream);
                        append_result_schema_change(frame, change_type, target, result.keyspace, result.table);
                    }break;
                    case engine::ResultKind::UseKeyspace:{
                        auto frame = make_native_frame(io, buffer_idx, opcode::RESULT, stream);
                        append_result_set_keyspace(frame, result.keyspace);
                    }break;
                    case engine::ResultKind::Rows:{
                        auto ks = schema::read_keyspace(engine.schema, result.keyspace);
                        assert_true(ks != nullptr, "keyspace not found for rows result");
                        auto tbl = schema::read_table(engine.schema, *ks, result.table);
                        assert_true(tbl != nullptr, "table not found for rows result");
                        auto frame = make_native_frame(io, buffer_idx, opcode::RESULT, stream);
                        append_result_rows(frame, result, tbl);
                    }break;
                    case engine::ResultKind::VirtualRows:{
                        assert_true(result.virtual_rows.has_value(), "virtual rows missing");
                        auto frame = make_native_frame(io, buffer_idx, opcode::RESULT, stream);
                        append_result_virtual_rows(frame, *result.virtual_rows);
                    }break;
                }
            } break;

            case opcode::REGISTER: {
                // Ignore event registration, respond with READY
                auto frame = make_native_frame(io, buffer_idx, opcode::READY, stream);
            } break;

            case opcode::PREPARE: {
                assert_true_not_implemented(false, "PREPARE not implemented");
            } break;

            case opcode::EXECUTE: {
                assert_true_not_implemented(false, "EXECUTE not implemented");
            } break;

            case opcode::BATCH: {
                assert_true_not_implemented(false, "BATCH not implemented");
            } break;

            default: {
                auto frame = make_native_frame(io, buffer_idx, opcode::ERROR, stream);
                append_error_body(frame, engine::ExecutionStatus::Invalid, "Unknown opcode");
            } break;
        }
    }
}

// ============================================================================
// Public interface
// ============================================================================
export namespace objstore::native {
    template<typename F>
    concept OnReady = requires(F f) { f(); };

    Optional<String8> run(U16 port, os::Notifier& signal_pipe, volatile bool& should_exit, engine::Engine& engine, OnReady auto&& on_ready_callback) {
        MapFixedSentinel<os::Handle, NativeState, 2_u64*tcp::MAX_CONCURRENT_CONNECTIONS> client_to_state;

        auto on_connection = [&](tcp::CoroConnectionIO& io) -> coro::Task {
            NativeState& state = find_or_insert(client_to_state, io.connection->client);

            while (true) {
                auto recv_result = co_await io.recv();

                if (!recv_result.valid()) {
                    try_remove(client_to_state, io.connection->client);
                    co_return;
                }

                // Append recv data to our frame-reassembly buffer.
                for (int i = 0; i < recv_result.byte_count; i++)
                    push_back(state.recv_buf, recv_result.data[i]);

                // Process one complete CQL frame if available.
                if (state.recv_buf.length >= FRAME_HEADER_SIZE) {
                    const U8* hdr     = state.recv_buf.ptr;
                    S32       body_len = read_be_s32(hdr + 5);

                    if (body_len >= 0 &&
                        state.recv_buf.length >= FRAME_HEADER_SIZE + U64(body_len))
                    {
                        // Reuse the recv buffer for the response: data has been
                        // copied to recv_buf so the registered buffer is free.
                        handle_frame(state, engine, &io, recv_result.buffer_idx,
                                     hdr, hdr + FRAME_HEADER_SIZE, body_len);

                        U64 frame_size = FRAME_HEADER_SIZE + U64(body_len);
                        U64 remaining  = state.recv_buf.length - frame_size;
                        if (remaining > 0)
                            os::memory_move(state.recv_buf.ptr,
                                            state.recv_buf.ptr + frame_size, remaining);
                        state.recv_buf.length = remaining;

                        // buffer_idx is now in-flight for the send; the write
                        // completion handler in listen() releases it.
                    } else {
                        // Incomplete frame — release buffer and wait for more data.
                        io.release_recv(recv_result.buffer_idx);
                    }
                } else {
                    io.release_recv(recv_result.buffer_idx);
                }
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
