module;
#include "macros.h"
#include <profiling/tracy.hpp>
#include <coroutine>

export module objstore.native;

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.aio;
import plexdb.dynamic.containers;
import plexdb.btree;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;
import xxhash;

import objstore.crc;
import objstore.tcp;
import objstore.parsers;
import objstore.engine;
import objstore.engine.statements;
import objstore.engine.io;
import objstore.log;
import objstore.lz4;

using namespace plexdb;
using namespace objstore;

namespace objstore::native {
    // ========================================================================
    // protocol
    // ========================================================================
    struct NegotiatedProtocol {
        U8   version    = 4;
        bool compressed = false;
    };

    constexpr U64 V4_FRAME_HEADER_BYTE_COUNT = 9;

    constexpr U64 V5_MAX_PAYLOAD_BYTE_COUNT   = 131071u; // 17-bit max outer frame payload
    constexpr U64 V5_UNCOMP_HDR_BYTE_COUNT    = 3;
    constexpr U64 V5_COMP_HDR_BYTE_COUNT      = 5;
    constexpr U64 V5_HDR_CRC24_BYTE_COUNT     = 3;
    constexpr U64 V5_PAYLOAD_CRC32_BYTE_COUNT = 4;

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

    namespace error_codes {
        constexpr S32 PROTOCOL_ERROR    = 0x000A;
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

    // Acquire a TCP buffer, read data into it, append to buf, then release.
    coroutine::Task<bool> try_append_tcp_read(const tcp::Request& req, DynamicArray<U8>& buf);

    // ========================================================================
    // output
    //   @note
    //   frame requires co_await send_native_frame<V,C>(frame) which writes a
    //   version-appropriate envelope (v4: plain 9-byte header + body;
    //   v5: v4 envelope wrapped in outer frames with CRC24/CRC32).
    // ========================================================================
    struct Frame {
        DynamicArray<U8> body;
        const tcp::Request* req;
        U8   op;
        S16  stream;
    };

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
    coroutine::Task<> append_result_rows(Frame& f, engine::ExecutionResult& result, schema::Table* tbl);
    void append_result_virtual_rows(Frame& f, engine::VirtualRows& vr);

    // ========================================================================
    // send helpers
    // ========================================================================
    coroutine::Task<> send_block(const tcp::Request& req, const U8* data, U64 len);

    // Wrap data in v5 outer frame(s) and send. Splits into V5_MAX_PAYLOAD chunks
    // when needed, setting isSelfContained on the final fragment only.
    template<bool Compressed>
    coroutine::Task<> send_v5_outer_frames(const tcp::Request& req, const U8* data, U64 data_len) {
        U64 offset = 0;
        do {
            U64 chunk = min(data_len - offset, V5_MAX_PAYLOAD_BYTE_COUNT);
            bool self_contained = (offset + chunk >= data_len);

            if constexpr (Compressed) {
                S32 bound    = lz4::compress_bound(S32(chunk));
                DynamicArray<U8> cbuf{};
                resize(cbuf, U64(bound));
                S32 comp_len = lz4::compress(data + offset, cbuf.ptr, S32(chunk), bound);
                assert_true(comp_len > 0, "LZ4 compression failed in send_v5_outer_frames");

                U32 c = U32(comp_len), u = U32(chunk);
                U8 bits[5] = {
                    U8(c),
                    U8(c >> 8),
                    U8(U8(c >> 16 & 0x01u) | U8((u & 0x7Fu) << 1)),
                    U8(u >> 7),
                    U8(U8(u >> 15 & 0x03u) | U8(U8(self_contained) << 2))
                };
                U32 hcrc = crc::crc24(bits, 5);
                U8 hdr[V5_COMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT] = {
                    bits[0], bits[1], bits[2], bits[3], bits[4],
                    U8(hcrc), U8(hcrc >> 8), U8(hcrc >> 16)
                };
                U32 pcrc = crc::crc32(cbuf.ptr, U64(comp_len), crc::CRC32_CQL_V5_INIT);
                U8 trailer[4] = { U8(pcrc), U8(pcrc >> 8), U8(pcrc >> 16), U8(pcrc >> 24) };
                co_await send_block(req, hdr, V5_COMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT);
                co_await send_block(req, cbuf.ptr, U64(comp_len));
                co_await send_block(req, trailer, 4);
            } else {
                U32 pl = U32(chunk);
                U8 bits[3] = {
                    U8(pl), U8(pl >> 8),
                    U8(U8(pl >> 16 & 0x01u) | U8(U8(self_contained) << 1))
                };
                U32 hcrc = crc::crc24(bits, 3);
                U8 hdr[V5_UNCOMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT] = {
                    bits[0], bits[1], bits[2],
                    U8(hcrc), U8(hcrc >> 8), U8(hcrc >> 16)
                };
                U32 pcrc = crc::crc32(data + offset, chunk, crc::CRC32_CQL_V5_INIT);
                U8 trailer[4] = { U8(pcrc), U8(pcrc >> 8), U8(pcrc >> 16), U8(pcrc >> 24) };
                co_await send_block(req, hdr, V5_UNCOMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT);
                co_await send_block(req, data + offset, chunk);
                co_await send_block(req, trailer, 4);
            }

            offset += chunk;
        } while (offset < data_len);
    }

    // ========================================================================
    // handler
    // ========================================================================
    coroutine::Task<bool> negotiate_connection(const tcp::Request& req, NegotiatedProtocol* opt_out_negotiated_protocol);

    // Defined in native.cpp; explicitly instantiated for (5,true), (5,false), (4,false).
    template<U8 Version, bool Compressed>
    coroutine::Task<void> frame_handler(engine::Engine& engine, const tcp::Request& req, const U8* header, const U8* body, S32 body_length);

    extern template coroutine::Task<void> frame_handler<5u, true> (engine::Engine&, const tcp::Request&, const U8*, const U8*, S32);
    extern template coroutine::Task<void> frame_handler<5u, false>(engine::Engine&, const tcp::Request&, const U8*, const U8*, S32);
    extern template coroutine::Task<void> frame_handler<4u, false>(engine::Engine&, const tcp::Request&, const U8*, const U8*, S32);

    template<U8 Version, bool Compressed>
    coroutine::Task<void> post_startup_loop(engine::Engine& engine, const tcp::Request& req) {
        if constexpr (Version >= 5) {
            DynamicArray<U8> read_buf{};
            DynamicArray<U8> envelope_buf{};

            // @todo refactor to be common
            const auto fill_to = [&](U64 n) -> coroutine::Task<bool> {
                while (read_buf.length < n) {
                    if (!co_await try_append_tcp_read(req, read_buf)) co_return false;
                }
                co_return true;
            };

            // @todo @perf remove
            const auto consume_front = [&](U64 n) {
                U64 rem = read_buf.length - n;
                if (rem > 0) os::memory_move(read_buf.ptr, read_buf.ptr + n, rem);
                resize(read_buf, rem);
            };

            constexpr U64 V5_HDR_BYTE_COUNT = Compressed ? V5_COMP_HDR_BYTE_COUNT : V5_UNCOMP_HDR_BYTE_COUNT;
            constexpr U64 V5_HDR_WITH_CRC24_BYTE_COUNT = V5_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT;

            while (true) {
                envelope_buf.length = 0;
                bool self_contained  = false;
                bool ok              = true;

                { ZoneScopedN("post_startup_loop::read")
                    // Accumulate outer frames until a self-contained message is assembled
                    while (!self_contained) {
                        if (!co_await fill_to(V5_HDR_WITH_CRC24_BYTE_COUNT)) { ok = false; break; }

                        // Verify header CRC24
                        U32 hdr_crc_computed = crc::crc24(read_buf.ptr, V5_HDR_BYTE_COUNT);
                        U32 hdr_crc_stored   = U32(read_buf[V5_HDR_BYTE_COUNT])
                                            | (U32(read_buf[V5_HDR_BYTE_COUNT + 1]) << 8)
                                            | (U32(read_buf[V5_HDR_BYTE_COUNT + 2]) << 16);
                        if (hdr_crc_computed != hdr_crc_stored) {
                            log::native_error("v5 frame header CRC24 mismatch, dropping connection");
                            ok = false; break;
                        }

                        U32 payload_len = 0, uncomp_len = 0;
                        if constexpr (Compressed) {
                            payload_len    = U32(read_buf[0]) | (U32(read_buf[1]) << 8) | ((U32(read_buf[2]) & 0x01u) << 16);
                            uncomp_len     = ((U32(read_buf[2]) >> 1) & 0x7Fu) | (U32(read_buf[3]) << 7) | ((U32(read_buf[4]) & 0x03u) << 15);
                            self_contained = bool((read_buf[4] >> 2) & 1u);
                        } else {
                            payload_len    = U32(read_buf[0]) | (U32(read_buf[1]) << 8) | ((U32(read_buf[2]) & 0x01u) << 16);
                            self_contained = bool((read_buf[2] >> 1) & 1u);
                        }
                        consume_front(V5_HDR_WITH_CRC24_BYTE_COUNT);

                        if (!co_await fill_to(payload_len + V5_PAYLOAD_CRC32_BYTE_COUNT)) { ok = false; break; }

                        // Verify CRC32 of payload
                        {
                            U32 payload_crc_computed = crc::crc32(read_buf.ptr, payload_len, crc::CRC32_CQL_V5_INIT);
                            U32 payload_crc_stored   = U32(read_buf[payload_len])
                                                    | (U32(read_buf[payload_len + 1]) << 8)
                                                    | (U32(read_buf[payload_len + 2]) << 16)
                                                    | (U32(read_buf[payload_len + 3]) << 24);
                            if (payload_crc_computed != payload_crc_stored) {
                                log::native_error("v5 frame payload CRC32 mismatch, dropping connection");
                                ok = false; break;
                            }
                        }

                        if constexpr (Compressed) {
                            U64 old_len = envelope_buf.length;
                            resize(envelope_buf, old_len + uncomp_len);
                            S32 result = lz4::decompress(
                                read_buf.ptr, envelope_buf.ptr + old_len,
                                S32(payload_len), S32(uncomp_len)
                            );
                            if (result != S32(uncomp_len)) {
                                log::native_error("v5 frame LZ4 decompression failed, dropping connection");
                                ok = false; break;
                            }
                        } else {
                            U64 old_len = envelope_buf.length;
                            resize(envelope_buf, old_len + payload_len);
                            os::memory_copy(envelope_buf.ptr + old_len, read_buf.ptr, payload_len);
                        }
                        consume_front(payload_len + V5_PAYLOAD_CRC32_BYTE_COUNT);
                    }
                }

                if (!ok) break;

                // Dispatch all inner envelopes from the assembled payload
                const U8* p   = envelope_buf.ptr;
                const U8* end = p + envelope_buf.length;
                while (p + V4_FRAME_HEADER_BYTE_COUNT <= end) {
                    S32 inner_body_len = read_be_s32(p + 5);
                    if (inner_body_len < 0) break;
                    if (p + V4_FRAME_HEADER_BYTE_COUNT + U64(inner_body_len) > end) break;
                    co_await frame_handler<Version, Compressed>(engine, req, p, p + V4_FRAME_HEADER_BYTE_COUNT, inner_body_len);
                    p += V4_FRAME_HEADER_BYTE_COUNT + inner_body_len;
                }
            }
        } else {
            // v4: plain envelope framing (no outer wrapping)
            DynamicArray<U8> frame{};
            S32 body_byte_count  = MAX_S32;
            U64 frame_byte_count = MAX_U64;
            while (true) {
                body_byte_count  = MAX_S32;
                frame_byte_count = MAX_U64;

                { ZoneScopedN("post_startup_loop::read")
                    while (frame.length < V4_FRAME_HEADER_BYTE_COUNT) {
                        if (!co_await try_append_tcp_read(req, frame)) break;
                    }
                    if (frame.length < V4_FRAME_HEADER_BYTE_COUNT) break;

                    body_byte_count  = read_be_s32(&frame[5]);
                    frame_byte_count = V4_FRAME_HEADER_BYTE_COUNT + U64(body_byte_count);

                    while (frame.length < frame_byte_count) {
                        if (!co_await try_append_tcp_read(req, frame)) break;
                    }
                    if (frame.length < frame_byte_count) break;
                }

                co_await frame_handler<Version, Compressed>(engine, req, frame.ptr, &frame.ptr[V4_FRAME_HEADER_BYTE_COUNT], body_byte_count);

                if (frame.length > frame_byte_count) {
                    os::memory_copy(frame.ptr, &frame.ptr[frame_byte_count], frame.length - frame_byte_count);
                }
                resize(frame, frame.length - frame_byte_count);
            }
        }
    }
}

export namespace objstore::native {
    template<typename F>
    concept OnReady = requires(F f) { f(); };

    Optional<String8> run(U16 port, os::Notifier& interrupt, volatile bool& should_exit, engine::Engine& engine, OnReady auto&& on_ready_callback, bool use_uring = true) {
        const auto connection_handler = [&engine](const tcp::Request& req) -> coroutine::Task<void, coroutine::Start::Eager> { TracyFiberEnter("request")
            NegotiatedProtocol negotiated_protocol{};
            bool ok = co_await negotiate_connection(req, &negotiated_protocol);
            if (!ok) {
                co_await tcp::close(req);
                TracyFiberLeave; co_return;
            }

            if (negotiated_protocol.version == 5) {
                if (negotiated_protocol.compressed)
                    co_await post_startup_loop<5, true>(engine, req);
                else
                    co_await post_startup_loop<5, false>(engine, req);
            } else {
                co_await post_startup_loop<4, false>(engine, req);
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
