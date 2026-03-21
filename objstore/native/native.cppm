module;
#include "macros.h"

export module objstore.native;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.btree;
import plexdb.tagged_union;

import objstore.tcp;
import objstore.parsers;
import objstore.engine;
import objstore.engine.statements;
import objstore.log;

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
        constexpr S32 SCHEMA_CHANGE = 0x0005;
    }

    namespace type_codes {
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

    constexpr U16 native_type_to_type_code(NativeType dtype) {
        switch (dtype) {
            case types::text:       return type_codes::Varchar;
            case types::uuid:       return type_codes::Uuid;
            case types::timestamp:  return type_codes::Timestamp;
            case types::smallint:   return type_codes::Smallint;
            case types::int_:       return type_codes::Int;
            case types::bigint:     return type_codes::Bigint;
            case types::counter:    return type_codes::Counter;
            case types::boolean:    return type_codes::Boolean;
            case types::float_:     return type_codes::Float;
            case types::double_:    return type_codes::Double;
            case types::ascii:
            case types::blob:
            case types::date:
            case types::decimal:
            case types::duration:
            case types::inet:
            case types::time:
            case types::timeuuid:
            case types::tinyint:
            case types::varchar:
            case types::varint:
            case types::vector:{
                assert_not_implemented("native protocol type code for native type is not implemented");
                return 0x0000;
            }break;
        }

        assert_true(false, "invalid native type");
        return 0x0000;
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
    void append_cql_native_element(BufferedString8<F>& buf, const NativeType& dtype, const T& v) {
        using TT = Decay<T>;

        if constexpr (SameAs<TT, AutoString8>) {
            assert_true(
                dtype == types::ascii   ||
                dtype == types::text    ||
                dtype == types::varchar ||
                dtype == types::blob    ||
                dtype == types::varint  ||
                dtype == types::decimal ||
                dtype == types::duration||
                dtype == types::inet    ||
                dtype == types::vector,
                "AutoString8 value does not match NativeType"
            );

            append_cql_bytes_raw(buf, reinterpret_cast<const U8*>(v.c_str), S32(v.length));
        } else if constexpr (SameAs<TT, Array<U8,16>>) {
            assert_true(dtype == types::uuid,"Array<U8,16> value does not match NativeType");

            append_cql_bytes_raw(buf, &v.values[0], 16);
        } else if constexpr (SameAs<TT, S64>) {
            assert_true(
                dtype == types::bigint ||
                dtype == types::counter ||
                dtype == types::timestamp ||
                dtype == types::time,
                "S64 value does not match NativeType"
            );

            S64 vv = v;
            U8 data[8];
            for (int i = 7; i >= 0; i--) { data[i] = U8(vv); vv >>= 8; }
            append_cql_bytes_raw(buf, data, 8);
        } else if constexpr (SameAs<TT, S32>) {
            assert_true(
                dtype == types::int_ ||
                dtype == types::float_ ||
                dtype == types::date,
                "S32 value does not match NativeType"
            );
            U8 data[4] = { U8(U32(v) >> 24), U8(U32(v) >> 16), U8(U32(v) >> 8), U8(U32(v)) };
            append_cql_bytes_raw(buf, data, 4);

        } else if constexpr (SameAs<TT, S16>) {
            assert_true(dtype == types::smallint, "S16 value does not match NativeType");
            U8 data[2] = { U8(U16(v) >> 8), U8(v) };
            append_cql_bytes_raw(buf, data, 2);

        } else if constexpr (SameAs<TT, U8>) {
            assert_true(
                dtype == types::boolean ||
                dtype == types::tinyint,
                "U8 value does not match NativeType"
            );
            append_cql_bytes_raw(buf, &v, 1);

        } else if constexpr (SameAs<TT, F32>) {
            assert_true(dtype == types::float_, "F32 value does not match NativeType");
            U32 bits; os::memory_copy(&bits, &v, sizeof(bits));
            U8 data[4] = { U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits) };
            append_cql_bytes_raw(buf, data, 4);

        } else if constexpr (SameAs<TT, F64>) {
            assert_true(dtype == types::double_, "F64 value does not match NativeType");
            U64 bits; os::memory_copy(&bits, &v, sizeof(bits));
            U8 data[8];
            for (int i = 7; i >= 0; i--) { data[i] = U8(bits); bits >>= 8; }
            append_cql_bytes_raw(buf, data, 8);

        } else {
            static_assert(!SameAs<TT, TT>, "append_cql_native_element: unsupported static type");
        }
    }

    template <typename T>
    S32 native_element_byte_size(const NativeType& dtype, const T& value) {
        using TT = Decay<T>;

        if constexpr (SameAs<TT, S64>) {
            assert_true(dtype == types::bigint || dtype == types::counter || dtype == types::timestamp || dtype == types::time,
                        "S64 does not match native type");
            return 8;
        } else if constexpr (SameAs<TT, S32>) {
            assert_true(dtype == types::int_ || dtype == types::float_ || dtype == types::date,
                        "S32 does not match native type");
            return 4;
        } else if constexpr (SameAs<TT, S16>) {
            assert_true(dtype == types::smallint,
                        "S16 does not match native type");
            return 2;
        } else if constexpr (SameAs<TT, U8>) {
            assert_true(dtype == types::boolean || dtype == types::tinyint,
                        "U8 does not match native type");
            return 1;
        } else if constexpr (SameAs<TT, F64>) {
            assert_true(dtype == types::double_,
                        "F64 does not match native type");
            return 8;
        } else if constexpr (SameAs<TT, F32>) {
            assert_true(dtype == types::float_,
                        "F32 does not match native type");
            return 4;
        } else if constexpr (SameAs<TT, AutoString8>) {
            assert_true(
                dtype == types::ascii   ||
                dtype == types::text    ||
                dtype == types::varchar ||
                dtype == types::blob    ||
                dtype == types::varint  ||
                dtype == types::decimal ||
                dtype == types::duration||
                dtype == types::inet    ||
                dtype == types::vector,
                "AutoString8 does not match native type"
            );
            return value.length;
        } else if constexpr (SameAs<TT, Array<U8,16>>) {
            assert_true(dtype == types::uuid,"Array<U8,16> value does not match NativeType");
            return 16;
        } else {
            static_assert(!SameAs<TT,TT>, "missing native value type");
        }
    }

    // Write an [option] for a column type (§4.2.5.2): [short] type_id + optional nested type(s)
    template<BufferedString8Flush F>
    void append_type_codes_option(BufferedString8<F>& buf, CqlType cdtype) {
        switch (cdtype.ctype) {
            case CollectionType::native:{
                append_be_u16(buf, native_type_to_type_code(cdtype.native.value_dtype));
            }break;
            case CollectionType::list:{
                append_be_u16(buf, type_codes::List);
                append_be_u16(buf, native_type_to_type_code(cdtype.list.element_dtype));
            }break;
            case CollectionType::set:{
                append_be_u16(buf, type_codes::Set);
                append_be_u16(buf, native_type_to_type_code(cdtype.set.key_dtype));
            }break;
            case CollectionType::map:{
                append_be_u16(buf, type_codes::Map);
                append_be_u16(buf, native_type_to_type_code(cdtype.map.key_dtype));
                append_be_u16(buf, native_type_to_type_code(cdtype.map.value_dtype));
            }break;
            case CollectionType::vector:{
                assert_not_implemented("native protocol type code for vector collection type is not implemented");
            }break;
        }
    }

    // Write a types::ReadValue as CQL binary [bytes] (big-endian network order)
    template<BufferedString8Flush F>
    void append_cql_value(BufferedString8<F>& buf, const types::ReadValue& value, CqlType cdtype) {
        visit(value, [&](const auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, types::ReadNativeTypes>) {
                assert_true(cdtype.ctype == CollectionType::native, "static value type requires ctype native, this should never happen");
                append_cql_native_element(buf, cdtype.native.value_dtype, v);
            } else if constexpr (IsCqlDM<T>) {
                assert_true(cdtype.ctype == CollectionType::map, "static value type requires ctype map, this should never happen");

                U64 pair_count = length(v);
                S32 body = 4;
                for (auto& it : v) {
                    body += 4 + native_element_byte_size(cdtype.map.key_dtype, it.first);
                    body += 4 + native_element_byte_size(cdtype.map.value_dtype, it.second);
                }

                append_be_s32(buf, body);
                append_be_s32(buf, S32(pair_count));
                for (auto& it : v) {
                    append_cql_native_element(buf, cdtype.map.key_dtype, it.first);
                    append_cql_native_element(buf, cdtype.map.value_dtype, it.second);
                }

            } else if constexpr (IsCqlDS<T>) {
                assert_true(cdtype.ctype == CollectionType::set, "static value type requires ctype set, this should never happen");

                U64 elem_count = length(v);
                S32 body = 4;
                for (auto& e : v) body += 4 + native_element_byte_size(cdtype.set.key_dtype, e);

                append_be_s32(buf, body);
                append_be_s32(buf, S32(elem_count));
                for (auto& e : v) append_cql_native_element(buf, cdtype.set.key_dtype, e);

            } else if constexpr (IsCqlDA<T>) {
                assert_true(
                    cdtype.ctype == CollectionType::list || cdtype.ctype == CollectionType::vector,
                    "static value type requires ctype list/vector, this should never happen"
                );

                U64 elem_count = (cdtype.ctype == CollectionType::vector) ? cdtype.vector.count : v.length;
                S32 body = 4;
                for (U64 i = 0; i < elem_count; ++i) {
                    auto dtype = (cdtype.ctype == CollectionType::vector) ? cdtype.vector.element_dtype : cdtype.list.element_dtype;
                    body += 4 + native_element_byte_size(dtype, v[i]);
                }

                append_be_s32(buf, body);
                append_be_s32(buf, S32(elem_count));
                for (U64 i = 0; i < elem_count; ++i) {
                    auto dtype = (cdtype.ctype == CollectionType::vector) ? cdtype.vector.element_dtype : cdtype.list.element_dtype;
                    append_cql_native_element(buf, dtype, v[i]);
                }

            } else {
                static_assert(!SameAs<T, T>, "missing implementation for read value");
            }
        });
    }

    // ========================================================================
    // Response frame builder
    // ========================================================================
    // Reserves FRAME_HEADER_SIZE bytes at the front of the chunk buffer, writes
    // the body after, and fills in the 9-byte header when the BufferedString8 flushes.
    inline auto make_native_frame(tcp::Connection* conn, tcp::Chunk* chunk, tcp::AsyncWriteFunctor* write, U8 op, S16 stream) {
        assert_true(chunk->buffer_idx >= 0, "cannot use large chunk for native response");

        U64 chunk_size = conn->chunk_chain.chunk_size;

        return BufferedString8(
            TArrayView<char>(
                reinterpret_cast<char*>(chunk->data.ptr + FRAME_HEADER_SIZE),
                chunk_size - FRAME_HEADER_SIZE
            ),
            [=](const char* data, U64 length, bool is_final) {
                assert_true_not_implemented(is_final, "native response body too large for chunk buffer");

                U8* hdr = chunk->data.ptr;
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

                const U8* body = reinterpret_cast<const U8*>(data);
                log::native_info(fmt(
                    "TX frame: op=%02x stream=%d body_len=%u hdr=%02x%02x%02x%02x%02x%02x%02x%02x%02x body[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x",
                    op, (int)(S16)stream, body_len,
                    hdr[0],hdr[1],hdr[2],hdr[3],hdr[4],hdr[5],hdr[6],hdr[7],hdr[8],
                    length>0?body[0]:0u,length>1?body[1]:0u,length>2?body[2]:0u,length>3?body[3]:0u,
                    length>4?body[4]:0u,length>5?body[5]:0u,length>6?body[6]:0u,length>7?body[7]:0u
                ));

                (*write)(conn, chunk->buffer_idx, 0, FRAME_HEADER_SIZE + U32(length));
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
        append_be_s32(buf, result_codes::VOID);
    }

    template<BufferedString8Flush F>
    void append_result_set_keyspace(BufferedString8<F>& buf, String8 keyspace) {
        append_be_s32(buf, result_codes::SET_KEYSPACE);
        append_cql_string(buf, keyspace);
    }

    template<BufferedString8Flush F>
    void append_result_schema_change(BufferedString8<F>& buf, String8 change_type, String8 target, String8 keyspace, String8 table) {
        append_be_s32(buf, result_codes::SCHEMA_CHANGE);
        append_cql_string(buf, change_type);
        append_cql_string(buf, target);
        append_cql_string(buf, keyspace);
        if (table.length > 0)
            append_cql_string(buf, table);
    }

    // @note uses btree::size so RowRange is only iterated once (RowRange iterators are not resettable)
    template<BufferedString8Flush F>
    void append_result_rows(BufferedString8<F>& buf, engine::ExecutionResult& result, schema::Table* tbl) {
        append_be_s32(buf, result_codes::ROWS);

        append_be_s32(buf, 0x0001);  // Global_tables_spec flag
        append_be_s32(buf, S32(tbl->cols.length));
        append_cql_string(buf, result.keyspace);
        append_cql_string(buf, result.table);

        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
            append_cql_string(buf, tbl->cols[ci].name);
            append_type_codes_option(buf, tbl->cols[ci].type);
        }

        // @todo size for WHERE queries
        append_be_s32(buf, S32(btree::size(tbl->btree)));

        if (result.rows.has_value()) {
            for (auto& row = result.rows->begin(); row != result.rows->end(); ++row) {
                for (auto col = engine::columns_begin(row); col != engine::columns_end(row); ++col) {
                    append_cql_value(buf, engine::read_value(col), engine::column(col).type);
                }
            }
        }
    }

    template<BufferedString8Flush F>
    void append_result_virtual_rows(BufferedString8<F>& buf, engine::VirtualRows& vr) {
        append_be_s32(buf, result_codes::ROWS);

        append_be_s32(buf, 0x0001);  // Global_tables_spec flag
        append_be_s32(buf, S32(vr.columns.length));
        append_cql_string(buf, vr.keyspace);
        append_cql_string(buf, vr.table);

        for (U64 ci = 0; ci < vr.columns.length; ci++) {
            append_cql_string(buf, vr.columns[ci].name);
            append_type_codes_option(buf, vr.columns[ci].type);
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
        tcp::Connection* conn,
        tcp::AsyncWriteFunctor* write,
        const U8* header,
        const U8* body,
        S32 body_len
    ) {
        S16 stream  = read_be_s16(header + 2);
        U8  op      = header[4];

        tcp::Chunk& chunk = *front(conn->chunk_chain.chunks);
        const U8* body_end = body + body_len;

        log::native_info(fmt("RX frame: op=%02x(%s) stream=%d body_len=%d hdr=%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            op, (const char*)op_codes_to_str(op), (int)(S16)stream, body_len,
            header[0],header[1],header[2],header[3],header[4],header[5],header[6],header[7],header[8]));

        switch (op) {
            case op_codes::STARTUP: {
                state.startup_done = true;
                auto frame = make_native_frame(conn, &chunk, write, op_codes::READY, stream);
                // empty body
            } break;

            case op_codes::OPTIONS: {
                auto frame = make_native_frame(conn, &chunk, write, op_codes::SUPPORTED, stream);
                // [string multimap]: n pairs of ([string] key, [string list] values)
                append_be_u16(frame, 2);
                append_cql_string(frame, "CQL_VERSION");
                append_be_u16(frame, 1);
                append_cql_string(frame, "3.0.0");
                append_cql_string(frame, "COMPRESSION");
                append_be_u16(frame, 0); // no compression algorithms supported
            } break;

            case op_codes::QUERY: {
                S64 t0 = os::monotonic_us();
                const U8* p = body;
                String8 query = read_cql_long_string(p, body_end);
                // Remaining bytes are query parameters (consistency, flags, etc.) - ignored

                auto cql_opt = parsers::cql::parse(query);
                if (!cql_opt) {
                    auto frame = make_native_frame(conn, &chunk, write, op_codes::ERROR, stream);
                    append_error_body(frame, engine::ExecutionStatus::SyntaxError, "Failed to parse CQL");
                    objstore::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                engine::ExecutionResult result = engine::execute(engine, *cql_opt);

                if (result.status != engine::ExecutionStatus::Success) {
                    auto frame = make_native_frame(conn, &chunk, write, op_codes::ERROR, stream);
                    String8 msg = result.message.length ? result.message : engine::to_str(result.status);
                    append_error_body(frame, result.status, msg);
                    objstore::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                switch (result.kind) {
                    case engine::ResultKind::Void:{
                        auto frame = make_native_frame(conn, &chunk, write, op_codes::RESULT, stream);
                        append_result_void(frame);
                    }break;
                    case engine::ResultKind::SchemaChange:{
                        String8 change_type = result.message.length ? result.message : "UPDATED";
                        String8 target      = result.table.length   ? "TABLE"        : "KEYSPACE";
                        auto frame = make_native_frame(conn, &chunk, write, op_codes::RESULT, stream);
                        append_result_schema_change(frame, change_type, target, result.keyspace, result.table);
                    }break;
                    case engine::ResultKind::UseKeyspace:{
                        auto frame = make_native_frame(conn, &chunk, write, op_codes::RESULT, stream);
                        append_result_set_keyspace(frame, result.keyspace);
                    }break;
                    case engine::ResultKind::Rows:{
                        auto ks = schema::read_keyspace(engine.schema, result.keyspace);
                        assert_true(ks != nullptr, "keyspace not found for rows result");
                        auto tbl = schema::read_table(engine.schema, *ks, result.table);
                        assert_true(tbl != nullptr, "table not found for rows result");
                        auto frame = make_native_frame(conn, &chunk, write, op_codes::RESULT, stream);
                        append_result_rows(frame, result, tbl);
                        objstore::log::db_response_returned_rows(btree::size(tbl->btree));
                    }break;
                    case engine::ResultKind::VirtualRows:{
                        assert_true(result.virtual_rows.has_value(), "virtual rows missing");
                        auto frame = make_native_frame(conn, &chunk, write, op_codes::RESULT, stream);
                        append_result_virtual_rows(frame, *result.virtual_rows);
                        objstore::log::db_response_returned_rows(result.virtual_rows->rows.length);
                    }break;
                }
                objstore::log::db_operation_duration(os::monotonic_us() - t0);
            } break;

            case op_codes::REGISTER: {
                // Ignore event registration, respond with READY
                auto frame = make_native_frame(conn, &chunk, write, op_codes::READY, stream);
            } break;

            case op_codes::PREPARE: {
                assert_true_not_implemented(false, "PREPARE not implemented");
            } break;

            case op_codes::EXECUTE: {
                assert_true_not_implemented(false, "EXECUTE not implemented");
            } break;

            case op_codes::BATCH: {
                assert_true_not_implemented(false, "BATCH not implemented");
            } break;

            default: {
                auto frame = make_native_frame(conn, &chunk, write, op_codes::ERROR, stream);
                append_error_body(frame, engine::ExecutionStatus::Invalid, "Unknown op_codes");
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

    Optional<String8> run(U16 port, os::Notifier& signal_pipe, volatile bool& should_exit, engine::Engine& engine, OnReady auto&& on_ready_callback, bool use_uring = true) {
        MapFixedSentinel<os::Handle, NativeState, 2_u64*tcp::MAX_CONCURRENT_CONNECTIONS> client_to_state;

        const auto on_chunk = [&engine, &client_to_state](const tcp::Request& req) -> tcp::RequestStatus {
            NativeState& state = find_or_insert(client_to_state, req.connection->client);

            // @todo async, proper chunk chaining
            // Append all new data from the chunk chain to our receive buffer.
            // Since we always return Handled, the chain contains only data from the current read.
            for (tcp::Chunk& chunk : req.connection->chunk_chain.chunks) {
                for (U64 i = 0; i < chunk.data.length; i++)
                    push_back(state.recv_buf, chunk.data.ptr[i]);
            }

            // Process all complete frames in the buffer
            while (state.recv_buf.length >= FRAME_HEADER_SIZE) {
                const U8* hdr = state.recv_buf.ptr;
                S32 body_len = read_be_s32(hdr + 5);

                if (body_len < 0 || state.recv_buf.length < FRAME_HEADER_SIZE + U64(body_len))
                    break;

                handle_frame(state, engine, req.connection, req.write, hdr, hdr + FRAME_HEADER_SIZE, body_len);

                // Remove the consumed frame from the front of recv_buf
                U64 frame_size = FRAME_HEADER_SIZE + U64(body_len);
                U64 remaining  = state.recv_buf.length - frame_size;
                if (remaining > 0)
                    os::memory_move(state.recv_buf.ptr, state.recv_buf.ptr + frame_size, remaining);
                state.recv_buf.length = remaining;
            }

            // Always release the chunk chain; unprocessed data is in recv_buf
            return tcp::RequestStatus::Handled;
        };

        const auto on_open  = [](tcp::Connection*) {};

        const auto on_close = [&client_to_state](tcp::Connection* connection) {
            if (connection != nullptr)
                try_remove(client_to_state, connection->client);
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
                signal_pipe, should_exit,
                use_uring
            );

            return {};
        }
    }
}
