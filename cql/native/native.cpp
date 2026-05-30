module;
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

module cql.native;

import plexdb.pager;
import plexdb.pager.transaction;

namespace cql::native {
    // ========================================================================
    // input
    // ========================================================================
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

    String8 read_cql_long_string(const U8*& p, const U8* end) {
        assert_true(p + 4 <= end, "truncated long string length");
        S32 len = read_be_s32(p);
        p += 4;
        assert_true(len >= 0 && p + len <= end, "long string body truncated");
        String8 s(reinterpret_cast<const char*>(p), U64(len));
        p += len;
        return s;
    }

    String8 read_cql_string(const U8*& p, const U8* end) {
        assert_true(p + 2 <= end, "truncated string length");
        U16 len = read_be_u16(p);
        p += 2;
        assert_true(p + len <= end, "string body truncated");
        String8 s(reinterpret_cast<const char*>(p), U64(len));
        p += len;
        return s;
    }

    U16 read_cql_short_bytes(const U8*& p, const U8* end, const U8*& out_data) {
        assert_true(p + 2 <= end, "truncated short bytes length");
        U16 len = read_be_u16(p);
        p += 2;
        assert_true(p + len <= end, "short bytes body truncated");
        out_data = p;
        p += len;
        return len;
    }

    static VarInt decode_varint_cql(const U8* val, S32 len) {
        VarInt result;
        if (len == 0) return result;
        bool negative = (val[0] & 0x80u) != 0;
        result.negative = negative;
        if (!negative) {
            S32 start = 0;
            while (start < len - 1 && val[start] == 0) start++;
            resize(result.magnitude, U64(len - start));
            os::memory_copy(result.magnitude.ptr, val + start, U64(len - start));
        } else {
            resize(result.magnitude, U64(len));
            U16 carry = 1;
            for (S32 i = len - 1; i >= 0; i--) {
                U16 sum = U16(~val[i]) + carry;
                result.magnitude.ptr[i] = U8(sum);
                carry = sum >> 8;
            }
            S32 start = 0;
            while (start < S32(result.magnitude.length) - 1 && result.magnitude.ptr[start] == 0) start++;
            if (start > 0) {
                U64 new_len = result.magnitude.length - U64(start);
                os::memory_move(result.magnitude.ptr, result.magnitude.ptr + start, new_len);
                resize(result.magnitude, new_len);
            }
        }
        return result;
    }

    static S64 read_vint_cql(const U8*& p) {
        U8 b = *p++;
        if ((b & 0x80u) == 0) {
            return S64((U64(b) >> 1) ^ -(U64(b) & 1));
        }
        int n_extra = 0;
        for (int bit = 7; bit >= 0 && ((b >> bit) & 1); bit--) n_extra++;
        U64 raw;
        if (n_extra >= 8) {
            U64 b0=*p++, b1=*p++, b2=*p++, b3=*p++, b4=*p++, b5=*p++, b6=*p++, b7=*p++;
            raw = (b0<<56)|(b1<<48)|(b2<<40)|(b3<<32)|(b4<<24)|(b5<<16)|(b6<<8)|b7;
        } else {
            raw = U64(b & (0xffu >> (n_extra + 1)));
            for (int i = 0; i < n_extra; i++) raw = (raw << 8) | U64(*p++);
        }
        return S64((raw >> 1) ^ -(raw & 1));
    }

    ColumnValue read_cql_value_as_column_value(const U8*& p, const U8* end, BasicType dtype) {
        assert_true(p + 4 <= end, "truncated value length");
        S32 len = read_be_s32(p);
        p += 4;
        if (len < 0) return ColumnValue{Null{}};
        assert_true(p + len <= end, "value body truncated");
        const U8* val = p;
        p += len;

        switch (dtype) {
            case BasicType::text: case BasicType::ascii: case BasicType::varchar:
                return ColumnValue{AutoString8(val, U64(len))};
            case BasicType::int_:{
                assert_true(len == 4, "int value must be 4 bytes");
                S32 v = S32((U32(val[0]) << 24) | (U32(val[1]) << 16) | (U32(val[2]) << 8) | U32(val[3]));
                return ColumnValue{v};
            }
            case BasicType::bigint: case BasicType::timestamp: case BasicType::counter: case BasicType::time:{
                assert_true(len == 8, "bigint value must be 8 bytes");
                return ColumnValue{read_be_s64(val)};
            }
            case BasicType::smallint:{
                assert_true(len == 2, "smallint value must be 2 bytes");
                return ColumnValue{S16((U16(val[0]) << 8) | U16(val[1]))};
            }
            case BasicType::double_:{
                assert_true(len == 8, "double value must be 8 bytes");
                U64 bits = (U64(val[0]) << 56) | (U64(val[1]) << 48) | (U64(val[2]) << 40) | (U64(val[3]) << 32) |
                           (U64(val[4]) << 24) | (U64(val[5]) << 16) | (U64(val[6]) << 8) | U64(val[7]);
                F64 d;
                os::memory_copy(&d, &bits, sizeof(d));
                return ColumnValue{d};
            }
            case BasicType::float_:{
                assert_true(len == 4, "float value must be 4 bytes");
                U32 bits = (U32(val[0]) << 24) | (U32(val[1]) << 16) | (U32(val[2]) << 8) | U32(val[3]);
                F32 fv;
                os::memory_copy(&fv, &bits, sizeof(fv));
                return ColumnValue{fv};
            }
            case BasicType::boolean:{
                assert_true(len == 1, "boolean value must be 1 byte");
                return ColumnValue{U8(val[0])};
            }
            case BasicType::tinyint:{
                assert_true(len == 1, "tinyint value must be 1 byte");
                return ColumnValue{U8(val[0])};
            }
            case BasicType::uuid: case BasicType::timeuuid:{
                assert_true(len == 16, "uuid/timeuuid value must be 16 bytes");
                UUID uuid{};
                os::memory_copy(&uuid.value[0], val, 16);
                return ColumnValue{uuid};
            }
            case BasicType::date:{
                assert_true(len == 4, "date value must be 4 bytes");
                S32 v = read_be_s32(val);
                return ColumnValue{v};
            }
            case BasicType::blob: case BasicType::hex:{
                Blob b;
                resize(b.value, U64(len));
                os::memory_copy(b.value.ptr, val, U64(len));
                return ColumnValue{move(b)};
            }
            case BasicType::inet:{
                assert_true(len == 4 || len == 16, "inet value must be 4 or 16 bytes");
                Inet inet;
                inet.is_v6 = (len == 16);
                if (inet.is_v6) os::memory_copy(&inet.v6[0], val, 16);
                else            os::memory_copy(&inet.v4[0], val, 4);
                return ColumnValue{move(inet)};
            }
            case BasicType::varint:{
                return ColumnValue{decode_varint_cql(val, len)};
            }
            case BasicType::decimal:{
                assert_true(len >= 4, "decimal value must be at least 4 bytes");
                Decimal d;
                d.scale = read_be_s32(val);
                d.unscaled = decode_varint_cql(val + 4, len - 4);
                return ColumnValue{move(d)};
            }
            case BasicType::duration:{
                assert_true(len > 0, "duration value must not be empty");
                const U8* dp = val;
                Duration dur;
                dur.months      = S32(read_vint_cql(dp));
                dur.days        = S32(read_vint_cql(dp));
                dur.nanoseconds = read_vint_cql(dp);
                return ColumnValue{dur};
            }
            default:
                assert_not_implemented("CQL value decoding for this basic type is not implemented");
                return ColumnValue{Null{}};
        }
    }

    Term read_cql_value_as_term(const U8*& p, const U8* end, const Type& dtype) {
        return visit(dtype.variants, [&](const auto& v) -> Term {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, Type::Basic>) {
                Term t;
                t.value = read_cql_value_as_column_value(p, end, v.value_dtype);
                return t;
            } else if constexpr (SameAs<T, Type::List>) {
                assert_true(p + 4 <= end, "truncated list bind value length");
                S32 outer_len = read_be_s32(p);
                p += 4;
                if (outer_len < 0) return Term{.value = Constant{.value = Null{}}};
                const U8* coll_end = p + U64(outer_len);
                assert_true(coll_end <= end, "list bind value body truncated");
                assert_true(p + 4 <= coll_end, "truncated list element count");
                S32 count = read_be_s32(p);
                p += 4;
                Type elem_type = io::element_type_to_type(v.element);
                ListOrVectorLiteral lit{};
                for (S32 i = 0; i < count && p < coll_end; i++)
                    push_back(lit.elements, read_cql_value_as_term(p, coll_end, elem_type));
                p = coll_end;
                return Term{.value = move(lit)};
            } else if constexpr (SameAs<T, Type::Set>) {
                assert_true(p + 4 <= end, "truncated set bind value length");
                S32 outer_len = read_be_s32(p);
                p += 4;
                if (outer_len < 0) return Term{.value = Constant{.value = Null{}}};
                const U8* coll_end = p + U64(outer_len);
                assert_true(coll_end <= end, "set bind value body truncated");
                assert_true(p + 4 <= coll_end, "truncated set element count");
                S32 count = read_be_s32(p);
                p += 4;
                Type key_type = io::element_type_to_type(v.key);
                SetLiteral lit{};
                for (S32 i = 0; i < count && p < coll_end; i++)
                    push_back(lit.keys, read_cql_value_as_term(p, coll_end, key_type));
                p = coll_end;
                return Term{.value = move(lit)};
            } else if constexpr (SameAs<T, Type::Map>) {
                assert_true(p + 4 <= end, "truncated map bind value length");
                S32 outer_len = read_be_s32(p);
                p += 4;
                if (outer_len < 0) return Term{.value = Constant{.value = Null{}}};
                const U8* coll_end = p + U64(outer_len);
                assert_true(coll_end <= end, "map bind value body truncated");
                assert_true(p + 4 <= coll_end, "truncated map element count");
                S32 count = read_be_s32(p);
                p += 4;
                Type key_type = io::element_type_to_type(v.key);
                Type val_type = io::element_type_to_type(v.value);
                MapLiteral lit{};
                for (S32 i = 0; i < count && p < coll_end; i++) {
                    Term key = read_cql_value_as_term(p, coll_end, key_type);
                    Term val = read_cql_value_as_term(p, coll_end, val_type);
                    push_back(lit.key_values, Pair<Term, Term>{move(key), move(val)});
                }
                p = coll_end;
                return Term{.value = move(lit)};
            } else {
                assert_not_implemented("vector collection type bind parameters are not implemented");
                return Term{.value = Constant{.value = Null{}}};
            }
        });
    }

    coroutine::Task<bool> try_append_tcp_read(const tcp::Request& req, DynamicArray<U8>& buf) {
        auto rbuf = co_await tcp::acquire(req);
        if (auto err = co_await tcp::read(req, &rbuf); err != tcp::Error::None) {
            tcp::release(req, &rbuf);
            co_return false;
        }
        U64 old = buf.length;
        resize(buf, old + rbuf.view.length);
        os::memory_copy(buf.ptr + old, rbuf.view.ptr, rbuf.view.length);
        tcp::release(req, &rbuf);
        co_return true;
    }

    // Read query parameter values. In v5, flags is [int] (32-bit); in v4 it is [byte].
    // v5 also adds optional keyspace and now_in_seconds fields.
    template<U8 Version>
    DynamicArray<Term> read_query_parameter_values(const U8*& p, const U8* end, const DynamicArray<engine::BindVariableSpec>& bind_specs) {
        DynamicArray<Term> bound_values;
        if (p + 2 > end) return bound_values;
        p += 2; // consistency [short] @todo

        U32 flags;
        if constexpr (Version >= 5) {
            assert_true_not_implemented(p + 4 <= end, "error handling for truncated v5 flags not implemented");
            flags = U32(read_be_s32(p));
            p += 4;
        } else {
            if (p >= end) return bound_values;
            flags = U32(*p++);
        }

        if (flags & 0x01u) {
            assert_true_not_implemented(p + 2 <= end, "error handling for truncated values count not implemented");
            U16 n_values = read_be_u16(p);
            p += 2;

            if (flags & 0x40u) {
                for (U64 bi = 0; bi < bind_specs.length; bi++)
                    push_back(bound_values, Term{.value = Constant{.value = Null{}}});
                for (U16 value_idx = 0; value_idx < n_values && p < end; value_idx++) {
                    String8 name = read_cql_string(p, end);
                    U64 bind_spec_idx = bind_specs.length;
                    for (U64 idx = 0; idx < bind_specs.length; idx++) {
                        if (bind_specs[idx].name == name) {
                            bind_spec_idx = idx;
                            break;
                        }
                    }
                    assert_true_not_implemented(bind_spec_idx < bind_specs.length, "named bind parameter not found in prepared statement spec - error handling is not implemented");
                    bound_values[bind_spec_idx] = read_cql_value_as_term(p, end, bind_specs[bind_spec_idx].type);
                }
            } else {
                for (U16 bind_spec_idx = 0; bind_spec_idx < n_values && p < end; bind_spec_idx++) {
                    assert_true_not_implemented(bind_spec_idx < bind_specs.length, "more positional bind values than expected - error handling is not implemented");
                    push_back(bound_values, read_cql_value_as_term(p, end, bind_specs[bind_spec_idx].type));
                }
            }
        }

        // Skip optional fields that precede the v5-only fields
        if ((flags & 0x04u) && p + 4 <= end) p += 4;               // result_page_size [int]
        if (flags & 0x08u) {                                          // paging_state [bytes]
            if (p + 4 <= end) {
                S32 ps_len = read_be_s32(p); p += 4;
                if (ps_len > 0 && p + ps_len <= end) p += ps_len;
            }
        }
        if ((flags & 0x10u) && p + 2 <= end) p += 2;               // serial_consistency [short]
        if ((flags & 0x20u) && p + 8 <= end) p += 8;               // timestamp [long]

        if constexpr (Version >= 5) {
            if ((flags & 0x0080u) && p < end) {
                String8 keyspace = read_cql_string(p, end);
                (void)keyspace; // @todo pass to engine when per-query keyspace context is supported
            }
            if (flags & 0x0100u) {
                assert_not_implemented("now_in_seconds is not supported (engine has no TTL support)");
            }
        }

        return bound_values;
    }

    // ========================================================================
    // output
    // ========================================================================
    coroutine::Task<> send_block(const tcp::Request& req, const U8* data, U64 len) {
        while (len > 0) {
            auto buf = co_await tcp::acquire(req);
            U64 chunk = min(len, static_cast<U64>(buf.length));
            os::memory_copy(buf.view.ptr, data, chunk);
            buf.view.length = chunk;
            co_await tcp::write(req, &buf);
            tcp::release(req, &buf);

            data += chunk;
            len  -= chunk;
        }
    }

    template<U8 Version, bool Compressed>
    coroutine::Task<> send_native_frame(Frame& f) { ZoneScopedN("send_native_frame")
        assert_true(f.body.length <= MAX_U32, "body length too large");
        U32 body_len = U32(f.body.length);

        constexpr U8 resp_flags = (Version >= 5) ? 0x10u : 0x00u;
        U8 envelope_header[V4_FRAME_HEADER_BYTE_COUNT] = {
            U8(0x80u | Version), resp_flags,
            U8(U16(f.stream) >> 8), U8(f.stream),
            f.op,
            U8(body_len >> 24), U8(body_len >> 16), U8(body_len >> 8), U8(body_len)
        };

        log::native_info(fmt(
            "TX frame: op=%02x stream=%d body_len=%u hdr=%02x%02x%02x%02x%02x%02x%02x%02x%02x body[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x",
            f.op, (int)f.stream, body_len,
            envelope_header[0],envelope_header[1],envelope_header[2],envelope_header[3],
            envelope_header[4],envelope_header[5],envelope_header[6],envelope_header[7],
            envelope_header[8],
            body_len>0?f.body.ptr[0]:0u, body_len>1?f.body.ptr[1]:0u,
            body_len>2?f.body.ptr[2]:0u, body_len>3?f.body.ptr[3]:0u,
            body_len>4?f.body.ptr[4]:0u, body_len>5?f.body.ptr[5]:0u,
            body_len>6?f.body.ptr[6]:0u, body_len>7?f.body.ptr[7]:0u
        ));

        if constexpr (Version < 5) {
            co_await send_block(*f.req, envelope_header, V4_FRAME_HEADER_BYTE_COUNT);
            co_await send_block(*f.req, f.body.ptr, body_len);
        } else {
            // Build full inner envelope, then wrap in v5 outer frame(s)
            U64 env_len = V4_FRAME_HEADER_BYTE_COUNT + body_len;
            assert_true(env_len <= MAX_U32, "v5 envelope too large");
            DynamicArray<U8> env{};
            resize(env, env_len);
            os::memory_copy(env.ptr, envelope_header, V4_FRAME_HEADER_BYTE_COUNT);
            if (body_len > 0)
                os::memory_copy(env.ptr + V4_FRAME_HEADER_BYTE_COUNT, f.body.ptr, body_len);
            co_await send_v5_outer_frames<Compressed>(*f.req, env.ptr, env_len);
        }

        f.body.length = 0;
    }

    void append_byte(Frame& f, U8 v) {
        if (f.body.length < f.body.capacity) {
            f.body.ptr[f.body.length++] = v;
            return;
        }
        U64 new_cap = f.body.capacity == 0 ? 64 : f.body.capacity * 2;
        reserve(f.body, new_cap);
        f.body.ptr[f.body.length++] = v;
    }

    void append_bytes(Frame& f, const U8* data, U64 n) {
        U64 pos = f.body.length;
        if (pos + n > f.body.capacity) {
            U64 new_cap = f.body.capacity == 0 ? 64 : f.body.capacity * 2;
            if (new_cap < pos + n) new_cap = pos + n;
            reserve(f.body, new_cap);
        }
        os::memory_copy(f.body.ptr + pos, data, n);
        f.body.length = pos + n;
    }

    void append_be_u16(Frame& f, U16 v) {
        append_byte(f, U8(v >> 8));
        append_byte(f, U8(v));
    }

    void append_be_s32(Frame& f, S32 v) {
        append_byte(f, U8(U32(v) >> 24));
        append_byte(f, U8(U32(v) >> 16));
        append_byte(f, U8(U32(v) >> 8));
        append_byte(f, U8(U32(v)));
    }

    void append_cql_string(Frame& f, String8 s) {
        append_be_u16(f, U16(s.length));
        append_bytes(f, reinterpret_cast<const U8*>(s.data), s.length);
    }

    void append_cql_short_bytes(Frame& f, const U8* data, U16 n) {
        append_be_u16(f, n);
        append_bytes(f, data, n);
    }

    void append_cql_bytes_raw(Frame& f, const U8* data, S32 n) {
        append_be_s32(f, n);
        if (n > 0)
            append_bytes(f, data, U64(n));
    }

    void append_type_codes_option(Frame& f, Type cdtype) {
        visit(cdtype.variants, [&f](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, Type::Basic>) {
                append_be_u16(f, basic_type_to_type_code(v.value_dtype));
            } else if constexpr (SameAs<T, Type::List>) {
                append_be_u16(f, type_codes::List);
                append_be_u16(f, basic_type_to_type_code(get<BasicType>(v.element)));
            } else if constexpr (SameAs<T, Type::Set>) {
                append_be_u16(f, type_codes::Set);
                append_be_u16(f, basic_type_to_type_code(get<BasicType>(v.key)));
            } else if constexpr (SameAs<T, Type::Map>) {
                append_be_u16(f, type_codes::Map);
                append_be_u16(f, basic_type_to_type_code(get<BasicType>(v.key)));
                append_be_u16(f, basic_type_to_type_code(get<BasicType>(v.value)));
            } else if constexpr (SameAs<T, Type::Vector>) {
                assert_not_implemented("native protocol type code for vector collection type is not implemented");
            }
        });
    }

    void append_cql_value(Frame& f, const ColumnValue& value, Type cdtype) {
        visit(value, [&](const auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (SameAs<T, Null>) {
                append_be_s32(f, -1);
            } else if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                assert_true(type_matches_tag<Type::Basic>(cdtype.variants), "static value type requires ctype basic, this should never happen");
                append_cql_basic_element(f, get<Type::Basic>(cdtype.variants).value_dtype, v);
            } else if constexpr (IsCqlDM<T>) {
                assert_true(type_matches_tag<Type::Map>(cdtype.variants), "static value type requires ctype map, this should never happen");
                const auto& m = get<Type::Map>(cdtype.variants);
                U64 pair_count = length(v);
                S32 body = 4;
                for (auto& it : v) {
                    body += 4 + basic_element_byte_size(get<BasicType>(m.key), it.first);
                    body += 4 + basic_element_byte_size(get<BasicType>(m.value), it.second);
                }
                append_be_s32(f, body);
                append_be_s32(f, S32(pair_count));
                for (auto& it : v) {
                    append_cql_basic_element(f, get<BasicType>(m.key), it.first);
                    append_cql_basic_element(f, get<BasicType>(m.value), it.second);
                }
            } else if constexpr (IsCqlDS<T>) {
                assert_true(type_matches_tag<Type::Set>(cdtype.variants), "static value type requires ctype set, this should never happen");
                const auto key_dtype = get<BasicType>(get<Type::Set>(cdtype.variants).key);
                U64 elem_count = length(v);
                S32 body = 4;
                for (auto& e : v) body += 4 + basic_element_byte_size(key_dtype, e);
                append_be_s32(f, body);
                append_be_s32(f, S32(elem_count));
                for (auto& e : v) append_cql_basic_element(f, key_dtype, e);
            } else if constexpr (IsCqlDA<T>) {
                bool is_vec = type_matches_tag<Type::Vector>(cdtype.variants);
                assert_true(
                    type_matches_tag<Type::List>(cdtype.variants) || is_vec,
                    "static value type requires ctype list/vector, this should never happen"
                );
                U64 elem_count = is_vec ? get<Type::Vector>(cdtype.variants).count : v.length;
                const auto elem_dtype = is_vec ? get<BasicType>(get<Type::Vector>(cdtype.variants).element) : get<BasicType>(get<Type::List>(cdtype.variants).element);
                S32 body = 4;
                for (U64 i = 0; i < elem_count; ++i)
                    body += 4 + basic_element_byte_size(elem_dtype, v[i]);
                append_be_s32(f, body);
                append_be_s32(f, S32(elem_count));
                for (U64 i = 0; i < elem_count; ++i)
                    append_cql_basic_element(f, elem_dtype, v[i]);
            } else {
                static_assert(!SameAs<T, T>, "missing implementation for read value");
            }
        });
    }

    void append_error_body(Frame& f, engine::ExecutionStatus status, String8 message) {
        append_be_s32(f, S32(static_cast<U16>(status)));
        append_cql_string(f, message);
    }

    void append_result_void(Frame& f) {
        append_be_s32(f, result_codes::VOID);
    }

    void append_result_set_keyspace(Frame& f, String8 keyspace) {
        append_be_s32(f, result_codes::SET_KEYSPACE);
        append_cql_string(f, keyspace);
    }

    void append_result_schema_change(Frame& f, String8 change_type, String8 target, String8 keyspace, String8 table) {
        append_be_s32(f, result_codes::SCHEMA_CHANGE);
        append_cql_string(f, change_type);
        append_cql_string(f, target);
        append_cql_string(f, keyspace);
        if (table.length > 0)
            append_cql_string(f, table);
    }

    coroutine::Task<> append_result_rows(Frame& f, engine::ExecutionResult& result, schema::Table* tbl) {
        append_be_s32(f, result_codes::ROWS);

        bool has_select = result.select_col_indices.length > 0;
        U64 col_count = has_select ? result.select_col_indices.length : tbl->cols.length;

        auto is_selected = [&](U64 ci) -> bool {
            if (!has_select) return true;
            for (U64 i = 0; i < result.select_col_indices.length; i++)
                if (result.select_col_indices[i] == ci) return true;
            return false;
        };

        append_be_s32(f, 0x0001);  // Global_tables_spec flag
        append_be_s32(f, S32(col_count));
        append_cql_string(f, result.keyspace);
        append_cql_string(f, result.table);

        for (U64 i = 0; i < col_count; i++) {
            U64 ci = has_select ? result.select_col_indices[i] : i;
            append_cql_string(f, tbl->cols[ci].name);
            append_type_codes_option(f, tbl->cols[ci].type);
        }

        U64 count_pos = f.body.length;
        append_be_s32(f, 0);

        S32 row_count = 0;
        if (result.rows.has_value()) {
            auto& rows = *result.rows;
            U64 row_limit = result.row_limit_count;
            RowIterator& row_it  = rows.start;
            RowIterator& row_end = rows.stop;
            while (row_it != row_end && U64(row_count) < row_limit) {
                ColumnRange col_range = co_await row_it.deref();
                U64 ci = 0;
                while (col_range.start != col_range.stop && ci < tbl->cols.length) {
                    if (is_selected(ci)) {
                        ColumnValue v = co_await col_range.start.deref();
                        append_cql_value(f, v, tbl->cols[ci].type);
                    }
                    co_await col_range.start.advance();
                    ++ci;
                }
                row_count++;
                co_await row_it.advance();
            }
        }

        f.body.ptr[count_pos + 0] = U8(U32(row_count) >> 24);
        f.body.ptr[count_pos + 1] = U8(U32(row_count) >> 16);
        f.body.ptr[count_pos + 2] = U8(U32(row_count) >> 8);
        f.body.ptr[count_pos + 3] = U8(U32(row_count));
    }

    void append_result_virtual_rows(Frame& f, engine::VirtualRows& vr) {
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

    template<U8 Version>
    void append_result_prepared(Frame& f, U64 id, engine::PreparedEntry& entry) {
        append_be_s32(f, result_codes::PREPARED);

        U8 id_bytes[8];
        U64 id_val = id;
        for (int i = 7; i >= 0; i--) { id_bytes[i] = U8(id_val); id_val >>= 8; }
        append_cql_short_bytes(f, id_bytes, 8);

        if constexpr (Version >= 5) {
            // result_metadata_id: stable hash of keyspace + table name.
            // @note If result metadata changes dynamically (e.g., schema evolution while a
            // prepared statement is cached), a richer versioning scheme (e.g., schema epoch)
            // will be needed here to correctly signal Metadata_changed to clients.
            XXHash64 h(0xCA550DA7_u64);
            h.add(entry.keyspace.c_str, entry.keyspace.length);
            h.add(entry.table.c_str, entry.table.length);
            U64 rmi = h.hash();
            U8 rmi_bytes[8];
            for (int i = 7; i >= 0; i--) { rmi_bytes[i] = U8(rmi); rmi >>= 8; }
            append_cql_short_bytes(f, rmi_bytes, 8);
        }

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

    template<U8 Version, bool Compressed>
    coroutine::Task<bool> send_error_if_failed(engine::ExecutionResult& result, const tcp::Request* req, S16 stream) {
        if (result.status != engine::ExecutionStatus::Success) {
            Frame frame{.body = {}, .req = req, .op = op_codes::ERROR, .stream = stream};
            String8 msg = result.message.length ? result.message : engine::to_str(result.status);
            append_error_body(frame, result.status, msg);
            co_await send_native_frame<Version, Compressed>(frame);
            co_return true;
        }
        co_return false;
    }

    template<U8 Version, bool Compressed>
    coroutine::Task<> send_execution_result(engine::ExecutionResult& result, Engine& engine, const tcp::Request* req, S16 stream) {
        switch (result.kind) {
            case engine::ResultKind::Void:{
                Frame frame{.body = {}, .req = req, .op = op_codes::RESULT, .stream = stream};
                append_result_void(frame);
                co_await send_native_frame<Version, Compressed>(frame);
            }break;
            case engine::ResultKind::SchemaChange:{
                String8 change_type = result.message.length ? result.message : "UPDATED";
                String8 target      = result.table.length   ? "TABLE"        : "KEYSPACE";
                Frame frame{.body = {}, .req = req, .op = op_codes::RESULT, .stream = stream};
                append_result_schema_change(frame, change_type, target, result.keyspace, result.table);
                co_await send_native_frame<Version, Compressed>(frame);
            }break;
            case engine::ResultKind::UseKeyspace:{
                Frame frame{.body = {}, .req = req, .op = op_codes::RESULT, .stream = stream};
                append_result_set_keyspace(frame, result.keyspace);
                co_await send_native_frame<Version, Compressed>(frame);
            }break;
            case engine::ResultKind::Rows:{
                auto ks = schema::read_keyspace(engine.schema, result.keyspace).value;
                assert_true(ks != nullptr, "keyspace not found for rows result");
                auto tbl = schema::read_table(engine.schema, *ks, result.table).value;
                assert_true(tbl != nullptr, "table not found for rows result");
                Frame frame{.body = {}, .req = req, .op = op_codes::RESULT, .stream = stream};
                co_await append_result_rows(frame, result, tbl);
                co_await send_native_frame<Version, Compressed>(frame);
                {
                    pager::begin_transaction(*engine.pager);
                    U64 tbl_size = co_await btree::size(tbl->btree);
                    co_await pager::commit_transaction(*engine.pager);
                    cql::log::db_response_returned_rows(tbl_size);
                }
            }break;
            case engine::ResultKind::VirtualRows:{
                assert_true(result.virtual_rows.has_value(), "virtual rows missing");
                Frame frame{.body = {}, .req = req, .op = op_codes::RESULT, .stream = stream};
                append_result_virtual_rows(frame, *result.virtual_rows);
                co_await send_native_frame<Version, Compressed>(frame);
                cql::log::db_response_returned_rows(result.virtual_rows->rows.length);
            }break;
        }
    }

    // ========================================================================
    //   Handles the pre-startup phase. Processes OPTIONS and STARTUP using
    //   unframed v4-style envelopes, negotiates version and compression,
    //   sends READY, and returns true on success.
    // ========================================================================
    coroutine::Task<bool> negotiate_connection(const tcp::Request& req, NegotiatedProtocol* opt_out_negotiated_protocol) {
        DynamicArray<U8> buf{};

        // Helper: send an unframed response envelope (STARTUP/OPTIONS responses
        // must never be wrapped in v5 outer frames per spec)
        const auto send_unframed_response = [&](
            U8 resp_op, U8 resp_ver, U8 resp_flags, S16 stream,
            const U8* body, U32 body_len
        ) -> coroutine::Task<> {
            log::native_info(fmt(
                "TX negotiate: op=%02x(%s) flags=%02x stream=%d",
                resp_op, (const char*)op_codes_to_str(resp_op), resp_flags, (int)stream
            ));

            U8 hdr[V4_FRAME_HEADER_BYTE_COUNT] = {
                U8(0x80u | resp_ver), resp_flags,
                U8(U16(stream) >> 8), U8(stream),
                resp_op,
                U8(body_len >> 24), U8(body_len >> 16), U8(body_len >> 8), U8(body_len)
            };
            co_await send_block(req, hdr, V4_FRAME_HEADER_BYTE_COUNT);
            if (body_len > 0) co_await send_block(req, body, body_len);
        };

        while (true) {
            while (buf.length < V4_FRAME_HEADER_BYTE_COUNT) {
                if (!co_await try_append_tcp_read(req, buf)) co_return false;
            }

            S32 body_len  = read_be_s32(&buf[5]);
            U64 frame_len = V4_FRAME_HEADER_BYTE_COUNT + U64(body_len < 0 ? 0 : body_len);
            while (buf.length < frame_len) {
                if (!co_await try_append_tcp_read(req, buf)) co_return false;
            }

            U8  version = buf[0] & 0x7Fu;
            S16 stream  = read_be_s16(&buf[2]);
            U8  op      = buf[4];
            const U8* body     = buf.ptr + V4_FRAME_HEADER_BYTE_COUNT;
            const U8* body_end = body + (body_len > 0 ? body_len : 0);

            log::native_info(fmt(
                "RX negotiate: op=%02x(%s) version=%d stream=%d",
                op, (const char*)op_codes_to_str(op), (int)version, (int)stream
            ));

            if (op == op_codes::OPTIONS) {
                // Advertise supported CQL_VERSION and COMPRESSION options.
                // List lz4 as a supported compression method.
                Frame tmp{.body = {}, .req = &req, .op = op_codes::SUPPORTED, .stream = stream};
                append_be_u16(tmp, 2);
                append_cql_string(tmp, "CQL_VERSION");
                append_be_u16(tmp, 1);
                append_cql_string(tmp, "3.0.0");
                append_cql_string(tmp, "COMPRESSION");
                append_be_u16(tmp, 1);
                append_cql_string(tmp, "lz4");
                co_await send_unframed_response(op_codes::SUPPORTED, 4, 0x00, stream, tmp.body.ptr, U32(tmp.body.length));
            } else if (op == op_codes::STARTUP) {
                if (version < 3 || version > 5) {
                    Frame tmp{.body = {}, .req = &req, .op = op_codes::ERROR, .stream = stream};
                    append_be_s32(tmp, error_codes::PROTOCOL_ERROR);
                    append_cql_string(tmp, "Invalid or unsupported protocol version (supported: 3, 4, 5)");
                    co_await send_unframed_response(op_codes::ERROR, 4, 0x00, stream, tmp.body.ptr, U32(tmp.body.length));
                    co_return false;
                }

                // Parse STARTUP body: [short map] of key → value strings
                bool want_lz4 = false;
                if (body + 2 <= body_end) {
                    const U8* p = body;
                    U16 n = read_be_u16(p); p += 2;
                    for (U16 i = 0; i < n && p < body_end; i++) {
                        String8 key = read_cql_string(p, body_end);
                        String8 val = read_cql_string(p, body_end);
                        if (key == "COMPRESSION" && val == "lz4")
                            want_lz4 = true;
                        // @todo unsupported options
                        // @todo CQL_VERSION, DRIVER_NAME, DRIVER_VERSION
                    }
                }

                opt_out_negotiated_protocol->version    = version;
                opt_out_negotiated_protocol->compressed = (version >= 5 && want_lz4);

                U8 ready_flags = (version >= 5) ? 0x10u : 0x00u;
                co_await send_unframed_response(op_codes::READY, version, ready_flags, stream, nullptr, 0);
                co_return true;
            } else {
                Frame tmp{.body = {}, .req = &req, .op = op_codes::ERROR, .stream = stream};
                append_be_s32(tmp, error_codes::PROTOCOL_ERROR);
                append_cql_string(tmp, "Expected STARTUP or OPTIONS before any other message");
                co_await send_unframed_response(op_codes::ERROR, 4, 0x00, stream, tmp.body.ptr, U32(tmp.body.length));
                co_return false;
            }

            // Consume the processed envelope from buf
            U64 rem = buf.length - frame_len;
            if (rem > 0) os::memory_copy(buf.ptr, buf.ptr + frame_len, rem);
            resize(buf, rem);
        }
    }

    // ========================================================================
    // frame_handler
    //   Handles all post-startup CQL messages. Called from post_startup_loop
    //   after outer-frame reassembly. v5-specific message format differences
    //   (32-bit flags, extra fields) are handled via if constexpr (Version >= 5).
    // ========================================================================
    template<U8 Version, bool Compressed>
    coroutine::Task<void> frame_handler(
        Engine& engine,
        const tcp::Request& req,
        const U8* header,
        const U8* body,
        S32 body_length
    ) { ZoneScopedN("frame_handler");
        S16 stream  = read_be_s16(header + 2);
        U8  op      = header[4];

        const U8* body_end = body + body_length;

        log::native_info(fmt(
            "RX frame: op=%02x(%s) stream=%d body_len=%d hdr=%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            op, (const char*)op_codes_to_str(op), (int)(S16)stream, body_length,
            header[0],header[1],header[2],header[3],header[4],
            header[5],header[6],header[7],header[8]
        ));

        switch (op) {
            case op_codes::STARTUP:{
                // STARTUP is handled in negotiate_connection before post_startup_loop.
                // A re-sent STARTUP post-negotiation is an error.
                Frame frame{.body = {}, .req = &req, .op = op_codes::ERROR, .stream = stream};
                append_be_s32(frame, error_codes::PROTOCOL_ERROR);
                append_cql_string(frame, "STARTUP sent after connection was already negotiated");
                co_await send_native_frame<Version, Compressed>(frame);
            }break;

            case op_codes::OPTIONS:{
                Frame frame{.body = {}, .req = &req, .op = op_codes::SUPPORTED, .stream = stream};
                append_be_u16(frame, 2);
                append_cql_string(frame, "CQL_VERSION");
                append_be_u16(frame, 1);
                append_cql_string(frame, "3.0.0");
                append_cql_string(frame, "COMPRESSION");
                append_be_u16(frame, 1);
                append_cql_string(frame, "lz4");
                co_await send_native_frame<Version, Compressed>(frame);
            }break;

            case op_codes::QUERY:{
                S64 t0 = os::monotonic_us();
                const U8* p = body;
                String8 query = read_cql_long_string(p, body_end);

                auto cql_opt = parsers::parse(query);
                if (!cql_opt) {
                    Frame frame{.body = {}, .req = &req, .op = op_codes::ERROR, .stream = stream};
                    append_error_body(frame, engine::ExecutionStatus::SyntaxError, "Failed to parse CQL");
                    co_await send_native_frame<Version, Compressed>(frame);
                    cql::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                auto bind_specs   = engine::collect_bind_variables(engine, *cql_opt);
                auto bound_values = read_query_parameter_values<Version>(p, body_end, bind_specs);

                engine::ExecutionResult result = co_await engine::execute(engine, *cql_opt, move(bound_values));
                if (!co_await send_error_if_failed<Version, Compressed>(result, &req, stream))
                    co_await send_execution_result<Version, Compressed>(result, engine, &req, stream);

                cql::log::db_operation_duration(os::monotonic_us() - t0);
            }break;

            case op_codes::REGISTER:{
                Frame frame{.body = {}, .req = &req, .op = op_codes::READY, .stream = stream};
                co_await send_native_frame<Version, Compressed>(frame);
            }break;

            case op_codes::PREPARE:{
                S64 t0 = os::monotonic_us();
                const U8* p = body;
                String8 query = read_cql_long_string(p, body_end);

                // v5 adds a flags [int] field and optional keyspace [string]
                if constexpr (Version >= 5) {
                    if (p + 4 <= body_end) {
                        U32 prepare_flags = U32(read_be_s32(p)); p += 4;
                        if ((prepare_flags & 0x01u) && p < body_end) {
                            String8 keyspace = read_cql_string(p, body_end);
                            (void)keyspace; // @todo use for per-query keyspace context
                        }
                    }
                }

                auto result = engine::prepare(engine, query);
                if (result.status != engine::ExecutionStatus::Success) {
                    Frame frame{.body = {}, .req = &req, .op = op_codes::ERROR, .stream = stream};
                    String8 msg = result.message.length ? result.message : engine::to_str(result.status);
                    append_error_body(frame, result.status, msg);
                    co_await send_native_frame<Version, Compressed>(frame);
                    cql::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                Frame frame{.body = {}, .req = &req, .op = op_codes::RESULT, .stream = stream};
                append_result_prepared<Version>(frame, result.id, *result.entry);
                co_await send_native_frame<Version, Compressed>(frame);
                cql::log::db_operation_duration(os::monotonic_us() - t0);
            }break;

            case op_codes::EXECUTE:{
                S64 t0 = os::monotonic_us();
                const U8* p = body;

                const U8* id_data = nullptr;
                U16 id_len = read_cql_short_bytes(p, body_end, id_data);
                U64 prepared_id = 0;
                for (U16 i = 0; i < id_len && i < 8; i++)
                    prepared_id = (prepared_id << 8) | U64(id_data[i]);

                // v5 includes result_metadata_id [short bytes] after the prepared id
                if constexpr (Version >= 5) {
                    const U8* rmi_data = nullptr;
                    read_cql_short_bytes(p, body_end, rmi_data); // read and discard
                }

                auto* entry = engine::try_get_prepared(engine, prepared_id);
                if (entry == nullptr) {
                    Frame frame{.body = {}, .req = &req, .op = op_codes::ERROR, .stream = stream};
                    append_error_body(frame, engine::ExecutionStatus::Invalid, "Prepared statement not found (unprepared)");
                    co_await send_native_frame<Version, Compressed>(frame);
                    cql::log::db_operation_duration(os::monotonic_us() - t0);
                    break;
                }

                auto bound_values = read_query_parameter_values<Version>(p, body_end, entry->bind_variables);

                engine::ExecutionResult result = co_await engine::execute(engine, prepared_id, move(bound_values));
                if (!co_await send_error_if_failed<Version, Compressed>(result, &req, stream))
                    co_await send_execution_result<Version, Compressed>(result, engine, &req, stream);

                cql::log::db_operation_duration(os::monotonic_us() - t0);
            }break;

            case op_codes::BATCH:{
                assert_true_not_implemented(false, "BATCH not implemented");
            }break;

            default:{
                Frame frame{.body = {}, .req = &req, .op = op_codes::ERROR, .stream = stream};
                append_error_body(frame, engine::ExecutionStatus::Invalid, "Unknown op code");
                co_await send_native_frame<Version, Compressed>(frame);
            }break;
        }

        co_return;
    }

    template coroutine::Task<void> frame_handler<5u, true> (Engine&, const tcp::Request&, const U8*, const U8*, S32);
    template coroutine::Task<void> frame_handler<5u, false>(Engine&, const tcp::Request&, const U8*, const U8*, S32);
    template coroutine::Task<void> frame_handler<4u, false>(Engine&, const tcp::Request&, const U8*, const U8*, S32);
}
