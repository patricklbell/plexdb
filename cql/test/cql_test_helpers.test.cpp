module;
#include <atomic>
#include <coroutine>

module cql.test_helpers;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.pager;
import plexdb.coroutine;
import plexdb.dynamic.containers;

import plexdb.test.pager_helpers;

import cql.engine;
import cql.crc;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

namespace {
    constexpr int    PORT_BASE  = 23000;
    constexpr int    PORT_LIMIT = 27000;
    std::atomic<int> g_port_count{0};

    // Negotiated per-connection protocol version, set by handshake() and
    // consulted by send_frame/recv_frame. Each test's client runs on its own
    // dedicated background thread with exactly one connection, so thread_local
    // scoping matches the actual connection lifetime with no extra state to
    // thread through every call site.
    thread_local U8 g_conn_version = 4;

    // v5 outer-frame constants (uncompressed only — none of the test clients
    // request lz4). Mirrors native.cppm's internal constants; kept as a
    // small, self-contained copy here since those aren't exported from the
    // cql.native module (same rationale as the op-code/result-kind constants
    // already duplicated in this file).
    constexpr U64 V5_UNCOMP_HDR_BYTE_COUNT    = 3;
    constexpr U64 V5_HDR_CRC24_BYTE_COUNT     = 3;
    constexpr U64 V5_PAYLOAD_CRC32_BYTE_COUNT = 4;
    constexpr U64 V5_MAX_PAYLOAD_BYTE_COUNT   = 131071u;

    bool buf_contains(const U8* hay, U64 hay_len, const U8* needle, U64 needle_len) {
        if (needle_len == 0) {
            return true;
        }
        if (hay_len < needle_len) {
            return false;
        }
        for (U64 i = 0; i + needle_len <= hay_len; i++) {
            if (memory_compare(hay + i, needle, needle_len) == 0) {
                return true;
            }
        }
        return false;
    }

    bool recv_all(Socket& socket, U8* out, U64 byte_count) {
        U64 got = 0;
        while (got < byte_count) {
            auto res = socket_receive(socket, out + got, U32(byte_count - got));
            if (res.byte_count == 0) {
                return false;
            }
            got += res.byte_count;
        }
        return true;
    }
}

namespace cql::test {

    U16 next_test_port() {
        int n = g_port_count.fetch_add(1);
        return U16(PORT_BASE + (n % (PORT_LIMIT - PORT_BASE)));
    }

    coroutine::Task<void> open_server(ServerFixture& f) {
        f.port    = next_test_port();
        f.db_file = file_tmp();
        assert_true(!is_zero_handle(f.db_file), "open_server failed to create temp file");
        f.pager = create_test_pager(f.db_file, f.page_size);
        {
            pager::Transaction tx{&f.pager};
            co_await tx.begin();
            co_await engine::create_database(f.pager);
            co_await tx.commit();
        }
        co_await engine::init(f.engine, &f.pager);
    }

    void close_server(ServerFixture& f) {
        destroy_test_pager(f.pager);
        if (!is_zero_handle(f.db_file)) {
            file_close(f.db_file);
            f.db_file = zero_handle();
        }
    }

    Socket client_connect(U16 port, int timeout_ms) {
        Socket s{socket_open()};
        assert_true(bool(s), "client_connect failed to open socket");
        socket_set_timeout(s, timeout_ms);
        bool ok = socket_connect(s, "127.0.0.1", port);
        assert_true(ok, "client_connect failed to reach local server");
        return s;
    }

    // ============================================================================
    // encoders
    // ============================================================================
    void append_u8(DynamicArray<U8>& buf, U8 v) {
        push_back(buf, v);
    }
    void append_be_u16(DynamicArray<U8>& buf, U16 v) {
        push_back(buf, U8(v >> 8));
        push_back(buf, U8(v));
    }
    void append_be_s32(DynamicArray<U8>& buf, S32 v) {
        U32 u = U32(v);
        push_back(buf, U8(u >> 24));
        push_back(buf, U8(u >> 16));
        push_back(buf, U8(u >> 8));
        push_back(buf, U8(u));
    }
    void append_be_s64(DynamicArray<U8>& buf, S64 v) {
        U64 u = U64(v);
        for (int i = 7; i >= 0; i--) {
            push_back(buf, U8(u >> (i * 8)));
        }
    }
    void append_be_f64(DynamicArray<U8>& buf, F64 v) {
        U64 bits;
        memory_copy(&bits, &v, sizeof(bits));
        for (int i = 7; i >= 0; i--) {
            push_back(buf, U8(bits >> (i * 8)));
        }
    }
    void append_bytes(DynamicArray<U8>& buf, const U8* data, U64 n) {
        for (U64 i = 0; i < n; i++) {
            push_back(buf, data[i]);
        }
    }

    void append_cql_string(DynamicArray<U8>& buf, String8 s) {
        append_be_u16(buf, U16(s.length));
        append_bytes(buf, reinterpret_cast<const U8*>(s.data), s.length);
    }
    void append_cql_long_string(DynamicArray<U8>& buf, String8 s) {
        append_be_s32(buf, S32(s.length));
        append_bytes(buf, reinterpret_cast<const U8*>(s.data), s.length);
    }
    void append_cql_short_bytes(DynamicArray<U8>& buf, const U8* data, U16 n) {
        append_be_u16(buf, n);
        append_bytes(buf, data, n);
    }
    void append_cql_bytes(DynamicArray<U8>& buf, const U8* data, S32 n) {
        append_be_s32(buf, n);
        if (n > 0) {
            append_bytes(buf, data, U64(n));
        }
    }

    void prepend_frame_header(DynamicArray<U8>& buf, U8 opcode, S16 stream, U8 version) {
        S32 body_len = S32(buf.length);
        U8  hdr[9]   = {
            version,
            0x00,
            U8(U16(stream) >> 8),
            U8(stream),
            opcode,
            U8(U32(body_len) >> 24),
            U8(U32(body_len) >> 16),
            U8(U32(body_len) >> 8),
            U8(U32(body_len)),
        };

        DynamicArray<U8> out;
        reserve(out, 9 + buf.length);
        append_bytes(out, hdr, 9);
        append_bytes(out, buf.ptr, buf.length);
        buf = move(out);
    }

    void prepend_v4_header(DynamicArray<U8>& buf, U8 opcode, S16 stream) {
        prepend_frame_header(buf, opcode, stream, 0x04);
    }

    // ============================================================================
    // frame builders
    // ============================================================================
    DynamicArray<U8> build_startup(S16 stream, U8 version) {
        DynamicArray<U8> b;
        append_be_u16(b, 1);
        append_cql_string(b, String8("CQL_VERSION"));
        append_cql_string(b, String8("3.0.0"));
        prepend_frame_header(b, op::STARTUP, stream, version);
        return b;
    }

    DynamicArray<U8> build_options(S16 stream) {
        DynamicArray<U8> b;
        prepend_v4_header(b, op::OPTIONS, stream);
        return b;
    }

    DynamicArray<U8> build_query(String8 cql, S16 stream) {
        DynamicArray<U8> b;
        append_cql_long_string(b, cql);
        append_be_u16(b, 0x0001); // consistency = ONE
        // flags: [byte] in v4, [int] in v5 (native.cpp read_query_parameter_values)
        if (g_conn_version >= 5) {
            append_be_s32(b, 0x00);
        } else {
            append_u8(b, 0x00);
        }
        prepend_frame_header(b, op::QUERY, stream, g_conn_version);
        return b;
    }

    DynamicArray<U8> build_prepare(String8 cql, S16 stream) {
        DynamicArray<U8> b;
        append_cql_long_string(b, cql);
        // v5 adds an optional flags [int] field here (native.cpp PREPARE
        // handler); the server tolerates it being absent (only reads it if
        // 4 bytes remain), so v4-shaped bodies still work under v5.
        prepend_frame_header(b, op::PREPARE, stream, g_conn_version);
        return b;
    }

    // ============================================================================
    // frame I/O
    // ============================================================================
    // Always plain v4-style envelope on the wire — used directly for
    // STARTUP/OPTIONS (never v5-wrapped, per protocol spec) and as the
    // underlying transport once a v5 outer frame's payload is unwrapped.
    static void send_frame_raw(Socket& socket, const DynamicArray<U8>& frame) {
        socket_send_all(socket, frame.ptr, U32(frame.length));
    }

    static Frame parse_envelope(const U8* data, U64 len) {
        Frame f;
        if (len < 9) {
            return f;
        }
        f.version  = data[0];
        f.flags    = data[1];
        f.stream   = S16((U16(data[2]) << 8) | data[3]);
        f.opcode   = data[4];
        f.body_len = S32((U32(data[5]) << 24) | (U32(data[6]) << 16) | (U32(data[7]) << 8) | data[8]);

        if (f.body_len > 0 && len >= 9 + U64(f.body_len)) {
            resize(f.body, U64(f.body_len));
            memory_copy(f.body.ptr, data + 9, U64(f.body_len));
        }
        return f;
    }

    static Frame recv_frame_raw(Socket& socket) {
        U8 hdr[9] = {};
        if (!recv_all(socket, hdr, 9)) {
            return Frame{};
        }
        S32 body_len = S32((U32(hdr[5]) << 24) | (U32(hdr[6]) << 16) | (U32(hdr[7]) << 8) | hdr[8]);

        DynamicArray<U8> envelope;
        resize(envelope, 9 + U64(body_len > 0 ? body_len : 0));
        memory_copy(envelope.ptr, hdr, 9);
        if (body_len > 0 && !recv_all(socket, envelope.ptr + 9, U64(body_len))) {
            return Frame{};
        }
        return parse_envelope(envelope.ptr, envelope.length);
    }

    // Wrap `envelope` (a full v4-style header+body) in one uncompressed v5
    // outer frame and send it. Test payloads never exceed
    // V5_MAX_PAYLOAD_BYTE_COUNT, so a single self-contained frame suffices —
    // no chunking to mirror here.
    static void send_v5_outer_frame(Socket& socket, const U8* envelope, U64 len) {
        assert_true(len <= V5_MAX_PAYLOAD_BYTE_COUNT, "send_v5_outer_frame: payload too large for a single frame");
        U32 pl                                                      = U32(len);
        U8  bits[3]                                                 = {U8(pl), U8(pl >> 8), U8(U8(pl >> 16 & 0x01u) | U8(1u << 1))}; // self_contained=1
        U32 hcrc                                                    = crc::crc24(bits, 3);
        U8  hdr[V5_UNCOMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT] = {
            bits[0], bits[1], bits[2],
            U8(hcrc), U8(hcrc >> 8), U8(hcrc >> 16)
        };
        U32 pcrc       = crc::crc32(envelope, len, crc::CRC32_CQL_V5_INIT);
        U8  trailer[4] = {U8(pcrc), U8(pcrc >> 8), U8(pcrc >> 16), U8(pcrc >> 24)};

        socket_send_all(socket, hdr, U32(sizeof(hdr)));
        socket_send_all(socket, envelope, U32(len));
        socket_send_all(socket, trailer, U32(sizeof(trailer)));
    }

    // Reads v5 outer frames until a self-contained one completes the
    // envelope, verifying both CRCs the same way the server does, then
    // parses the reassembled envelope. Returns an empty Frame on any framing
    // or CRC failure.
    static Frame recv_v5_frame(Socket& socket) {
        DynamicArray<U8> envelope;
        bool             self_contained = false;
        do {
            U8 hdr[V5_UNCOMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT] = {};
            if (!recv_all(socket, hdr, sizeof(hdr))) {
                return Frame{};
            }
            U32 hdr_crc_computed = crc::crc24(hdr, V5_UNCOMP_HDR_BYTE_COUNT);
            U32 hdr_crc_stored   = U32(hdr[V5_UNCOMP_HDR_BYTE_COUNT]) | (U32(hdr[V5_UNCOMP_HDR_BYTE_COUNT + 1]) << 8) | (U32(hdr[V5_UNCOMP_HDR_BYTE_COUNT + 2]) << 16);
            if (hdr_crc_computed != hdr_crc_stored) {
                return Frame{};
            }

            U32 payload_len = U32(hdr[0]) | (U32(hdr[1]) << 8) | ((U32(hdr[2]) & 0x01u) << 16);
            self_contained  = bool((hdr[2] >> 1) & 1u);

            DynamicArray<U8> payload;
            resize(payload, U64(payload_len) + V5_PAYLOAD_CRC32_BYTE_COUNT);
            if (!recv_all(socket, payload.ptr, payload.length)) {
                return Frame{};
            }
            U32 payload_crc_computed = crc::crc32(payload.ptr, payload_len, crc::CRC32_CQL_V5_INIT);
            U32 payload_crc_stored   = U32(payload.ptr[payload_len]) | (U32(payload.ptr[payload_len + 1]) << 8) | (U32(payload.ptr[payload_len + 2]) << 16) | (U32(payload.ptr[payload_len + 3]) << 24);
            if (payload_crc_computed != payload_crc_stored) {
                return Frame{};
            }

            U64 old_len = envelope.length;
            resize(envelope, old_len + payload_len);
            memory_copy(envelope.ptr + old_len, payload.ptr, payload_len);
        } while (!self_contained);

        return parse_envelope(envelope.ptr, envelope.length);
    }

    // Version-aware public API — transparently v5-wraps once handshake()
    // has negotiated protocol 5 on this thread, otherwise identical to the
    // raw v4 path every existing test already relies on.
    void send_frame(Socket& socket, const DynamicArray<U8>& frame) {
        if (g_conn_version >= 5) {
            send_v5_outer_frame(socket, frame.ptr, frame.length);
        } else {
            send_frame_raw(socket, frame);
        }
    }

    Frame recv_frame(Socket& socket) {
        return g_conn_version >= 5 ? recv_v5_frame(socket) : recv_frame_raw(socket);
    }

    Frame send_startup(Socket& socket, S16 stream, U8 version) {
        send_frame_raw(socket, build_startup(stream, version));
        return recv_frame_raw(socket);
    }
    Frame send_options(Socket& socket, S16 stream) {
        send_frame_raw(socket, build_options(stream));
        return recv_frame_raw(socket);
    }
    Frame send_query(Socket& socket, String8 cql, S16 stream) {
        send_frame(socket, build_query(cql, stream));
        return recv_frame(socket);
    }
    Frame send_prepare(Socket& socket, String8 cql, S16 stream) {
        send_frame(socket, build_prepare(cql, stream));
        return recv_frame(socket);
    }

    void handshake(Socket& socket, U8 version) {
        Frame ready = send_startup(socket, 0, version);
        assert_true(ready.version == (0x80u | version), "handshake: bad response version");
        assert_true(ready.opcode == op::READY, "handshake: expected READY");
        g_conn_version = version;
    }

    // ============================================================================
    // frame inspection
    // ============================================================================
    S32 result_kind(const Frame& f) {
        if (f.body.length < 4) {
            return -1;
        }
        return S32((U32(f.body[0]) << 24) | (U32(f.body[1]) << 16) | (U32(f.body[2]) << 8) | U32(f.body[3]));
    }

    String8 body_str(const Frame& f) {
        return String8(reinterpret_cast<const char*>(f.body.ptr), f.body.length);
    }

    bool body_contains(const Frame& f, String8 needle) {
        return buf_contains(f.body.ptr, f.body.length, reinterpret_cast<const U8*>(needle.data), needle.length);
    }
    bool body_contains(const Frame& f, const U8* needle, U64 needle_len) {
        return buf_contains(f.body.ptr, f.body.length, needle, needle_len);
    }
    U64 body_index_of(const Frame& f, String8 needle) {
        if (needle.length == 0) {
            return 0;
        }
        if (f.body.length < needle.length) {
            return MAX_U64;
        }
        const U8* n = reinterpret_cast<const U8*>(needle.data);
        for (U64 i = 0; i + needle.length <= f.body.length; i++) {
            if (memory_compare(f.body.ptr + i, n, needle.length) == 0) {
                return i;
            }
        }
        return MAX_U64;
    }

    TArrayView<const U8> read_prepared_id(const Frame& f) {
        assert_true(f.body.length >= 6, "read_prepared_id: body too short");
        const U8* p      = f.body.ptr + 4;
        U16       id_len = U16((U16(p[0]) << 8) | p[1]);
        assert_true(f.body.length >= U64(6 + id_len), "read_prepared_id: id length runs past body");
        return TArrayView<const U8>{p + 2, U64(id_len)};
    }

    // ============================================================================
    // typed bind-value helpers
    // ============================================================================
    void append_cql_bind_s32(DynamicArray<U8>& buf, S32 v) {
        U8 b[4] = {U8(U32(v) >> 24), U8(U32(v) >> 16), U8(U32(v) >> 8), U8(U32(v))};
        append_cql_bytes(buf, b, 4);
    }
    void append_cql_bind_s64(DynamicArray<U8>& buf, S64 v) {
        U8  b[8];
        U64 u = U64(v);
        for (int i = 7; i >= 0; i--) {
            b[i] = U8(u >> ((7 - i) * 8));
        }
        append_cql_bytes(buf, b, 8);
    }
    void append_cql_bind_f64(DynamicArray<U8>& buf, F64 v) {
        U64 bits;
        memory_copy(&bits, &v, 8);
        U8 b[8];
        for (int i = 7; i >= 0; i--) {
            b[i] = U8(bits >> ((7 - i) * 8));
        }
        append_cql_bytes(buf, b, 8);
    }
    void append_cql_bind_str(DynamicArray<U8>& buf, String8 s) {
        append_cql_bytes(buf, reinterpret_cast<const U8*>(s.data), S32(s.length));
    }

    void append_cql_named_s32(DynamicArray<U8>& buf, String8 name, S32 v) {
        append_cql_string(buf, name);
        append_cql_bind_s32(buf, v);
    }
    void append_cql_named_s64(DynamicArray<U8>& buf, String8 name, S64 v) {
        append_cql_string(buf, name);
        append_cql_bind_s64(buf, v);
    }
    void append_cql_named_f64(DynamicArray<U8>& buf, String8 name, F64 v) {
        append_cql_string(buf, name);
        append_cql_bind_f64(buf, v);
    }
    void append_cql_named_str(DynamicArray<U8>& buf, String8 name, String8 s) {
        append_cql_string(buf, name);
        append_cql_bind_str(buf, s);
    }

}
