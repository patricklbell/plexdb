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

import plexdb.pager.test_helpers;

import cql.engine;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

namespace {
    constexpr int    PORT_BASE  = 23000;
    constexpr int    PORT_LIMIT = 27000;
    std::atomic<int> g_port_count{0};

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

    void prepend_v4_header(DynamicArray<U8>& buf, U8 opcode, S16 stream) {
        S32 body_len = S32(buf.length);
        U8  hdr[9]   = {
            0x04,
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

    // ============================================================================
    // frame builders
    // ============================================================================
    DynamicArray<U8> build_startup(S16 stream) {
        DynamicArray<U8> b;
        append_be_u16(b, 1);
        append_cql_string(b, String8("CQL_VERSION"));
        append_cql_string(b, String8("3.0.0"));
        prepend_v4_header(b, op::STARTUP, stream);
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
        append_u8(b, 0x00);       // flags
        prepend_v4_header(b, op::QUERY, stream);
        return b;
    }

    DynamicArray<U8> build_prepare(String8 cql, S16 stream) {
        DynamicArray<U8> b;
        append_cql_long_string(b, cql);
        prepend_v4_header(b, op::PREPARE, stream);
        return b;
    }

    // ============================================================================
    // frame I/O
    // ============================================================================
    void send_frame(Socket& socket, const DynamicArray<U8>& frame) {
        socket_send_all(socket, frame.ptr, U32(frame.length));
    }

    Frame recv_frame(Socket& socket) {
        Frame f;
        U8    hdr[9] = {};
        if (!recv_all(socket, hdr, 9)) {
            return f;
        }

        f.version  = hdr[0];
        f.flags    = hdr[1];
        f.stream   = S16((U16(hdr[2]) << 8) | hdr[3]);
        f.opcode   = hdr[4];
        f.body_len = S32((U32(hdr[5]) << 24) | (U32(hdr[6]) << 16) | (U32(hdr[7]) << 8) | hdr[8]);

        if (f.body_len > 0) {
            resize(f.body, U64(f.body_len));
            if (!recv_all(socket, f.body.ptr, U64(f.body_len))) {
                f.body.length = 0;
            }
        }
        return f;
    }

    Frame send_startup(Socket& socket, S16 stream) {
        send_frame(socket, build_startup(stream));
        return recv_frame(socket);
    }
    Frame send_options(Socket& socket, S16 stream) {
        send_frame(socket, build_options(stream));
        return recv_frame(socket);
    }
    Frame send_query(Socket& socket, String8 cql, S16 stream) {
        send_frame(socket, build_query(cql, stream));
        return recv_frame(socket);
    }
    Frame send_prepare(Socket& socket, String8 cql, S16 stream) {
        send_frame(socket, build_prepare(cql, stream));
        return recv_frame(socket);
    }

    void handshake(Socket& socket) {
        Frame ready = send_startup(socket);
        assert_true(ready.version == RESPONSE_VERSION, "handshake: bad response version");
        assert_true(ready.opcode == op::READY, "handshake: expected READY");
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
