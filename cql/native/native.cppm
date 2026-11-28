module;
#include <plexdb/macros/macros.h>
#include <plexdb/support/tracy/tracy.hpp>
#include <coroutine>

export module cql.native;

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.aio;
import plexdb.dynamic.containers;
import plexdb.btree;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;
import plexdb.tcp;

import cql.crc;
import cql.parsers;
import cql.engine;
import cql.engine.column_value;
import cql.engine.statements;
import cql.engine.io;
import cql.native.wire;
import cql.log;
import cql.lz4;

import xxhash;

using namespace plexdb;
using namespace cql;
using namespace cql::column_types;

export namespace cql::native {
    // ========================================================================
    // protocol
    // ========================================================================
    constexpr U8  MIN_PROTOCOL_VERSION         = 3;
    constexpr U8  MAX_PROTOCOL_VERSION         = 5;
    constexpr U64 MAX_ENVELOPE_BODY_BYTE_COUNT = 256_u64 * 1024 * 1024;

    constexpr U64 V4_FRAME_HEADER_BYTE_COUNT = 9;

    constexpr U64 V5_MAX_PAYLOAD_BYTE_COUNT   = 131071u; // 17-bit max outer frame payload
    constexpr U64 V5_UNCOMP_HDR_BYTE_COUNT    = 3;
    constexpr U64 V5_COMP_HDR_BYTE_COUNT      = 5;
    constexpr U64 V5_HDR_CRC24_BYTE_COUNT     = 3;
    constexpr U64 V5_PAYLOAD_CRC32_BYTE_COUNT = 4;

    namespace op_codes {
        constexpr U8 ERROR          = 0x00;
        constexpr U8 STARTUP        = 0x01;
        constexpr U8 READY          = 0x02;
        constexpr U8 AUTHENTICATE   = 0x03;
        constexpr U8 OPTIONS        = 0x05;
        constexpr U8 SUPPORTED      = 0x06;
        constexpr U8 QUERY          = 0x07;
        constexpr U8 RESULT         = 0x08;
        constexpr U8 PREPARE        = 0x09;
        constexpr U8 EXECUTE        = 0x0A;
        constexpr U8 REGISTER       = 0x0B;
        constexpr U8 EVENT          = 0x0C;
        constexpr U8 BATCH          = 0x0D;
        constexpr U8 AUTH_CHALLENGE = 0x0E;
        constexpr U8 AUTH_RESPONSE  = 0x0F;
        constexpr U8 AUTH_SUCCESS   = 0x10;
    }

    namespace envelope_flags {
        constexpr U8 COMPRESSION    = 0x01;
        constexpr U8 TRACING        = 0x02;
        constexpr U8 CUSTOM_PAYLOAD = 0x04;
        constexpr U8 WARNING        = 0x08;
        constexpr U8 USE_BETA       = 0x10;
    }

    namespace error_codes {
        constexpr S32 SERVER_ERROR    = 0x0000;
        constexpr S32 PROTOCOL_ERROR  = 0x000A;
        constexpr S32 BAD_CREDENTIALS = 0x0100;
    }

    namespace event_types {
        constexpr U8 TOPOLOGY_CHANGE = 0x01;
        constexpr U8 STATUS_CHANGE   = 0x02;
        constexpr U8 SCHEMA_CHANGE   = 0x04;
    }

    constexpr String8 op_codes_to_str(const U8& op) {
        switch (op) {
            case op_codes::ERROR:
                return "ERROR";
            case op_codes::STARTUP:
                return "STARTUP";
            case op_codes::READY:
                return "READY";
            case op_codes::AUTHENTICATE:
                return "AUTHENTICATE";
            case op_codes::OPTIONS:
                return "OPTIONS";
            case op_codes::SUPPORTED:
                return "SUPPORTED";
            case op_codes::QUERY:
                return "QUERY";
            case op_codes::RESULT:
                return "RESULT";
            case op_codes::PREPARE:
                return "PREPARE";
            case op_codes::EXECUTE:
                return "EXECUTE";
            case op_codes::REGISTER:
                return "REGISTER";
            case op_codes::EVENT:
                return "EVENT";
            case op_codes::BATCH:
                return "BATCH";
            case op_codes::AUTH_CHALLENGE:
                return "AUTH_CHALLENGE";
            case op_codes::AUTH_RESPONSE:
                return "AUTH_RESPONSE";
            case op_codes::AUTH_SUCCESS:
                return "AUTH_SUCCESS";
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
        constexpr U16 Custom    = 0x0000;
        constexpr U16 Ascii     = 0x0001;
        constexpr U16 Bigint    = 0x0002;
        constexpr U16 Blob      = 0x0003;
        constexpr U16 Boolean   = 0x0004;
        constexpr U16 Counter   = 0x0005;
        constexpr U16 Decimal   = 0x0006;
        constexpr U16 Double    = 0x0007;
        constexpr U16 Float     = 0x0008;
        constexpr U16 Int       = 0x0009;
        constexpr U16 Timestamp = 0x000B;
        constexpr U16 Uuid      = 0x000C;
        constexpr U16 Varchar   = 0x000D;
        constexpr U16 Varint    = 0x000E;
        constexpr U16 Timeuuid  = 0x000F;
        constexpr U16 Inet      = 0x0010;
        constexpr U16 Date      = 0x0011;
        constexpr U16 Time      = 0x0012;
        constexpr U16 Smallint  = 0x0013;
        constexpr U16 Tinyint   = 0x0014;
        constexpr U16 Duration  = 0x0015;
        constexpr U16 List      = 0x0020;
        constexpr U16 Map       = 0x0021;
        constexpr U16 Set       = 0x0022;
        constexpr U16 Udt       = 0x0030;
        constexpr U16 Tuple     = 0x0031;
    }

    constexpr U16 basic_type_to_type_code(type::Basic dtype) {
        switch (dtype) {
            case type::Basic::ascii:
                return type_codes::Ascii;
            case type::Basic::bigint:
                return type_codes::Bigint;
            case type::Basic::blob:
                return type_codes::Blob;
            case type::Basic::boolean:
                return type_codes::Boolean;
            case type::Basic::counter:
                return type_codes::Counter;
            case type::Basic::date:
                return type_codes::Date;
            case type::Basic::decimal:
                return type_codes::Decimal;
            case type::Basic::double_:
                return type_codes::Double;
            case type::Basic::duration:
                return type_codes::Duration;
            case type::Basic::float_:
                return type_codes::Float;
            case type::Basic::inet:
                return type_codes::Inet;
            case type::Basic::int_:
                return type_codes::Int;
            case type::Basic::smallint:
                return type_codes::Smallint;
            case type::Basic::text:
                return type_codes::Varchar;
            case type::Basic::time:
                return type_codes::Time;
            case type::Basic::timestamp:
                return type_codes::Timestamp;
            case type::Basic::tinyint:
                return type_codes::Tinyint;
            case type::Basic::uuid:
                return type_codes::Uuid;
            case type::Basic::varchar:
                return type_codes::Varchar;
            case type::Basic::varint:
                return type_codes::Varint;
            case type::Basic::timeuuid:
                return type_codes::Timeuuid;
            case type::Basic::hex:
                // @note hex is a literal kind only, never a column or bind type
                break;
            case type::Basic::COUNT:
                break;
        }

        assert_true(false, "invalid basic type for native protocol type code");
        return 0x0000;
    }

    // ========================================================================
    // server / connection state
    // ========================================================================
    struct AuthCredential {
        AutoString8 username;
        AutoString8 password;
    };

    // @note an empty credential list disables authentication (AllowAllAuthenticator).
    struct ServerConfig {
        DynamicArray<AuthCredential> credentials;
    };

    // FIFO coroutine mutex serializing whole envelopes onto one socket.
    struct WriteLock {
        bool                                  held = false;
        DynamicArray<std::coroutine_handle<>> waiters;
    };

    coroutine::Task<> lock(WriteLock& l);
    void              unlock(WriteLock& l);

    struct Server;

    struct ConnectionState {
        U64                 id     = 0;
        const tcp::Request* req    = nullptr;
        Server*             server = nullptr;

        U8   version       = 4;
        bool started       = false; // STARTUP accepted
        bool framed        = false; // v5 outer framing active
        bool compressed    = false; // lz4 negotiated
        bool authenticated = false;
        bool write_failed  = false;
        U8   events        = 0; // event_types bitmask from REGISTER

        AutoString8 keyspace{""};
        WriteLock   write_lock;
    };

    struct Server {
        Engine*                        engine = nullptr;
        const ServerConfig*            config = nullptr;
        DynamicArray<ConnectionState*> connections;
        U64                            next_connection_id = 1;
    };

    coroutine::Task<void> handle_connection(Server& server, const tcp::Request& req);

    // ========================================================================
    // input
    //   @note every read is bounds checked. Malformed framing sets
    //   `protocol_error`, an ill-formed value sets `invalid_error`; after either,
    //   reads return zero values and never touch memory past `end`.
    // ========================================================================
    struct Reader {
        const U8*   p;
        const U8*   end;
        String8     protocol_error = "";
        String8     invalid_error  = "";
        AutoString8 error_storage  = {}; // @note backs a formatted invalid_error
    };

    bool reader_ok(const Reader& r);
    void reader_fail_protocol(Reader& r, String8 message);
    void reader_fail_invalid(Reader& r, String8 message);
    bool reader_need(Reader& r, U64 n);

    U8                   read_u8(Reader& r);
    U16                  read_u16(Reader& r);
    S32                  read_s32(Reader& r);
    S64                  read_s64(Reader& r);
    String8              read_string(Reader& r);
    String8              read_long_string(Reader& r);
    TArrayView<const U8> read_short_bytes(Reader& r);
    // [bytes]: returns the length (-1 null, -2 unset) and sets out_data when non-negative.
    S32 read_bytes(Reader& r, const U8*& out_data);

    // [value] decoded as a bind Term of the given type. v3 has no UNSET values.
    Term read_value_as_term(Reader& r, const type::Type& dtype, U8 version);

    U16 read_be_u16(const U8* p);
    S16 read_be_s16(const U8* p);
    S32 read_be_s32(const U8* p);
    S64 read_be_s64(const U8* p);

    struct QueryParameters {
        U16                consistency   = 0;
        U32                flags         = 0;
        DynamicArray<Term> values        = {};
        bool               skip_metadata = false;
        S32                page_size     = -1;
        bool               has_paging    = false;
        Optional<S64>      timestamp     = {};
        Optional<String8>  keyspace      = {};
    };

    // Reads <query_parameters> and checks the bound values against `bind_specs`.
    QueryParameters read_query_parameters(Reader& r, const DynamicArray<engine::BindVariableSpec>& bind_specs, U8 version);

    // Acquire a TCP buffer, read data into it, append to buf, then release.
    coroutine::Task<bool> try_append_tcp_read(const tcp::Request& req, DynamicArray<U8>& buf);

    // ========================================================================
    // output
    // ========================================================================
    struct Frame {
        DynamicArray<U8> body;
        ConnectionState* conn;
        U8               op;
        S16              stream;
    };

    void append_byte(Frame& f, U8 v);
    void append_bytes(Frame& f, const U8* data, U64 n);
    void append_be_u16(Frame& f, U16 v);
    void append_be_s32(Frame& f, S32 v);

    void append_cql_string(Frame& f, String8 s);
    void append_cql_short_bytes(Frame& f, const U8* data, U16 n);
    void append_cql_bytes_raw(Frame& f, const U8* data, S32 n);
    void append_cql_value(Frame& f, const ColumnValue& value, type::Type cdtype);

    void                 append_type_codes_option(Frame& f, type::Type cdtype);
    void                 append_error_body(Frame& f, engine::ExecutionStatus status, String8 message, String8 keyspace = "", String8 table = "");
    void                 append_unprepared_error(Frame& f, const U8* id, U16 id_len);
    void                 append_result_void(Frame& f);
    void                 append_result_set_keyspace(Frame& f, String8 keyspace);
    void                 append_schema_change_body(Frame& f, const engine::ExecutionResult& result);
    coroutine::Task<S32> append_result_rows(Frame& f, engine::ExecutionResult& result, schema::Table* tbl);
    void                 append_result_virtual_rows(Frame& f, engine::VirtualRows& vr);

    coroutine::Task<> send_block(const tcp::Request& req, const U8* data, U64 len);

    // Sends one envelope on f.conn using its negotiated version and framing. Holds the
    // connection's write lock for the whole envelope so concurrent writers never interleave.
    coroutine::Task<> send_frame(Frame& f);
}

export namespace cql::native {
    template<typename F>
    concept OnReady = requires(F f) { f(); };

    Optional<String8> run(
        U16 port, Engine& engine,
        const OnReady auto& on_ready_callback, bool use_uring,
        aio::EventConsumer& file_io_consumer, aio::EventConsumer& signal_consumer, os::Poll& io_poll,
        uring::IOBudgetOverride network_override = {},
        const ServerConfig&     config           = {}
    ) {
        Server     server{.engine = &engine, .config = &config, .connections = {}, .next_connection_id = 1};
        const auto connection_handler = [&server](const tcp::Request& req) -> coroutine::Task<void, coroutine::Start::Eager> {
            ZoneScopedN("request");
            co_await handle_connection(server, req);
            co_return;
        };

        {
            os::Socket socket{os::socket_open()};
            if (!socket) {
                return {"failed to open server socket"};
            }
            if (!os::socket_set_option(socket, os::SocketOption::Reuse, true)) {
                return {"failed to set reuse on server socket"};
            }
            if (!os::socket_bind(socket, port)) {
                return {"failed to bind server socket"};
            }
            if (!os::socket_listen(socket, 128)) {
                return {"failed to listen on server socket"};
            }

            auto tcp_server = tcp::create_tcp_server(socket, &connection_handler, io_poll, use_uring, network_override);

            on_ready_callback();

            aio::run_blocking_event_loop(io_poll, file_io_consumer, signal_consumer, tcp_server.consumer);
        }

        return {};
    }

}
