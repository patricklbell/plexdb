module;
#include <coroutine>

export module cql.test_helpers;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.threads;
import plexdb.pager;
import plexdb.coroutine;
import plexdb.dynamic.containers;

import plexdb.test.pager_helpers;
import plexdb.test.server_helpers;

import cql.engine;
import cql.native;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

export namespace cql::test {
    // ========================================================================
    // opcodes / result kinds
    // ========================================================================
    namespace op {
        constexpr U8 ERROR     = 0x00;
        constexpr U8 STARTUP   = 0x01;
        constexpr U8 READY     = 0x02;
        constexpr U8 OPTIONS   = 0x05;
        constexpr U8 SUPPORTED = 0x06;
        constexpr U8 QUERY     = 0x07;
        constexpr U8 RESULT    = 0x08;
        constexpr U8 PREPARE   = 0x09;
        constexpr U8 EXECUTE   = 0x0A;
        constexpr U8 BATCH     = 0x0D;
    }

    namespace result {
        constexpr S32 VOID          = 0x0001;
        constexpr S32 ROWS          = 0x0002;
        constexpr S32 SET_KEYSPACE  = 0x0003;
        constexpr S32 PREPARED      = 0x0004;
        constexpr S32 SCHEMA_CHANGE = 0x0005;
    }

    constexpr U8 RESPONSE_VERSION = 0x84;

    // ========================================================================
    // server fixture
    //   @note Pager/Engine are not movable in convenient ways here, so
    //   ServerFixture lives on the stack and is mutated by open/close.
    // ========================================================================
    struct ServerFixture {
        Handle db_file = zero_handle();
        Pager  pager;
        Engine engine;
        U16    port      = 0;
        U64    page_size = 4_kb;
    };

    U16 next_test_port();

    coroutine::Task<void> open_server(ServerFixture& f);
    void                  close_server(ServerFixture& f);

    Socket client_connect(U16 port, int timeout_ms = 2000);

    // ========================================================================
    // native frame I/O
    // ========================================================================
    struct Frame {
        DynamicArray<U8> body;
        U8               version  = 0;
        U8               flags    = 0;
        S16              stream   = 0;
        U8               opcode   = 0;
        S32              body_len = 0;
    };

    // Low-level byte encoders for building bodies and frames
    void append_u8(DynamicArray<U8>& buf, U8 v);
    void append_be_u16(DynamicArray<U8>& buf, U16 v);
    void append_be_s32(DynamicArray<U8>& buf, S32 v);
    void append_be_s64(DynamicArray<U8>& buf, S64 v);
    void append_be_f64(DynamicArray<U8>& buf, F64 v);
    void append_bytes(DynamicArray<U8>& buf, const U8* data, U64 n);

    void append_cql_string(DynamicArray<U8>& buf, String8 s);
    void append_cql_long_string(DynamicArray<U8>& buf, String8 s);
    void append_cql_short_bytes(DynamicArray<U8>& buf, const U8* data, U16 n);
    void append_cql_bytes(DynamicArray<U8>& buf, const U8* data, S32 n);

    // Wrap the buffer with the 9-byte v4 frame header
    void prepend_v4_header(DynamicArray<U8>& buf, U8 opcode, S16 stream);

    // Construct ready-to-send frames
    DynamicArray<U8> build_startup(S16 stream = 0);
    DynamicArray<U8> build_options(S16 stream = 0);
    DynamicArray<U8> build_query(String8 cql, S16 stream = 0);
    DynamicArray<U8> build_prepare(String8 cql, S16 stream = 0);

    void  send_frame(Socket& socket, const DynamicArray<U8>& frame);
    Frame recv_frame(Socket& socket);

    // Send-then-recv convenience
    Frame send_startup(Socket& socket, S16 stream = 0);
    Frame send_options(Socket& socket, S16 stream = 0);
    Frame send_query(Socket& socket, String8 cql, S16 stream = 0);
    Frame send_prepare(Socket& socket, String8 cql, S16 stream = 0);

    // Perform STARTUP and assert via assert_true that READY is returned. For
    // tests that do not care about the handshake exchange itself.
    void handshake(Socket& socket);

    // ========================================================================
    // frame inspection
    // ========================================================================
    S32     result_kind(const Frame& f);
    String8 body_str(const Frame& f);
    bool    body_contains(const Frame& f, String8 needle);
    bool    body_contains(const Frame& f, const U8* needle, U64 needle_len);
    // Returns the first offset where `needle` appears in the body, or MAX_U64 if absent.
    U64 body_index_of(const Frame& f, String8 needle);

    // Read prepared-statement id ([short bytes] starting at body offset 4).
    // The returned view points into f.body and is valid until f is destroyed.
    TArrayView<const U8> read_prepared_id(const Frame& f);

    // ========================================================================
    // typed bind-value helpers (CQL [bytes] encoding for EXECUTE / QUERY bodies)
    // ========================================================================
    void append_cql_bind_s32(DynamicArray<U8>& buf, S32 v);     // [bytes] of 4
    void append_cql_bind_s64(DynamicArray<U8>& buf, S64 v);     // [bytes] of 8
    void append_cql_bind_f64(DynamicArray<U8>& buf, F64 v);     // [bytes] of 8
    void append_cql_bind_str(DynamicArray<U8>& buf, String8 s); // [bytes] of s.length

    // Named-value prefix: [string] name then the bind value bytes
    void append_cql_named_s32(DynamicArray<U8>& buf, String8 name, S32 v);
    void append_cql_named_s64(DynamicArray<U8>& buf, String8 name, S64 v);
    void append_cql_named_f64(DynamicArray<U8>& buf, String8 name, F64 v);
    void append_cql_named_str(DynamicArray<U8>& buf, String8 name, String8 s);

    // ========================================================================
    // server runner
    //   The client lambda runs on a background thread, the server on the main
    //   (calling) thread. The client is responsible for calling
    //   signal_notify_safe(interrupt) before returning — without it the
    //   server will not shut down and the test will hang.
    // ========================================================================

    // Fixture-based variant (standard single-session tests)
    template<typename ClientFn>
    void run_native_server(ServerFixture& f, ClientFn&& client_fn) {
        run_server_test("cql-test-client", [&](Notifier& interrupt) {
                Socket client = client_connect(f.port);
                client_fn(client, interrupt); }, [&](auto on_ready, auto& signal_consumer, auto& poll) { native::run(f.port, f.engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll); });
    }

    // Engine+port variant for multi-session / WAL / crash tests where the
    // caller manages file handles and pager lifecycle manually.
    template<typename ClientFn>
    void run_native_server(Engine& engine, U16 port, ClientFn&& client_fn) {
        run_server_test("cql-test-client", [&](Notifier& interrupt) {
                Socket client = client_connect(port);
                client_fn(client, interrupt); }, [&](auto on_ready, auto& signal_consumer, auto& poll) { native::run(port, engine, on_ready, false, g_test_sync_consumer, signal_consumer, poll); });
    }

    // Handshake variants (perform STARTUP before handing socket to client_fn)
    template<typename ClientFn>
    void run_native_server_with_handshake(ServerFixture& f, ClientFn&& client_fn) {
        run_native_server(f, [&](Socket& client, Notifier& interrupt) {
            handshake(client);
            client_fn(client, interrupt);
        });
    }

    template<typename ClientFn>
    void run_native_server_with_handshake(Engine& engine, U16 port, ClientFn&& client_fn) {
        run_native_server(engine, port, [&](Socket& client, Notifier& interrupt) {
            handshake(client);
            client_fn(client, interrupt);
        });
    }
}
