module;
#include "macros.h"
#include <profiling/tracy.hpp>
#include <coroutine>

export module objstore.native;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.btree;
import plexdb.tagged_union;
import plexdb.os.dynamic_tagged_union;
import plexdb.coroutine;

import objstore.tcp;
import objstore.parsers;
import objstore.engine;
import objstore.engine.statements;
import objstore.engine.io;
import objstore.log;

using namespace plexdb;
using namespace objstore;

namespace objstore::native {
    constexpr U64 FRAME_HEADER_BYTE_COUNT = 9;
    constexpr U8  RESPONSE_VERSION  = 0x84;

    // ========================================================================
    // protocol
    // ========================================================================
    namespace op_codes {
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

    constexpr String8 op_codes_to_str(const U8& op) {
        switch (op) {
            case op_codes::ERROR        : return "ERROR";
            case op_codes::STARTUP      : return "STARTUP";
            case op_codes::READY        : return "READY";
            case op_codes::OPTIONS      : return "OPTIONS";
            case op_codes::SUPPORTED    : return "SUPPORTED";
            case op_codes::QUERY        : return "QUERY";
            case op_codes::RESULT       : return "RESULT";
            case op_codes::PREPARE      : return "PREPARE";
            case op_codes::EXECUTE      : return "EXECUTE";
            case op_codes::REGISTER     : return "REGISTER";
            case op_codes::BATCH        : return "BATCH";
            case op_codes::AUTH_RESPONSE: return "AUTH_RESPONSE";
        };
        return "UNKNOWN";
    }

    namespace result_codes {
        constexpr S32 VOID          = 0x0001;
        constexpr S32 ROWS          = 0x0002;
        constexpr S32 SET_KEYSPACE  = 0x0003;
        constexpr S32 PREPARED      = 0x0004;
        constexpr S32 SCHEMA_CHANGE = 0x0005;
    }

namespace type_codes {
        constexpr U16 Ascii    = 0x0001;
        constexpr U16 Bigint   = 0x0002;
        constexpr U16 Blob     = 0x0003;
        constexpr U16 Boolean  = 0x0004;
        constexpr U16 Counter  = 0x0005;
        constexpr U16 Decimal  = 0x0006;
        constexpr U16 Double   = 0x0007;
        constexpr U16 Float    = 0x0008;
        constexpr U16 Int      = 0x0009;
        constexpr U16 Timestamp = 0x000B;
        constexpr U16 Uuid    = 0x000C;
        constexpr U16 Varchar  = 0x000D;
        constexpr U16 Varint   = 0x000E;
        constexpr U16 Timeuuid = 0x000F;
        constexpr U16 Inet    = 0x0010;
        constexpr U16 Date     = 0x0011;
        constexpr U16 Time     = 0x0012;
        constexpr U16 Smallint = 0x0013;
        constexpr U16 Tinyint  = 0x0014;
        constexpr U16 Duration = 0x0015;
        constexpr U16 List     = 0x0020;
        constexpr U16 Map     = 0x0021;
        constexpr U16 Set     = 0x0022;
    }

    constexpr U16 basic_type_to_type_code(BasicType dtype) {
        switch (dtype) {
            case BasicType::ascii:     return type_codes::Ascii;
            case BasicType::bigint:    return type_codes::Bigint;
            case BasicType::blob:      return type_codes::Blob;
            case BasicType::boolean:   return type_codes::Boolean;
            case BasicType::counter:   return type_codes::Counter;
            case BasicType::date:      return type_codes::Date;
            case BasicType::decimal:   return type_codes::Decimal;
            case BasicType::double_:   return type_codes::Double;
            case BasicType::duration:  return type_codes::Duration;
            case BasicType::float_:    return type_codes::Float;
            case BasicType::inet:      return type_codes::Inet;
            case BasicType::int_:      return type_codes::Int;
            case BasicType::smallint:  return type_codes::Smallint;
            case BasicType::text:      return type_codes::Varchar;
            case BasicType::time:      return type_codes::Time;
            case BasicType::timestamp: return type_codes::Timestamp;
            case BasicType::tinyint:   return type_codes::Tinyint;
            case BasicType::uuid:      return type_codes::Uuid;
            case BasicType::varchar:   return type_codes::Varchar;
            case BasicType::varint:    return type_codes::Varint;
            case BasicType::timeuuid:  return type_codes::Timeuuid;
            case BasicType::vector:
            case BasicType::hex:{
                assert_not_implemented("native protocol type code for vector/hex type is not implemented");
                return 0x0000;
            }break;
        }

        assert_true(false, "invalid basic type");
        return 0x0000;
    }

    template <typename T>
    S32 basic_element_byte_size(const BasicType& dtype, const T& value) {
        using TT = Decay<T>;

        if constexpr (SameAs<TT, S64>) {
            assert_true(dtype == BasicType::bigint || dtype == BasicType::counter || dtype == BasicType::timestamp || dtype == BasicType::time,
                        "S64 does not match basic type");
            return 8;
        } else if constexpr (SameAs<TT, S32>) {
            assert_true(dtype == BasicType::int_ || dtype == BasicType::date,
                        "S32 does not match basic type");
            return 4;
        } else if constexpr (SameAs<TT, S16>) {
            assert_true(dtype == BasicType::smallint,
                        "S16 does not match basic type");
            return 2;
        } else if constexpr (SameAs<TT, U8>) {
            assert_true(dtype == BasicType::boolean || dtype == BasicType::tinyint,
                        "U8 does not match basic type");
            return 1;
        } else if constexpr (SameAs<TT, F64>) {
            assert_true(dtype == BasicType::double_,
                        "F64 does not match basic type");
            return 8;
        } else if constexpr (SameAs<TT, F32>) {
            assert_true(dtype == BasicType::float_,
                        "F32 does not match basic type");
            return 4;
        } else if constexpr (SameAs<TT, AutoString8>) {
            assert_true(
                dtype == BasicType::ascii   ||
                dtype == BasicType::text    ||
                dtype == BasicType::varchar,
                "AutoString8 does not match basic type"
            );
            return value.length;
        } else if constexpr (SameAs<TT, UUID>) {
            assert_true(dtype == BasicType::uuid || dtype == BasicType::timeuuid, "UUID value does not match basic type");
            return UUID::length;
        } else if constexpr (SameAs<TT, Blob>) {
            assert_true(
                dtype == BasicType::blob    ||
                dtype == BasicType::inet    ||
                dtype == BasicType::varint  ||
                dtype == BasicType::decimal ||
                dtype == BasicType::duration||
                dtype == BasicType::vector  ||
                dtype == BasicType::hex,
                "Blob value does not match basic type"
            );
            return value.value.length;
        } else if constexpr (SameAs<TT, Hex>) {
            assert_true(dtype == BasicType::hex, "Hex value does not match basic type");
            return value.value.length;
        } else {
            static_assert(!SameAs<TT,TT>, "missing basic value type");
        }
    }

    template<typename T> concept IsCqlDA = IsDynamicArray<T>;
    template<typename T> concept IsCqlDS = IsDynamicSet<T>;
    template<typename T> concept IsCqlDM = IsDynamicMap<T>;

    // ========================================================================
    // input
    // ========================================================================
    U16 read_be_u16(const U8* p);
    S16 read_be_s16(const U8* p);
    S32 read_be_s32(const U8* p);
    S64 read_be_s64(const U8* p);

    // Read [long string]: [int] n + n bytes
    String8 read_cql_long_string(const U8*& p, const U8* end);

    // Read [string]: [short] n + n bytes
    String8 read_cql_string(const U8*& p, const U8* end);

    // Read [short bytes]: [short] n + n bytes, returns length and sets out_data
    U16 read_cql_short_bytes(const U8*& p, const U8* end, const U8*& out_data);

    Constant read_cql_value_as_constant(const U8*& p, const U8* end, BasicType dtype);

    // ========================================================================
    // output
    //   @note
    //   frame requires co_await send_native_frame(frame) writes the 9-byte
    //   header + body to TCP in and is currently heap buffered.
    // ========================================================================
    struct Frame {
        DynamicArray<U8> body;
        const tcp::Request* req;
        U8  op;
        S16 stream;
    };

    Frame make_native_frame(const tcp::Request* req, U8 op, S16 stream);
    coroutine::Task<> send_native_frame(Frame& f);

    void append_byte(Frame& f, U8 v);
    void append_bytes(Frame& f, const U8* data, U64 n);
    void append_be_u16(Frame& f, U16 v);
    void append_be_s32(Frame& f, S32 v);

    void append_cql_string(Frame& f, String8 s);
    void append_cql_short_bytes(Frame& f, const U8* data, U16 n);
    void append_cql_bytes_raw(Frame& f, const U8* data, S32 n);
    void append_cql_value(Frame& f, const ColumnValue& value, Type cdtype);

    template<typename T>
    void append_cql_basic_element(Frame& f, const BasicType& dtype, const T& v) {
        using TT = Decay<T>;

        if constexpr (SameAs<TT, AutoString8>) {
            assert_true(
                dtype == BasicType::ascii   ||
                dtype == BasicType::text    ||
                dtype == BasicType::varchar,
                "AutoString8 value does not match BasicType"
            );

            append_cql_bytes_raw(f, reinterpret_cast<const U8*>(v.c_str), S32(v.length));
        } else if constexpr (SameAs<TT, UUID>) {
            assert_true(dtype == BasicType::uuid || dtype == BasicType::timeuuid, "UUID value does not match BasicType");

            append_cql_bytes_raw(f, &v.value[0], v.length);
        } else if constexpr (SameAs<TT, S64>) {
            assert_true(
                dtype == BasicType::bigint ||
                dtype == BasicType::counter ||
                dtype == BasicType::timestamp ||
                dtype == BasicType::time,
                "S64 value does not match BasicType"
            );

            S64 vv = v;
            U8 data[8];
            for (int i = 7; i >= 0; i--) { data[i] = U8(vv); vv >>= 8; }
            append_cql_bytes_raw(f, data, 8);
        } else if constexpr (SameAs<TT, S32>) {
            assert_true(
                dtype == BasicType::int_ ||
                dtype == BasicType::date,
                "S32 value does not match BasicType"
            );
            U8 data[4] = { U8(U32(v) >> 24), U8(U32(v) >> 16), U8(U32(v) >> 8), U8(U32(v)) };
            append_cql_bytes_raw(f, data, 4);

        } else if constexpr (SameAs<TT, S16>) {
            assert_true(dtype == BasicType::smallint, "S16 value does not match BasicType");
            U8 data[2] = { U8(U16(v) >> 8), U8(v) };
            append_cql_bytes_raw(f, data, 2);

        } else if constexpr (SameAs<TT, U8>) {
            assert_true(
                dtype == BasicType::boolean ||
                dtype == BasicType::tinyint,
                "U8 value does not match BasicType"
            );
            append_cql_bytes_raw(f, &v, 1);

        } else if constexpr (SameAs<TT, F32>) {
            assert_true(dtype == BasicType::float_, "F32 value does not match BasicType");
            U32 bits; os::memory_copy(&bits, &v, sizeof(bits));
            U8 data[4] = { U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits) };
            append_cql_bytes_raw(f, data, 4);

        } else if constexpr (SameAs<TT, F64>) {
            assert_true(dtype == BasicType::double_, "F64 value does not match BasicType");
            U64 bits; os::memory_copy(&bits, &v, sizeof(bits));
            U8 data[8];
            for (int i = 7; i >= 0; i--) { data[i] = U8(bits); bits >>= 8; }
            append_cql_bytes_raw(f, data, 8);
        } else if constexpr (SameAs<TT, Blob>) {
            assert_true(
                dtype == BasicType::blob    ||
                dtype == BasicType::inet    ||
                dtype == BasicType::varint  ||
                dtype == BasicType::decimal ||
                dtype == BasicType::duration||
                dtype == BasicType::vector  ||
                dtype == BasicType::hex,
                "Blob value does not match BasicType"
            );
            append_cql_bytes_raw(f, v.value.ptr, S32(v.value.length));
        } else if constexpr (SameAs<TT, Hex>) {
            assert_true(dtype == BasicType::hex, "Hex value does not match BasicType");
            append_cql_bytes_raw(f, v.value.ptr, S32(v.value.length));
        } else {
            static_assert(!SameAs<TT, TT>, "unsupported static type");
        }
    }

    void append_type_codes_option(Frame& f, Type cdtype);
    void append_error_body(Frame& f, engine::ExecutionStatus status, String8 message);
    void append_result_void(Frame& f);
    void append_result_set_keyspace(Frame& f, String8 keyspace);
    void append_result_schema_change(Frame& f, String8 change_type, String8 target, String8 keyspace, String8 table);
    void append_result_rows(Frame& f, engine::ExecutionResult& result, schema::Table* tbl);
    void append_result_virtual_rows(Frame& f, engine::VirtualRows& vr);
    void append_result_prepared(Frame& f, U64 id, engine::PreparedEntry& entry);

    DynamicArray<Constant> read_query_parameter_values(const U8*& p, const U8* end, const DynamicArray<engine::BindVariableSpec>& bind_specs);
    coroutine::Task<bool> send_error_if_failed(engine::ExecutionResult& result, const tcp::Request* req, S16 stream);
    coroutine::Task<> send_execution_result(engine::ExecutionResult& result, engine::Engine& engine, const tcp::Request* req, S16 stream);

    // ========================================================================
    // handler
    // ========================================================================
    coroutine::Task<void> frame_handler(engine::Engine& engine, const tcp::Request& req, const U8* header, const U8* body, S32 body_length);
}

export namespace objstore::native {
    template<typename F>
    concept OnReady = requires(F f) { f(); };

    Optional<String8> run(U16 port, os::Notifier& interrupt, volatile bool& should_exit, engine::Engine& engine, OnReady auto&& on_ready_callback, bool use_uring = true) {
        const auto connection_handler = [&engine](const tcp::Request& req) -> coroutine::Task<void, coroutine::Start::Eager> { TracyFiberEnter("request")
            // process each frame @todo investigate avoiding allocations/copy in favour of direct lock on read buffer
            DynamicArray<U8> frame{};
            while (true) {
                S32 body_byte_count = MAX_S32;
                U64 frame_byte_count = MAX_U64;

                { ZoneScopedN("read")
                    // @perf avoid allocation for frames which fit in buffer
                    // @perf investigate holding one or multiple buffers for during of request
                    // @perf investigate shared read memory outside ring
                    const auto try_append_to_frame = [&]() -> coroutine::Task<bool> {
                        Optional<tcp::RWBuffer> opt_buffer = tcp::acquire(req);
                        if (!opt_buffer) {
                            log::native_error("buffer starvation, dropping read");
                            co_return false;
                        }
                        auto& buffer = *opt_buffer;
                        if (auto err = co_await tcp::read(req, &buffer); err != tcp::Error::None) {
                            log::native_error("error reading request, dropping connection");
                            tcp::release(req, &buffer);
                            co_return false;
                        }
                        resize(frame, frame.length + buffer.length);
                        os::memory_copy(frame.ptr + frame.length - buffer.length, buffer.view.ptr, buffer.length);
                        tcp::release(req, &buffer);
                        co_return true;
                    };

                    // read header
                    while (frame.length < FRAME_HEADER_BYTE_COUNT) {
                        if (!co_await try_append_to_frame())
                            break;
                    }
                    if (frame.length < FRAME_HEADER_BYTE_COUNT) break;

                    body_byte_count = read_be_s32(&frame[5]);
                    frame_byte_count = FRAME_HEADER_BYTE_COUNT + U64(body_byte_count);

                    // read body
                    while (frame.length < frame_byte_count) {
                        if (!co_await try_append_to_frame())
                            break;
                    }
                    if (frame.length < frame_byte_count) break;
                }

                co_await frame_handler(engine, req, frame.ptr, &frame.ptr[FRAME_HEADER_BYTE_COUNT], body_byte_count);

                // if next frame was partially read, move it to the front
                if (frame.length > frame_byte_count) {
                    os::memory_copy(frame.ptr, &frame.ptr[frame_byte_count], frame.length - frame_byte_count);
                }
                resize(frame, frame.length - frame_byte_count);
            }

            co_await tcp::close(req);
            TracyFiberLeave; co_return;
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

            tcp::Listener listener{socket, use_uring};
            tcp::listen(
                listener,
                connection_handler,
                interrupt, should_exit
            );
        }

        return {};
    }
}
