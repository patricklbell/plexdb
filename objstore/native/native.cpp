module;
#include <coroutine>

module objstore.native;

namespace objstore::native {
    U16 read_be_u16(const U8* p) {
        return (U16(p[0]) << 8) | p[1];
    }

    S16 read_be_s16(const U8* p) {
        return S16(read_be_u16(p));
    }

    S32 read_be_s32(const U8* p) {
        return S32((U32(p[0]) << 24) | (U32(p[1]) << 16) | (U32(p[2]) << 8) | p[3]);
    }

    S64 read_be_s64(const U8* p) {
        return S64(
            (U64(p[0]) << 56) | (U64(p[1]) << 48) | (U64(p[2]) << 40) | (U64(p[3]) << 32) |
            (U64(p[4]) << 24) | (U64(p[5]) << 16) | (U64(p[6]) << 8)  | U64(p[7])
        );
    }

    // Read [long string]: [int] n + n bytes
    String8 read_cql_long_string(const U8*& p, const U8* end) {
        assert_true(p + 4 <= end, "truncated long string length");
        S32 len = read_be_s32(p);
        p += 4;
        assert_true(len >= 0 && p + len <= end, "long string body truncated");
        String8 s(reinterpret_cast<const char*>(p), U64(len));
        p += len;
        return s;
    }

    // Read [string]: [short] n + n bytes
    String8 read_cql_string(const U8*& p, const U8* end) {
        assert_true(p + 2 <= end, "truncated string length");
        U16 len = read_be_u16(p);
        p += 2;
        assert_true(p + len <= end, "string body truncated");
        String8 s(reinterpret_cast<const char*>(p), U64(len));
        p += len;
        return s;
    }

    // Read [short bytes]: [short] n + n bytes
    U16 read_cql_short_bytes(const U8*& p, const U8* end, const U8*& out_data) {
        assert_true(p + 2 <= end, "truncated short bytes length");
        U16 len = read_be_u16(p);
        p += 2;
        assert_true(p + len <= end, "short bytes body truncated");
        out_data = p;
        p += len;
        return len;
    }
    // ========================================================================
    // Write helpers
    // ========================================================================
    void append_byte(NativeFrame& f, U8 v) {
        if (f.body.length < f.body.capacity) {
            f.body.ptr[f.body.length++] = v;
            return;
        }
        U64 new_cap = f.body.capacity == 0 ? 64 : f.body.capacity * 2;
        reserve(f.body, new_cap);
        f.body.ptr[f.body.length++] = v;
    }

    void append_bytes(NativeFrame& f, const U8* data, U64 n) {
        U64 pos = f.body.length;
        if (pos + n > f.body.capacity) {
            U64 new_cap = f.body.capacity == 0 ? 64 : f.body.capacity * 2;
            if (new_cap < pos + n) new_cap = pos + n;
            reserve(f.body, new_cap);
        }
        os::memory_copy(f.body.ptr + pos, data, n);
        f.body.length = pos + n;
    }

    coroutine::Task<> commit(NativeFrame& f) {
        U32 body_len = U32(f.body.length);
        U8 header[FRAME_HEADER_BYTE_COUNT] = {
            RESPONSE_VERSION, 0x00,
            U8(U16(f.stream) >> 8), U8(f.stream),
            f.op,
            U8(body_len >> 24), U8(body_len >> 16), U8(body_len >> 8), U8(body_len)
        };

        log::native_info(fmt(
            "TX frame: op=%02x stream=%d body_len=%u hdr=%02x%02x%02x%02x%02x%02x%02x%02x%02x body[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x",
            f.op, (int)f.stream, body_len,
            header[0],header[1],header[2],header[3],header[4],header[5],header[6],header[7],header[8],
            body_len>0?f.body.ptr[0]:0u,body_len>1?f.body.ptr[1]:0u,body_len>2?f.body.ptr[2]:0u,body_len>3?f.body.ptr[3]:0u,
            body_len>4?f.body.ptr[4]:0u,body_len>5?f.body.ptr[5]:0u,body_len>6?f.body.ptr[6]:0u,body_len>7?f.body.ptr[7]:0u
        ));

        struct Span { const U8* ptr; U64 len; };
        Span spans[2] = {
            {header,       FRAME_HEADER_BYTE_COUNT},
            {f.body.ptr,   U64(body_len)},
        };

        for (auto& span : spans) {
            const U8* cursor    = span.ptr;
            U64       remaining = span.len;
            while (remaining > 0) {
                Optional<tcp::RWBuffer> buf_opt = tcp::acquire(*f.req);
                assert_true(static_cast<bool>(buf_opt), "buffer starvation in NativeFrame::commit");
                tcp::RWBuffer& buf = *buf_opt;
                U64 chunk = min(remaining, U64(buf.length));
                os::memory_copy(buf.view.ptr, cursor, chunk);
                buf.length = chunk;
                co_await tcp::write(*f.req, &buf);
                tcp::release(*f.req, &buf);
                cursor    += chunk;
                remaining -= chunk;
            }
        }

        f.body.length = 0;
    }

    void append_be_u16(NativeFrame& f, U16 v) {
        append_byte(f, U8(v >> 8));
        append_byte(f, U8(v));
    }

    void append_be_s32(NativeFrame& f, S32 v) {
        append_byte(f, U8(U32(v) >> 24));
        append_byte(f, U8(U32(v) >> 16));
        append_byte(f, U8(U32(v) >> 8));
        append_byte(f, U8(U32(v)));
    }

    void append_cql_string(NativeFrame& f, String8 s) {
        append_be_u16(f, U16(s.length));
        append_bytes(f, reinterpret_cast<const U8*>(s.data), s.length);
    }

    void append_cql_short_bytes(NativeFrame& f, const U8* data, U16 n) {
        append_be_u16(f, n);
        append_bytes(f, data, n);
    }

    void append_cql_bytes_raw(NativeFrame& f, const U8* data, S32 n) {
        append_be_s32(f, n);
        if (n > 0)
            append_bytes(f, data, U64(n));
    }

    // ========================================================================
    // Frame body builders
    // ========================================================================
    void append_type_codes_option(NativeFrame& f, CqlType cdtype) {
        switch (cdtype.ctype) {
            case CollectionType::native:{
                append_be_u16(f, native_type_to_type_code(cdtype.native.value_dtype));
            }break;
            case CollectionType::list:{
                append_be_u16(f, type_codes::List);
                append_be_u16(f, native_type_to_type_code(cdtype.list.element_dtype));
            }break;
            case CollectionType::set:{
                append_be_u16(f, type_codes::Set);
                append_be_u16(f, native_type_to_type_code(cdtype.set.key_dtype));
            }break;
            case CollectionType::map:{
                append_be_u16(f, type_codes::Map);
                append_be_u16(f, native_type_to_type_code(cdtype.map.key_dtype));
                append_be_u16(f, native_type_to_type_code(cdtype.map.value_dtype));
            }break;
            case CollectionType::vector:{
                assert_not_implemented("native protocol type code for vector collection type is not implemented");
            }break;
        }
    }

    void append_cql_value(NativeFrame& f, const types::ReadValue& value, CqlType cdtype) {
        visit(value, [&](const auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, types::ReadNativeTypes>) {
                assert_true(cdtype.ctype == CollectionType::native, "static value type requires ctype native, this should never happen");
                append_cql_native_element(f, cdtype.native.value_dtype, v);
            } else if constexpr (IsCqlDM<T>) {
                assert_true(cdtype.ctype == CollectionType::map, "static value type requires ctype map, this should never happen");

                U64 pair_count = length(v);
                S32 body = 4;
                for (auto& it : v) {
                    body += 4 + native_element_byte_size(cdtype.map.key_dtype, it.first);
                    body += 4 + native_element_byte_size(cdtype.map.value_dtype, it.second);
                }

                append_be_s32(f, body);
                append_be_s32(f, S32(pair_count));
                for (auto& it : v) {
                    append_cql_native_element(f, cdtype.map.key_dtype, it.first);
                    append_cql_native_element(f, cdtype.map.value_dtype, it.second);
                }

            } else if constexpr (IsCqlDS<T>) {
                assert_true(cdtype.ctype == CollectionType::set, "static value type requires ctype set, this should never happen");

                U64 elem_count = length(v);
                S32 body = 4;
                for (auto& e : v) body += 4 + native_element_byte_size(cdtype.set.key_dtype, e);

                append_be_s32(f, body);
                append_be_s32(f, S32(elem_count));
                for (auto& e : v) append_cql_native_element(f, cdtype.set.key_dtype, e);

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

                append_be_s32(f, body);
                append_be_s32(f, S32(elem_count));
                for (U64 i = 0; i < elem_count; ++i) {
                    auto dtype = (cdtype.ctype == CollectionType::vector) ? cdtype.vector.element_dtype : cdtype.list.element_dtype;
                    append_cql_native_element(f, dtype, v[i]);
                }

            } else {
                static_assert(!SameAs<T, T>, "missing implementation for read value");
            }
        });
    }

    void append_error_body(NativeFrame& f, engine::ExecutionStatus status, String8 message) {
        append_be_s32(f, S32(static_cast<U16>(status)));
        append_cql_string(f, message);
    }

    void append_result_void(NativeFrame& f) {
        append_be_s32(f, result_codes::VOID);
    }

    void append_result_set_keyspace(NativeFrame& f, String8 keyspace) {
        append_be_s32(f, result_codes::SET_KEYSPACE);
        append_cql_string(f, keyspace);
    }

    void append_result_schema_change(NativeFrame& f, String8 change_type, String8 target, String8 keyspace, String8 table) {
        append_be_s32(f, result_codes::SCHEMA_CHANGE);
        append_cql_string(f, change_type);
        append_cql_string(f, target);
        append_cql_string(f, keyspace);
        if (table.length > 0)
            append_cql_string(f, table);
    }

    // @note uses btree::size so RowRange is only iterated once (RowRange iterators are not resettable)
    void append_result_rows(NativeFrame& f, engine::ExecutionResult& result, schema::Table* tbl) {
        append_be_s32(f, result_codes::ROWS);

        append_be_s32(f, 0x0001);  // Global_tables_spec flag
        append_be_s32(f, S32(tbl->cols.length));
        append_cql_string(f, result.keyspace);
        append_cql_string(f, result.table);

        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
            append_cql_string(f, tbl->cols[ci].name);
            append_type_codes_option(f, tbl->cols[ci].type);
        }

        // @todo size for WHERE queries
        append_be_s32(f, S32(btree::size(tbl->btree)));

        if (result.rows.has_value()) {
            for (auto& row = result.rows->begin(); row != result.rows->end(); ++row) {
                for (auto col = engine::columns_begin(row); col != engine::columns_end(row); ++col) {
                    append_cql_value(f, engine::read_value(col), engine::column(col).type);
                }
            }
        }
    }

    void append_result_virtual_rows(NativeFrame& f, engine::VirtualRows& vr) {
        append_be_s32(f, result_codes::ROWS);

        append_be_s32(f, 0x0001);  // Global_tables_spec flag
        append_be_s32(f, S32(vr.columns.length));
        append_cql_string(f, vr.keyspace);
        append_cql_string(f, vr.table);

        for (U64 ci = 0; ci < vr.columns.length; ci++) {
            append_cql_string(f, vr.columns[ci].name);
            append_type_codes_option(f, vr.columns[ci].type);
        }

        append_be_s32(f, S32(vr.rows.length));

        for (U64 ri = 0; ri < vr.rows.length; ri++) {
            for (U64 ci = 0; ci < vr.columns.length; ci++)
                append_cql_value(f, vr.rows[ri].values[ci], vr.columns[ci].type);
        }
    }

    void append_result_prepared(NativeFrame& f, U64 id, engine::PreparedEntry& entry) {
        append_be_s32(f, result_codes::PREPARED);

        U8 id_bytes[8];
        for (int i = 7; i >= 0; i--) { id_bytes[i] = U8(id); id >>= 8; }
        append_cql_short_bytes(f, id_bytes, 8);

        S32 flags = 0x0001; // Global_tables_spec
        append_be_s32(f, flags);
        append_be_s32(f, S32(entry.bind_variables.length));

        S32 pk_count = (entry.pk_index >= 0) ? 1 : 0;
        append_be_s32(f, pk_count);
        if (pk_count > 0) {
            append_be_u16(f, U16(entry.pk_index));
        }

        append_cql_string(f, String8(entry.keyspace));
        append_cql_string(f, String8(entry.table));

        for (U64 i = 0; i < entry.bind_variables.length; i++) {
            append_cql_string(f, String8(entry.bind_variables[i].name));
            append_type_codes_option(f, entry.bind_variables[i].type);
        }

        S32 result_flags = 0x0004; // No_metadata
        append_be_s32(f, result_flags);
        append_be_s32(f, 0);
    }

    Constant read_cql_value_as_constant(const U8*& p, const U8* end, NativeType dtype) {
        assert_true(p + 4 <= end, "truncated value length");
        S32 len = read_be_s32(p);
        p += 4;
        if (len < 0) return Constant{.value = Null{}};
        assert_true(p + len <= end, "value body truncated");
        const U8* val = p;
        p += len;

        switch (dtype) {
            case types::text: case types::ascii: case types::varchar:
                return Constant{.value = AutoString8(val, U64(len))};
            case types::int_:{
                assert_true(len == 4, "int value must be 4 bytes");
                S64 v = S64(S32((U32(val[0]) << 24) | (U32(val[1]) << 16) | (U32(val[2]) << 8) | U32(val[3])));
                return Constant{.value = v};
            }
            case types::bigint: case types::timestamp: case types::counter: case types::time:{
                assert_true(len == 8, "bigint value must be 8 bytes");
                S64 v = read_be_s64(val);
                return Constant{.value = v};
            }
            case types::smallint:{
                assert_true(len == 2, "smallint value must be 2 bytes");
                S64 v = S64(S16((U16(val[0]) << 8) | U16(val[1])));
                return Constant{.value = v};
            }
            case types::double_:{
                assert_true(len == 8, "double value must be 8 bytes");
                U64 bits = (U64(val[0]) << 56) | (U64(val[1]) << 48) | (U64(val[2]) << 40) | (U64(val[3]) << 32) |
                           (U64(val[4]) << 24) | (U64(val[5]) << 16) | (U64(val[6]) << 8) | U64(val[7]);
                F64 d;
                os::memory_copy(&d, &bits, sizeof(d));
                return Constant{.value = d};
            }
            case types::float_:{
                assert_true(len == 4, "float value must be 4 bytes");
                U32 bits = (U32(val[0]) << 24) | (U32(val[1]) << 16) | (U32(val[2]) << 8) | U32(val[3]);
                F32 fv;
                os::memory_copy(&fv, &bits, sizeof(fv));
                return Constant{.value = F64(fv)};
            }
            case types::boolean:{
                assert_true(len == 1, "boolean value must be 1 byte");
                return Constant{.value = bool(val[0])};
            }
            default:
                assert_not_implemented("unsupported native type for bind value");
                return Constant{.value = Null{}};
        }
    }

    // @todo user input error handling
    DynamicArray<Constant> read_query_parameter_values(const U8*& p, const U8* end, const DynamicArray<engine::BindVariableSpec>& bind_specs) {
        DynamicArray<Constant> bound_values;
        if (p + 2 > end) return bound_values;
        p += 2; // skip consistency [short] @todo
        if (p >= end) return bound_values;

        U8 flags = *p++;
        if (!(flags & 0x01)) return bound_values;

        assert_true_not_implemented(p + 2 <= end, "error handling for truncated values count not implemented");
        U16 n_values = read_be_u16(p);
        p += 2;

        if (flags & 0x40) {
            for (U64 bi = 0; bi < bind_specs.length; bi++) {
                push_back(bound_values, Constant{.value = Null{}});
            }
            for (U16 value_idx = 0; value_idx < n_values && p < end; value_idx++) {
                String8 name = read_cql_string(p, end);

                NativeType dtype{};
                U64 bind_spec_idx = bind_specs.length;
                for (U64 idx = 0; idx < bind_specs.length; idx++) {
                    if (bind_specs[idx].name == name) {
                        dtype = bind_specs[idx].type.native.value_dtype;
                        bind_spec_idx = idx;
                        break;
                    }
                }

                assert_true_not_implemented(bind_spec_idx < bind_specs.length, "error handling for invalid bind specification not implemented");
                bound_values[bind_spec_idx] = read_cql_value_as_constant(p, end, dtype);
            }
        } else {
            for (U16 bind_spec_idx = 0; bind_spec_idx < n_values && p < end; bind_spec_idx++) {
                assert_true_not_implemented(bind_spec_idx < bind_specs.length, "error handling for invalid bind specification not implemented");

                NativeType dtype = bind_specs[bind_spec_idx].type.native.value_dtype;
                push_back(bound_values, read_cql_value_as_constant(p, end, dtype));
            }
        }

        return bound_values;
    }

    // ========================================================================
    // Shared result writing
    // ========================================================================
    coroutine::Task<bool> send_error_if_failed(
        engine::ExecutionResult& result,
        const tcp::Request* req,
        S16 stream
    ) {
        if (result.status != engine::ExecutionStatus::Success) {
            auto frame = make_native_frame(req, op_codes::ERROR, stream);
            String8 msg = result.message.length ? result.message : engine::to_str(result.status);
            append_error_body(frame, result.status, msg);
            co_await commit(frame);
            co_return true;
        }
        co_return false;
    }

    coroutine::Task<> send_execution_result(
        engine::ExecutionResult& result,
        engine::Engine& engine,
        const tcp::Request* req,
        S16 stream
    ) {
        switch (result.kind) {
            case engine::ResultKind::Void:{
                auto frame = make_native_frame(req, op_codes::RESULT, stream);
                append_result_void(frame);
                co_await commit(frame);
            }break;
            case engine::ResultKind::SchemaChange:{
                String8 change_type = result.message.length ? result.message : "UPDATED";
                String8 target      = result.table.length   ? "TABLE"        : "KEYSPACE";
                auto frame = make_native_frame(req, op_codes::RESULT, stream);
                append_result_schema_change(frame, change_type, target, result.keyspace, result.table);
                co_await commit(frame);
            }break;
            case engine::ResultKind::UseKeyspace:{
                auto frame = make_native_frame(req, op_codes::RESULT, stream);
                append_result_set_keyspace(frame, result.keyspace);
                co_await commit(frame);
            }break;
            case engine::ResultKind::Rows:{
                auto ks = schema::read_keyspace(engine.schema, result.keyspace);
                assert_true(ks != nullptr, "keyspace not found for rows result");
                auto tbl = schema::read_table(engine.schema, *ks, result.table);
                assert_true(tbl != nullptr, "table not found for rows result");
                auto frame = make_native_frame(req, op_codes::RESULT, stream);
                append_result_rows(frame, result, tbl);
                co_await commit(frame);
                objstore::log::db_response_returned_rows(btree::size(tbl->btree));
            }break;
            case engine::ResultKind::VirtualRows:{
                assert_true(result.virtual_rows.has_value(), "virtual rows missing");
                auto frame = make_native_frame(req, op_codes::RESULT, stream);
                append_result_virtual_rows(frame, *result.virtual_rows);
                co_await commit(frame);
                objstore::log::db_response_returned_rows(result.virtual_rows->rows.length);
            }break;
        }
    }

    // ========================================================================
    // Frame dispatcher
    //   @todo add asserts for unhandled features
    // ========================================================================
    coroutine::Task<void> frame_handler(
        engine::Engine& engine,
        const tcp::Request& req,
        const U8* header,
        const U8* body,
        S32 body_length
    ) {
        S16 stream  = read_be_s16(header + 2);
        U8  op      = header[4];

        const U8* body_end = body + body_length;

        log::native_info(fmt(
            "RX frame: op=%02x(%s) stream=%d body_len=%d hdr=%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            op, (const char*)op_codes_to_str(op), (int)(S16)stream, body_length,
            header[0],header[1],header[2],header[3],header[4],header[5],header[6],header[7],header[8]
        ));

        switch (op) {
            case op_codes::STARTUP:{
                U8 version = header[0] & 0x7F;
                if (version != 0x04 && version != 0x03) {
                    auto frame = make_native_frame(&req, op_codes::ERROR, stream);
                    append_be_s32(frame, 0x000A);
                    append_cql_string(frame, "Invalid or unsupported protocol version (4)");
                    co_await commit(frame);
                    break;
                }
                auto frame = make_native_frame(&req, op_codes::READY, stream);
                co_await commit(frame);
            }break;

            case op_codes::OPTIONS:{
                auto frame = make_native_frame(&req, op_codes::SUPPORTED, stream);
                append_be_u16(frame, 2);
                append_cql_string(frame, "CQL_VERSION");
                append_be_u16(frame, 1);
                append_cql_string(frame, "3.0.0");
                append_cql_string(frame, "COMPRESSION");
                append_be_u16(frame, 0);
                co_await commit(frame);
            }break;

            case op_codes::QUERY:{
                S64 t0 = os::monotonic_us();
                const U8* p = body;
                String8 query = read_cql_long_string(p, body_end);

                auto cql_opt = parsers::cql::parse(query);
                if (!cql_opt) {
                    auto frame = make_native_frame(&req, op_codes::ERROR, stream);
                    append_error_body(frame, engine::ExecutionStatus::SyntaxError, "Failed to parse CQL");
                    co_await commit(frame);
                    objstore::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                auto bind_specs = engine::collect_bind_variables(engine, *cql_opt);
                auto bound_values = read_query_parameter_values(p, body_end, bind_specs);
                engine::ExecutionResult result = engine::execute(engine, *cql_opt, move(bound_values));

                if (co_await send_error_if_failed(result, &req, stream)) {
                    objstore::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                co_await send_execution_result(result, engine, &req, stream);
                objstore::log::db_operation_duration(os::monotonic_us() - t0);
            }break;

            case op_codes::REGISTER:{
                auto frame = make_native_frame(&req, op_codes::READY, stream);
                co_await commit(frame);
            }break;

            case op_codes::PREPARE:{
                S64 t0 = os::monotonic_us();
                const U8* p = body;
                String8 query = read_cql_long_string(p, body_end);

                auto result = engine::prepare(engine, query);
                if (result.status != engine::ExecutionStatus::Success) {
                    auto frame = make_native_frame(&req, op_codes::ERROR, stream);
                    String8 msg = result.message.length ? result.message : engine::to_str(result.status);
                    append_error_body(frame, result.status, msg);
                    co_await commit(frame);
                    objstore::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                auto frame = make_native_frame(&req, op_codes::RESULT, stream);
                append_result_prepared(frame, result.id, *result.entry);
                co_await commit(frame);
                objstore::log::db_operation_duration(os::monotonic_us() - t0);
            }break;

            case op_codes::EXECUTE:{
                S64 t0 = os::monotonic_us();
                const U8* p = body;

                const U8* id_data = nullptr;
                U16 id_len = read_cql_short_bytes(p, body_end, id_data);
                U64 prepared_id = 0;
                for (U16 i = 0; i < id_len && i < 8; i++)
                    prepared_id = (prepared_id << 8) | U64(id_data[i]);

                auto* entry = engine::try_get_prepared(engine, prepared_id);
                if (entry == nullptr) {
                    auto frame = make_native_frame(&req, op_codes::ERROR, stream);
                    append_error_body(frame, engine::ExecutionStatus::Invalid, "Prepared statement not found (unprepared)");
                    co_await commit(frame);
                    objstore::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                auto bound_values = read_query_parameter_values(p, body_end, entry->bind_variables);
                engine::ExecutionResult result = engine::execute(engine, prepared_id, move(bound_values));

                if (co_await send_error_if_failed(result, &req, stream)) {
                    objstore::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                co_await send_execution_result(result, engine, &req, stream);
                objstore::log::db_operation_duration(os::monotonic_us() - t0);
            }break;

            case op_codes::BATCH:{
                assert_true_not_implemented(false, "BATCH not implemented");
            }break;

            default:{
                auto frame = make_native_frame(&req, op_codes::ERROR, stream);
                append_error_body(frame, engine::ExecutionStatus::Invalid, "Unknown op code");
                co_await commit(frame);
            }break;
        }

        co_return;
    }}