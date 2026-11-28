module;
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

module cql.native;

import plexdb.pager;

import cql.engine.evaluator;

using namespace cql::column_types;

namespace cql::native {
    // ========================================================================
    // write lock
    // ========================================================================
    struct LockWaiter {
        std::coroutine_handle<> handle{};
    };

    static void remove_waiter(WriteLock* l, LockWaiter* w) {
        for (U64 i = 0; i < l->waiters.length; i++) {
            if (l->waiters[i].address() == w->handle.address()) {
                for (U64 j = i + 1; j < l->waiters.length; j++) {
                    l->waiters[j - 1] = l->waiters[j];
                }
                pop_back(l->waiters);
                return;
            }
        }
    }

    coroutine::Task<> lock(WriteLock& l) {
        if (!l.held) {
            l.held = true;
            co_return;
        }
        LockWaiter  w{};
        WriteLock*  lp = &l;
        LockWaiter* wp = &w;
        // @note ownership is handed over by unlock(), so `held` stays true on resume
        co_await coroutine::Awaitable{
            [lp, wp](std::coroutine_handle<> h) {
                wp->handle = h;
                push_back(lp->waiters, h);
            },
            []() {},
            [lp, wp]() { remove_waiter(lp, wp); }
        };
    }

    void unlock(WriteLock& l) {
        assert_true(l.held, "unlock of a write lock that is not held");
        if (l.waiters.length == 0) {
            l.held = false;
            return;
        }
        std::coroutine_handle<> next = l.waiters[0];
        for (U64 j = 1; j < l.waiters.length; j++) {
            l.waiters[j - 1] = l.waiters[j];
        }
        pop_back(l.waiters);
        next.resume();
    }

    // ========================================================================
    // input
    // ========================================================================
    U16 read_be_u16(const U8* p) {
        return U16((U16(p[0]) << 8) | p[1]);
    }

    S16 read_be_s16(const U8* p) {
        return S16(read_be_u16(p));
    }

    S32 read_be_s32(const U8* p) {
        return S32((U32(p[0]) << 24) | (U32(p[1]) << 16) | (U32(p[2]) << 8) | p[3]);
    }

    S64 read_be_s64(const U8* p) {
        return S64(
            (U64(p[0]) << 56) | (U64(p[1]) << 48) | (U64(p[2]) << 40) | (U64(p[3]) << 32) | (U64(p[4]) << 24) | (U64(p[5]) << 16) | (U64(p[6]) << 8) | U64(p[7])
        );
    }

    bool reader_ok(const Reader& r) {
        return r.protocol_error.length == 0 && r.invalid_error.length == 0;
    }

    void reader_fail_protocol(Reader& r, String8 message) {
        if (reader_ok(r)) {
            r.protocol_error = message;
        }
        r.p = r.end;
    }

    void reader_fail_invalid(Reader& r, String8 message) {
        if (reader_ok(r)) {
            r.invalid_error = message;
        }
        r.p = r.end;
    }

    static void reader_fail_invalid_owned(Reader& r, AutoString8&& message) {
        if (reader_ok(r)) {
            r.error_storage = move(message);
            r.invalid_error = String8(r.error_storage);
        }
        r.p = r.end;
    }

    bool reader_need(Reader& r, U64 n) {
        if (!reader_ok(r)) {
            return false;
        }
        if (U64(r.end - r.p) < n) {
            reader_fail_protocol(r, "Not enough bytes to read the message");
            return false;
        }
        return true;
    }

    U8 read_u8(Reader& r) {
        if (!reader_need(r, 1)) {
            return 0;
        }
        return *r.p++;
    }

    U16 read_u16(Reader& r) {
        if (!reader_need(r, 2)) {
            return 0;
        }
        U16 v = read_be_u16(r.p);
        r.p += 2;
        return v;
    }

    S32 read_s32(Reader& r) {
        if (!reader_need(r, 4)) {
            return 0;
        }
        S32 v = read_be_s32(r.p);
        r.p += 4;
        return v;
    }

    S64 read_s64(Reader& r) {
        if (!reader_need(r, 8)) {
            return 0;
        }
        S64 v = read_be_s64(r.p);
        r.p += 8;
        return v;
    }

    String8 read_string(Reader& r) {
        U16 len = read_u16(r);
        if (!reader_need(r, len)) {
            return String8{};
        }
        String8 s(reinterpret_cast<const char*>(r.p), U64(len));
        r.p += len;
        return s;
    }

    String8 read_long_string(Reader& r) {
        S32 len = read_s32(r);
        if (!reader_ok(r)) {
            return String8{};
        }
        if (len < 0) {
            reader_fail_protocol(r, "Invalid negative long string length");
            return String8{};
        }
        if (!reader_need(r, U64(len))) {
            return String8{};
        }
        String8 s(reinterpret_cast<const char*>(r.p), U64(len));
        r.p += len;
        return s;
    }

    TArrayView<const U8> read_short_bytes(Reader& r) {
        U16 len = read_u16(r);
        if (!reader_need(r, len)) {
            return TArrayView<const U8>{};
        }
        TArrayView<const U8> v(r.p, len);
        r.p += len;
        return v;
    }

    S32 read_bytes(Reader& r, const U8*& out_data) {
        out_data = nullptr;
        S32 len  = read_s32(r);
        if (!reader_ok(r)) {
            return -1;
        }
        if (len < 0) {
            if (len != -1 && len != -2) {
                reader_fail_protocol(r, "Invalid negative value length");
            }
            return len;
        }
        if (!reader_need(r, U64(len))) {
            return -1;
        }
        out_data = r.p;
        r.p += len;
        return len;
    }

    // [bytes map]: [short] n, then n ([string] key, [bytes] value)
    static void skip_bytes_map(Reader& r) {
        U16 n = read_u16(r);
        for (U16 i = 0; i < n && reader_ok(r); i++) {
            (void)read_string(r);
            const U8* data = nullptr;
            (void)read_bytes(r, data);
        }
    }

    // ------------------------------------------------------------------------
    // value validation
    // ------------------------------------------------------------------------
    static bool is_valid_utf8(const U8* s, U64 n) {
        U64 i = 0;
        while (i < n) {
            U8 c = s[i];
            if (c < 0x80) {
                i++;
                continue;
            }
            U64 extra = 0;
            U32 cp    = 0;
            if ((c & 0xE0) == 0xC0) {
                extra = 1;
                cp    = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                extra = 2;
                cp    = c & 0x0F;
            } else if ((c & 0xF8) == 0xF0) {
                extra = 3;
                cp    = c & 0x07;
            } else {
                return false;
            }
            if (i + extra >= n) {
                return false;
            }
            for (U64 k = 1; k <= extra; k++) {
                if ((s[i + k] & 0xC0) != 0x80) {
                    return false;
                }
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
            constexpr U32 min_for_len[4] = {0, 0x80, 0x800, 0x10000};
            if (cp < min_for_len[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return false;
            }
            i += extra + 1;
        }
        return true;
    }

    // @note fixed width of a basic type's native body, or -1 for variable width
    static S32 basic_fixed_width(type::Basic b) {
        switch (b) {
            case type::Basic::tinyint:
            case type::Basic::boolean:
                return 1;
            case type::Basic::smallint:
                return 2;
            case type::Basic::int_:
            case type::Basic::date:
            case type::Basic::float_:
                return 4;
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time:
            case type::Basic::double_:
                return 8;
            case type::Basic::uuid:
            case type::Basic::timeuuid:
                return 16;
            default:
                return -1;
        }
    }

    // Unsigned VInt (Cassandra VIntCoding without zigzag), used by vector element lengths.
    static bool read_unsigned_vint(const U8*& p, const U8* end, U64& out) {
        if (p >= end) {
            return false;
        }
        U8  first = *p++;
        U32 extra = 0;
        U8  mask  = 0x80;
        while (extra < 8 && (first & mask)) {
            extra++;
            mask = U8(mask >> 1);
        }
        if (U64(end - p) < extra) {
            return false;
        }
        U64 value = U64(first & U8(0xFFu >> extra));
        if (extra == 8) {
            value = 0;
        }
        for (U32 i = 0; i < extra; i++) {
            value = (value << 8) | U64(*p++);
        }
        out = value;
        return true;
    }

    static void append_unsigned_vint(DynamicArray<U8>& out, U64 value) {
        U32 extra = 0;
        while (extra < 8 && value >= (U64(1) << (7 * (extra + 1)))) {
            extra++;
        }
        if (extra == 8) {
            push_back(out, U8(0xFF));
            for (int i = 7; i >= 0; i--) {
                push_back(out, U8(value >> (8 * i)));
            }
            return;
        }
        U8 prefix = U8(~(0xFFu >> extra));
        push_back(out, U8(prefix | U8(value >> (8 * extra))));
        for (int i = int(extra) - 1; i >= 0; i--) {
            push_back(out, U8(value >> (8 * i)));
        }
    }

    // Duration: three signed vints (months, days, nanoseconds) with a common sign.
    static bool validate_duration(Reader& r, const U8* data, S32 len) {
        const U8* p      = data;
        const U8* end    = data + len;
        S64       parts[3] = {};
        for (int i = 0; i < 3; i++) {
            U64 raw = 0;
            if (!read_unsigned_vint(p, end, raw)) {
                reader_fail_invalid(r, "Invalid duration value: truncated vint");
                return false;
            }
            parts[i] = S64(raw >> 1) ^ -S64(raw & 1);
        }
        if (p != end) {
            reader_fail_invalid(r, "Invalid duration value: unexpected trailing bytes");
            return false;
        }
        if (parts[0] < MIN_S32 || parts[0] > MAX_S32 || parts[1] < MIN_S32 || parts[1] > MAX_S32) {
            reader_fail_invalid(r, "Invalid duration value: months and days must fit in a 32-bit integer");
            return false;
        }
        bool any_neg = parts[0] < 0 || parts[1] < 0 || parts[2] < 0;
        bool any_pos = parts[0] > 0 || parts[1] > 0 || parts[2] > 0;
        if (any_neg && any_pos) {
            reader_fail_invalid(r, "The duration months, days and nanoseconds must be all of the same sign");
            return false;
        }
        return true;
    }

    static bool validate_basic(Reader& r, type::Basic b, const U8* data, S32 len) {
        S32 width = basic_fixed_width(b);
        if (width > 0 && len != width) {
            reader_fail_invalid_owned(r, fmt("Expected %d bytes for this value type but got %d", width, len));
            return false;
        }
        switch (b) {
            case type::Basic::timeuuid:
                if ((data[6] >> 4) != 1) {
                    reader_fail_invalid(r, "Unsupported UUID for timeuuid: expected a version 1 (time-based) UUID");
                    return false;
                }
                return true;
            case type::Basic::time: {
                S64 ns = read_be_s64(data);
                if (ns < 0 || ns > 86399999999999) {
                    reader_fail_invalid(r, "Invalid time value: must be between 0 and 86399999999999 nanoseconds");
                    return false;
                }
                return true;
            }
            case type::Basic::inet:
                if (len != 4 && len != 16) {
                    reader_fail_invalid_owned(r, fmt("Invalid inet value: expected 4 or 16 bytes but got %d", len));
                    return false;
                }
                return true;
            case type::Basic::decimal:
                if (len < 5) {
                    reader_fail_invalid_owned(r, fmt("Invalid decimal value: expected at least 5 bytes but got %d", len));
                    return false;
                }
                return true;
            case type::Basic::varint:
                if (len < 1) {
                    reader_fail_invalid(r, "Invalid varint value: empty value");
                    return false;
                }
                return true;
            case type::Basic::duration:
                return validate_duration(r, data, len);
            case type::Basic::ascii:
                for (S32 i = 0; i < len; i++) {
                    if (data[i] >= 0x80) {
                        reader_fail_invalid(r, "Invalid byte for ascii");
                        return false;
                    }
                }
                return true;
            case type::Basic::text:
            case type::Basic::varchar:
                if (!is_valid_utf8(data, U64(len))) {
                    reader_fail_invalid(r, "Invalid UTF-8 bytes");
                    return false;
                }
                return true;
            case type::Basic::hex:
            case type::Basic::COUNT:
                reader_fail_invalid(r, "Invalid bind type");
                return false;
            default:
                return true;
        }
    }

    enum class ValueContext : U8 {
        Top,     // a bind value: null and (v4+) unset allowed
        Element, // a collection element: neither null nor unset allowed
        Field,   // a tuple / UDT field: null allowed, unset not
    };

    static Term read_value_in_context(Reader& r, const type::Type& dtype, U8 version, ValueContext ctx);

    // Decodes a non-null value body. `data - 4` must be the value's [int] length prefix.
    static Term decode_value_body(Reader& r, const type::Type& dtype, const U8* data, S32 len, U8 version) {
        return visit(dtype.value, [&](const auto& v) -> Term {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                if (!validate_basic(r, v, data, len)) {
                    return Term{.value = Literal{.value = Null{}}};
                }
                const U8* q = data - 4;
                return native::wire::read_native_basic_value_as_term(q, data + len, v);
            } else if constexpr (SameAs<T, type::List> || SameAs<T, type::Set>) {
                constexpr bool is_list = SameAs<T, type::List>;
                Reader         cr{.p = data, .end = data + len};
                S32            count = read_s32(cr);
                if (reader_ok(cr) && count < 0) {
                    reader_fail_invalid(cr, "Invalid negative collection size");
                }
                const type::Type* elem_type_ptr = nullptr;
                if constexpr (is_list) {
                    elem_type_ptr = &v.element;
                } else {
                    elem_type_ptr = &v.key;
                }
                const type::Type&  elem_type = *elem_type_ptr;
                DynamicArray<Term> elements;
                for (S32 i = 0; i < count && reader_ok(cr); i++) {
                    push_back(elements, read_value_in_context(cr, elem_type, version, ValueContext::Element));
                }
                if (reader_ok(cr) && cr.p != cr.end) {
                    reader_fail_invalid(cr, "Unexpected extraneous bytes after collection value");
                }
                if (!reader_ok(cr)) {
                    reader_fail_invalid_owned(r, fmt("Invalid collection value: %s", (const char*)AutoString8(cr.invalid_error.length ? cr.invalid_error : cr.protocol_error).c_str));
                    return Term{.value = Literal{.value = Null{}}};
                }
                if constexpr (is_list) {
                    return Term{.value = ListOrVectorLiteral{.elements = move(elements)}};
                } else {
                    return Term{.value = SetLiteral{.keys = move(elements)}};
                }
            } else if constexpr (SameAs<T, type::Map>) {
                Reader cr{.p = data, .end = data + len};
                S32    count = read_s32(cr);
                if (reader_ok(cr) && count < 0) {
                    reader_fail_invalid(cr, "Invalid negative collection size");
                }
                MapLiteral lit{};
                for (S32 i = 0; i < count && reader_ok(cr); i++) {
                    Term key = read_value_in_context(cr, v.key, version, ValueContext::Element);
                    Term val = read_value_in_context(cr, v.value, version, ValueContext::Element);
                    push_back(lit.key_values, Pair<Term, Term>{move(key), move(val)});
                }
                if (reader_ok(cr) && cr.p != cr.end) {
                    reader_fail_invalid(cr, "Unexpected extraneous bytes after map value");
                }
                if (!reader_ok(cr)) {
                    reader_fail_invalid_owned(r, fmt("Invalid map value: %s", (const char*)AutoString8(cr.invalid_error.length ? cr.invalid_error : cr.protocol_error).c_str));
                    return Term{.value = Literal{.value = Null{}}};
                }
                return Term{.value = move(lit)};
            } else if constexpr (SameAs<T, type::Tuple> || SameAs<T, type::UDT*>) {
                constexpr bool    is_tuple    = SameAs<T, type::Tuple>;
                const type::Type* field_types = nullptr;
                U64               field_count = 0;
                if constexpr (is_tuple) {
                    field_types = v.elements.ptr;
                    field_count = v.elements.length;
                } else {
                    field_types = v->field_types.ptr;
                    field_count = v->field_types.length;
                }
                Reader             cr{.p = data, .end = data + len};
                DynamicArray<Term> fields;
                // @note trailing fields may be omitted (they read as null)
                for (U64 i = 0; cr.p < cr.end && reader_ok(cr); i++) {
                    if (i >= field_count) {
                        reader_fail_invalid(cr, "Too many fields in tuple or user type value");
                        break;
                    }
                    push_back(fields, read_value_in_context(cr, field_types[i], version, ValueContext::Field));
                }
                if (!reader_ok(cr)) {
                    reader_fail_invalid_owned(r, fmt("Invalid tuple or user type value: %s", (const char*)AutoString8(cr.invalid_error.length ? cr.invalid_error : cr.protocol_error).c_str));
                    return Term{.value = Literal{.value = Null{}}};
                }
                if constexpr (is_tuple) {
                    return Term{.value = TupleLiteral{.elements = move(fields)}};
                } else {
                    UdtLiteral lit{};
                    for (U64 i = 0; i < fields.length; i++) {
                        push_back(lit.identifier_values, Pair<AutoString8, Term>{AutoString8(v->field_names[i]), move(fields[i])});
                    }
                    return Term{.value = move(lit)};
                }
            } else if constexpr (SameAs<T, type::Vector>) {
                if (!type_matches_tag<type::Basic>(v.element.value)) {
                    reader_fail_invalid(r, "Vectors of non-scalar elements are not supported as bind values");
                    return Term{.value = Literal{.value = Null{}}};
                }
                const type::Basic eb    = get<type::Basic>(v.element.value);
                S32               width = basic_fixed_width(eb);
                const U8*         p     = data;
                const U8*         end   = data + len;
                ListOrVectorLiteral lit{};
                // Each element is re-framed as [int len][body] so the scalar decoder can be reused.
                DynamicArray<U8> scratch;
                for (U64 i = 0; i < v.count; i++) {
                    U64 elen = 0;
                    if (width > 0) {
                        elen = U64(width);
                    } else if (!read_unsigned_vint(p, end, elen)) {
                        reader_fail_invalid(r, "Invalid vector value: truncated element length");
                        return Term{.value = Literal{.value = Null{}}};
                    }
                    if (U64(end - p) < elen || elen > U64(MAX_S32)) {
                        reader_fail_invalid(r, "Invalid vector value: not enough bytes for the declared dimension");
                        return Term{.value = Literal{.value = Null{}}};
                    }
                    resize(scratch, 4 + elen);
                    scratch[0] = U8(elen >> 24);
                    scratch[1] = U8(elen >> 16);
                    scratch[2] = U8(elen >> 8);
                    scratch[3] = U8(elen);
                    os::memory_copy(scratch.ptr + 4, p, elen);
                    p += elen;
                    Term t = decode_value_body(r, v.element, scratch.ptr + 4, S32(elen), version);
                    if (!reader_ok(r)) {
                        return Term{.value = Literal{.value = Null{}}};
                    }
                    push_back(lit.elements, move(t));
                }
                if (p != end) {
                    reader_fail_invalid(r, "Invalid vector value: unexpected trailing bytes");
                    return Term{.value = Literal{.value = Null{}}};
                }
                return Term{.value = move(lit)};
            } else {
                static_assert(!SameAs<T, T>, "missing bind decode case");
            }
        });
    }

    static Term read_value_in_context(Reader& r, const type::Type& dtype, U8 version, ValueContext ctx) {
        const U8* data = nullptr;
        S32       len  = read_bytes(r, data);
        if (!reader_ok(r)) {
            return Term{.value = Literal{.value = Null{}}};
        }
        if (len == -2) {
            if (version < 4) {
                reader_fail_protocol(r, "Unset values are not supported before protocol v4");
            } else if (ctx != ValueContext::Top) {
                reader_fail_invalid(r, "Unset values are not allowed inside collections, tuples or user types");
            }
            return Term{.value = Literal{.value = Unset{}}};
        }
        if (len < 0) {
            if (ctx == ValueContext::Element) {
                reader_fail_invalid(r, "null is not supported inside collections");
            }
            return Term{.value = Literal{.value = Null{}}};
        }
        return decode_value_body(r, dtype, data, len, version);
    }

    Term read_value_as_term(Reader& r, const type::Type& dtype, U8 version) {
        return read_value_in_context(r, dtype, version, ValueContext::Top);
    }

    // ------------------------------------------------------------------------
    // query parameters
    // ------------------------------------------------------------------------
    struct RawValue {
        String8   name; // empty for positional values
        const U8* at;   // start of the [value]'s [int] length prefix
    };

    struct RawQueryParameters {
        QueryParameters        params;
        DynamicArray<RawValue> raw_values;
        bool                   has_values = false;
        bool                   named      = false;
    };

    namespace query_flags {
        constexpr U32 VALUES             = 0x0001;
        constexpr U32 SKIP_METADATA      = 0x0002;
        constexpr U32 PAGE_SIZE          = 0x0004;
        constexpr U32 PAGING_STATE       = 0x0008;
        constexpr U32 SERIAL_CONSISTENCY = 0x0010;
        constexpr U32 DEFAULT_TIMESTAMP  = 0x0020;
        constexpr U32 NAMES_FOR_VALUES   = 0x0040;
        constexpr U32 KEYSPACE           = 0x0080;
        constexpr U32 NOW_IN_SECONDS     = 0x0100;
    }

    static void skip_value(Reader& r) {
        const U8* data = nullptr;
        (void)read_bytes(r, data);
    }

    // Structural pass over <query_parameters>; values are decoded later, once the
    // statement's keyspace (which follows them in v5) is known.
    static RawQueryParameters read_raw_query_parameters(Reader& r, U8 version) {
        RawQueryParameters out{};
        out.params.consistency = read_u16(r);
        out.params.flags       = version >= 5 ? U32(read_s32(r)) : U32(read_u8(r));
        U32 flags              = out.params.flags;

        if (flags & query_flags::VALUES) {
            out.has_values = true;
            out.named      = (flags & query_flags::NAMES_FOR_VALUES) != 0;
            U16 n          = read_u16(r);
            for (U16 i = 0; i < n && reader_ok(r); i++) {
                RawValue rv{};
                if (out.named) {
                    rv.name = read_string(r);
                }
                rv.at = r.p;
                skip_value(r);
                push_back(out.raw_values, rv);
            }
        }
        out.params.skip_metadata = (flags & query_flags::SKIP_METADATA) != 0;
        if (flags & query_flags::PAGE_SIZE) {
            out.params.page_size = read_s32(r);
        }
        if (flags & query_flags::PAGING_STATE) {
            out.params.has_paging = true;
            skip_value(r);
        }
        if (flags & query_flags::SERIAL_CONSISTENCY) {
            (void)read_u16(r);
        }
        if (flags & query_flags::DEFAULT_TIMESTAMP) {
            out.params.timestamp = read_s64(r);
        }
        if (version >= 5) {
            if (flags & query_flags::KEYSPACE) {
                out.params.keyspace = read_string(r);
            }
            if (flags & query_flags::NOW_IN_SECONDS) {
                (void)read_s32(r);
            }
        }
        return out;
    }

    static DynamicArray<Term> decode_bound_values(Reader& err, const RawQueryParameters& raw, const DynamicArray<engine::BindVariableSpec>& specs, U8 version) {
        DynamicArray<Term> values;
        if (!raw.named) {
            if (raw.raw_values.length != specs.length) {
                reader_fail_invalid_owned(err, fmt("Invalid amount of bind variables: expected %llu, got %llu", (unsigned long long)specs.length, (unsigned long long)raw.raw_values.length));
                return values;
            }
            for (U64 i = 0; i < specs.length && reader_ok(err); i++) {
                Reader vr{.p = raw.raw_values[i].at, .end = err.end};
                push_back(values, read_value_as_term(vr, specs[i].type, version));
                if (!reader_ok(vr)) {
                    vr.invalid_error.length ? reader_fail_invalid_owned(err, AutoString8(vr.invalid_error)) : reader_fail_protocol(err, vr.protocol_error);
                }
            }
            return values;
        }

        DynamicArray<bool> filled;
        for (U64 i = 0; i < specs.length; i++) {
            push_back(values, Term{.value = Literal{.value = Unset{}}});
            push_back(filled, false);
        }
        for (const RawValue& rv : raw.raw_values) {
            bool matched = false;
            for (U64 i = 0; i < specs.length && reader_ok(err); i++) {
                if (String8(specs[i].name) != rv.name) {
                    continue;
                }
                matched = true;
                Reader vr{.p = rv.at, .end = err.end};
                values[i] = read_value_as_term(vr, specs[i].type, version);
                filled[i] = true;
                if (!reader_ok(vr)) {
                    vr.invalid_error.length ? reader_fail_invalid_owned(err, AutoString8(vr.invalid_error)) : reader_fail_protocol(err, vr.protocol_error);
                }
            }
            if (!matched && reader_ok(err)) {
                reader_fail_invalid_owned(err, fmt("No bind variable named '%s'", (const char*)AutoString8(rv.name).c_str));
            }
        }
        for (U64 i = 0; i < specs.length && reader_ok(err); i++) {
            if (!filled[i] && version < 4) {
                reader_fail_invalid_owned(err, fmt("Missing value for bind variable '%s'", (const char*)specs[i].name.c_str));
            }
        }
        return values;
    }

    QueryParameters read_query_parameters(Reader& r, const DynamicArray<engine::BindVariableSpec>& bind_specs, U8 version) {
        RawQueryParameters raw = read_raw_query_parameters(r, version);
        if (reader_ok(r)) {
            raw.params.values = decode_bound_values(r, raw, bind_specs, version);
        }
        return move(raw.params);
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

    // ========================================================================
    // output
    // ========================================================================
    coroutine::Task<> send_block(const tcp::Request& req, const U8* data, U64 len) {
        while (len > 0) {
            auto buf   = co_await tcp::acquire(req);
            U64  chunk = min(len, static_cast<U64>(buf.length));
            os::memory_copy(buf.view.ptr, data, chunk);
            buf.view.length = chunk;
            tcp::Error err  = co_await tcp::write(req, &buf);
            tcp::release(req, &buf);

            if (err != tcp::Error::None) {
                co_return;
            }

            data += chunk;
            len -= chunk;
        }
    }

    static coroutine::Task<> send_bytes(ConnectionState& c, const U8* data, U64 len) {
        while (len > 0 && !c.write_failed) {
            auto buf   = co_await tcp::acquire(*c.req);
            U64  chunk = min(len, static_cast<U64>(buf.length));
            os::memory_copy(buf.view.ptr, data, chunk);
            buf.view.length = chunk;
            tcp::Error err  = co_await tcp::write(*c.req, &buf);
            tcp::release(*c.req, &buf);
            if (err != tcp::Error::None) {
                c.write_failed = true;
                co_return;
            }
            data += chunk;
            len -= chunk;
        }
    }

    // v5 outer framing. A payload that fits one frame is sent self-contained; a larger
    // envelope is split into frames that are all marked not self-contained, as the spec
    // requires, so the receiver reassembles by the envelope's own length.
    static coroutine::Task<> send_v5_outer_frames(ConnectionState& c, const U8* data, U64 data_len) {
        const bool self_contained = data_len <= V5_MAX_PAYLOAD_BYTE_COUNT;
        U64        offset         = 0;
        do {
            U64 chunk = min(data_len - offset, V5_MAX_PAYLOAD_BYTE_COUNT);

            if (c.compressed) {
                S32              bound = lz4::compress_bound(S32(chunk));
                DynamicArray<U8> cbuf{};
                resize(cbuf, U64(bound));
                S32 comp_len = lz4::compress(data + offset, cbuf.ptr, S32(chunk), bound);
                // @note an uncompressed length of 0 marks the payload as sent uncompressed
                bool        use_raw     = comp_len <= 0 || U64(comp_len) >= chunk;
                const U8*   payload     = use_raw ? data + offset : cbuf.ptr;
                U32         payload_len = use_raw ? U32(chunk) : U32(comp_len);
                U32         u           = use_raw ? 0u : U32(chunk);
                U8          bits[5]     = {
                    U8(payload_len),
                    U8(payload_len >> 8),
                    U8(U8(payload_len >> 16 & 0x01u) | U8((u & 0x7Fu) << 1)),
                    U8(u >> 7),
                    U8(U8(u >> 15 & 0x03u) | U8(U8(self_contained) << 2))
                };
                U32 hcrc                                                  = crc::crc24(bits, 5);
                U8  hdr[V5_COMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT] = {
                    bits[0], bits[1], bits[2], bits[3], bits[4],
                    U8(hcrc), U8(hcrc >> 8), U8(hcrc >> 16)
                };
                U32 pcrc       = crc::crc32(payload, payload_len, crc::CRC32_CQL_V5_INIT);
                U8  trailer[4] = {U8(pcrc), U8(pcrc >> 8), U8(pcrc >> 16), U8(pcrc >> 24)};
                co_await send_bytes(c, hdr, sizeof(hdr));
                co_await send_bytes(c, payload, payload_len);
                co_await send_bytes(c, trailer, 4);
            } else {
                U32 pl      = U32(chunk);
                U8  bits[3] = {
                    U8(pl), U8(pl >> 8),
                    U8(U8(pl >> 16 & 0x01u) | U8(U8(self_contained) << 1))
                };
                U32 hcrc                                                    = crc::crc24(bits, 3);
                U8  hdr[V5_UNCOMP_HDR_BYTE_COUNT + V5_HDR_CRC24_BYTE_COUNT] = {
                    bits[0], bits[1], bits[2],
                    U8(hcrc), U8(hcrc >> 8), U8(hcrc >> 16)
                };
                U32 pcrc       = crc::crc32(data + offset, chunk, crc::CRC32_CQL_V5_INIT);
                U8  trailer[4] = {U8(pcrc), U8(pcrc >> 8), U8(pcrc >> 16), U8(pcrc >> 24)};
                co_await send_bytes(c, hdr, sizeof(hdr));
                co_await send_bytes(c, data + offset, chunk);
                co_await send_bytes(c, trailer, 4);
            }

            offset += chunk;
        } while (offset < data_len);
    }

    coroutine::Task<> send_frame(Frame& f) {
        ZoneScopedN("send_frame");
        ConnectionState& c = *f.conn;

        assert_true(f.body.length <= MAX_U32, "body length too large");
        U32 body_len = U32(f.body.length);
        // @note responses are never envelope-compressed on v3/v4 (the flag is per envelope and
        // clients honour it); v5 compresses at the outer-frame level.
        U8 header[V4_FRAME_HEADER_BYTE_COUNT] = {
            U8(0x80u | c.version), 0x00,
            U8(U16(f.stream) >> 8), U8(f.stream),
            f.op,
            U8(body_len >> 24), U8(body_len >> 16), U8(body_len >> 8), U8(body_len)
        };

        log::native_info(fmt("TX frame: op=%02x(%s) stream=%d body_len=%u", f.op, (const char*)op_codes_to_str(f.op), (int)f.stream, body_len));

        DynamicArray<U8> env{};
        resize(env, V4_FRAME_HEADER_BYTE_COUNT + body_len);
        os::memory_copy(env.ptr, header, V4_FRAME_HEADER_BYTE_COUNT);
        if (body_len > 0) {
            os::memory_copy(env.ptr + V4_FRAME_HEADER_BYTE_COUNT, f.body.ptr, body_len);
        }

        co_await lock(c.write_lock);
        if (c.framed) {
            co_await send_v5_outer_frames(c, env.ptr, env.length);
        } else {
            co_await send_bytes(c, env.ptr, env.length);
        }
        unlock(c.write_lock);

        f.body.length = 0;
    }

    void append_byte(Frame& f, U8 v) {
        push_back(f.body, v);
    }

    void append_bytes(Frame& f, const U8* data, U64 n) {
        U64 pos = f.body.length;
        resize(f.body, pos + n);
        if (n > 0) {
            os::memory_copy(f.body.ptr + pos, data, n);
        }
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
        assert_true(s.length <= MAX_U16, "[string] longer than 65535 bytes");
        append_be_u16(f, U16(s.length));
        append_bytes(f, reinterpret_cast<const U8*>(s.data), s.length);
    }

    void append_cql_short_bytes(Frame& f, const U8* data, U16 n) {
        append_be_u16(f, n);
        append_bytes(f, data, n);
    }

    void append_cql_bytes_raw(Frame& f, const U8* data, S32 n) {
        append_be_s32(f, n);
        if (n > 0) {
            append_bytes(f, data, U64(n));
        }
    }

    static void append_string_list(Frame& f, const String8* items, U16 n) {
        append_be_u16(f, n);
        for (U16 i = 0; i < n; i++) {
            append_cql_string(f, items[i]);
        }
    }

    // Cassandra marshal class name, used for custom type options (v3 scalar types and vectors).
    static void append_marshal_class(AutoString8& out, const type::Type& t) {
        const String8 prefix = "org.apache.cassandra.db.marshal.";
        auto              hex    = [&out](String8 s) {
            constexpr char digits[] = "0123456789abcdef";
            for (U64 i = 0; i < s.length; i++) {
                push_back(out, digits[U8(s.data[i]) >> 4]);
                push_back(out, digits[U8(s.data[i]) & 0xF]);
            }
        };
        visit(t.value, [&](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            append(out, prefix);
            if constexpr (SameAs<T, type::Basic>) {
                String8 name = "BytesType";
                switch (v) {
                    case type::Basic::ascii: name = "AsciiType"; break;
                    case type::Basic::bigint: name = "LongType"; break;
                    case type::Basic::blob: name = "BytesType"; break;
                    case type::Basic::boolean: name = "BooleanType"; break;
                    case type::Basic::counter: name = "CounterColumnType"; break;
                    case type::Basic::date: name = "SimpleDateType"; break;
                    case type::Basic::decimal: name = "DecimalType"; break;
                    case type::Basic::double_: name = "DoubleType"; break;
                    case type::Basic::duration: name = "DurationType"; break;
                    case type::Basic::float_: name = "FloatType"; break;
                    case type::Basic::inet: name = "InetAddressType"; break;
                    case type::Basic::int_: name = "Int32Type"; break;
                    case type::Basic::smallint: name = "ShortType"; break;
                    case type::Basic::text:
                    case type::Basic::varchar: name = "UTF8Type"; break;
                    case type::Basic::time: name = "TimeType"; break;
                    case type::Basic::timestamp: name = "TimestampType"; break;
                    case type::Basic::tinyint: name = "ByteType"; break;
                    case type::Basic::uuid: name = "UUIDType"; break;
                    case type::Basic::timeuuid: name = "TimeUUIDType"; break;
                    case type::Basic::varint: name = "IntegerType"; break;
                    case type::Basic::hex:
                    case type::Basic::COUNT: assert_true(false, "invalid basic type for marshal class"); break;
                }
                append(out, name);
            } else if constexpr (SameAs<T, type::List>) {
                append(out, "ListType(");
                append_marshal_class(out, v.element);
                push_back(out, ')');
            } else if constexpr (SameAs<T, type::Set>) {
                append(out, "SetType(");
                append_marshal_class(out, v.key);
                push_back(out, ')');
            } else if constexpr (SameAs<T, type::Map>) {
                append(out, "MapType(");
                append_marshal_class(out, v.key);
                push_back(out, ',');
                append_marshal_class(out, v.value);
                push_back(out, ')');
            } else if constexpr (SameAs<T, type::Tuple>) {
                append(out, "TupleType(");
                for (U64 i = 0; i < v.elements.length; i++) {
                    if (i > 0) {
                        push_back(out, ',');
                    }
                    append_marshal_class(out, v.elements[i]);
                }
                push_back(out, ')');
            } else if constexpr (SameAs<T, type::UDT*>) {
                append(out, "UserType(");
                append(out, v->keyspace);
                push_back(out, ',');
                hex(v->name);
                for (U64 i = 0; i < v->field_names.length; i++) {
                    push_back(out, ',');
                    hex(v->field_names[i]);
                    push_back(out, ':');
                    append_marshal_class(out, v->field_types[i]);
                }
                push_back(out, ')');
            } else if constexpr (SameAs<T, type::Vector>) {
                append(out, "VectorType(");
                append_marshal_class(out, v.element);
                append(out, String8(fmt(",%llu)", (unsigned long long)v.count)));
            } else {
                static_assert(!SameAs<T, T>, "missing marshal class case");
            }
        });
    }

    static void append_custom_type(Frame& f, const type::Type& t) {
        AutoString8 cls{""};
        append_marshal_class(cls, t);
        append_be_u16(f, type_codes::Custom);
        append_cql_string(f, String8(cls));
    }

    void append_type_codes_option(Frame& f, type::Type cdtype) {
        const U8 version = f.conn->version;
        visit(cdtype.value, [&](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                // @note date, time, smallint, tinyint and duration have native codes from v4
                const bool v4_only = v == type::Basic::date || v == type::Basic::time || v == type::Basic::smallint || v == type::Basic::tinyint || v == type::Basic::duration;
                if (version < 4 && v4_only) {
                    append_custom_type(f, cdtype);
                } else {
                    append_be_u16(f, basic_type_to_type_code(v));
                }
            } else if constexpr (SameAs<T, type::List>) {
                append_be_u16(f, type_codes::List);
                append_type_codes_option(f, v.element);
            } else if constexpr (SameAs<T, type::Set>) {
                append_be_u16(f, type_codes::Set);
                append_type_codes_option(f, v.key);
            } else if constexpr (SameAs<T, type::Map>) {
                append_be_u16(f, type_codes::Map);
                append_type_codes_option(f, v.key);
                append_type_codes_option(f, v.value);
            } else if constexpr (SameAs<T, type::Tuple>) {
                append_be_u16(f, type_codes::Tuple);
                append_be_u16(f, U16(v.elements.length));
                for (U64 i = 0; i < v.elements.length; i++) {
                    append_type_codes_option(f, v.elements[i]);
                }
            } else if constexpr (SameAs<T, type::UDT*>) {
                append_be_u16(f, type_codes::Udt);
                append_cql_string(f, v->keyspace);
                append_cql_string(f, v->name);
                append_be_u16(f, U16(v->field_names.length));
                for (U64 i = 0; i < v->field_names.length; i++) {
                    append_cql_string(f, v->field_names[i]);
                    append_type_codes_option(f, v->field_types[i]);
                }
            } else if constexpr (SameAs<T, type::Vector>) {
                append_custom_type(f, cdtype);
            } else {
                static_assert(!SameAs<T, T>, "missing type option case");
            }
        });
    }

    static void patch_be_s32(Frame& f, U64 offset, S32 v) {
        f.body.ptr[offset + 0] = U8(U32(v) >> 24);
        f.body.ptr[offset + 1] = U8(U32(v) >> 16);
        f.body.ptr[offset + 2] = U8(U32(v) >> 8);
        f.body.ptr[offset + 3] = U8(U32(v));
    }

    static void append_cql_value_body(Frame& f, const ColumnValue& value, const type::Type& cdtype);

    // [bytes]: [int] n + n bytes, or [int] -1 for null. The length is back-patched once the
    // body is written so nested values need no separate sizing pass.
    void append_cql_value(Frame& f, const ColumnValue& value, type::Type cdtype) {
        if (type_matches_tag<Null>(value)) {
            append_be_s32(f, -1);
            return;
        }
        U64 length_offset = f.body.length;
        append_be_s32(f, 0);
        append_cql_value_body(f, value, cdtype);
        patch_be_s32(f, length_offset, S32(f.body.length - length_offset - 4));
    }

    static void append_cql_basic_body(Frame& f, const type::Basic& dtype, const auto& v) {
        auto sink = [&f](const U8* data, U64 n) {
            append_bytes(f, data, n);
        };
        native::wire::write_native_basic_body(io::to_writer(sink), dtype, v);
    }

    // Vector elements carry no [int] length: fixed-width bodies are concatenated and
    // variable-width bodies are prefixed with an unsigned vint length.
    static void append_cql_vector_element(Frame& f, const ColumnValue& value, const type::Type& element_type) {
        assert_true(!type_matches_tag<Null>(value), "vector elements cannot be null");
        const bool fixed = type_matches_tag<type::Basic>(element_type.value) && basic_fixed_width(get<type::Basic>(element_type.value)) > 0;
        if (fixed) {
            append_cql_value_body(f, value, element_type);
            return;
        }
        Frame tmp{.body = {}, .conn = f.conn, .op = f.op, .stream = f.stream};
        append_cql_value_body(tmp, value, element_type);
        DynamicArray<U8> len_prefix;
        append_unsigned_vint(len_prefix, tmp.body.length);
        append_bytes(f, len_prefix.ptr, len_prefix.length);
        append_bytes(f, tmp.body.ptr, tmp.body.length);
    }

    static void append_cql_value_body(Frame& f, const ColumnValue& value, const type::Type& cdtype) {
        visit(value, [&](const auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (SameAs<T, Null>) {
                assert_true(false, "null has no value body; append_cql_value writes it as a -1 length");
            } else if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                append_cql_basic_body(f, get<type::Basic>(cdtype.value), v);
            } else if constexpr (IsDynamicMap<T>) {
                const auto& m = get<type::Map>(cdtype.value);
                append_be_s32(f, S32(length(v)));
                for (auto& it : v) {
                    append_cql_value(f, it.first.value, m.key);
                    append_cql_value(f, it.second.value, m.value);
                }
            } else if constexpr (IsDynamicSet<T>) {
                const auto& key_type = get<type::Set>(cdtype.value).key;
                append_be_s32(f, S32(length(v)));
                for (auto& e : v) {
                    append_cql_value(f, e.value, key_type);
                }
            } else if constexpr (IsDynamicArray<T>) {
                if (type_matches_tag<type::List>(cdtype.value)) {
                    const auto& element_type = get<type::List>(cdtype.value).element;
                    append_be_s32(f, S32(v.length));
                    for (const auto& e : v) {
                        append_cql_value(f, e.value, element_type);
                    }
                } else if (type_matches_tag<type::Vector>(cdtype.value)) {
                    const auto& element_type = get<type::Vector>(cdtype.value).element;
                    for (const auto& e : v) {
                        append_cql_vector_element(f, e.value, element_type);
                    }
                } else {
                    // Tuple / UDT: one [bytes] per field, no count. A UDT value may carry fewer
                    // fields than its type after ALTER TYPE ADD; trailing fields are then omitted.
                    const bool        is_tuple    = type_matches_tag<type::Tuple>(cdtype.value);
                    const type::Type* field_types = is_tuple ? get<type::Tuple>(cdtype.value).elements.ptr : get<type::UDT*>(cdtype.value)->field_types.ptr;
                    for (U64 i = 0; i < v.length; i++) {
                        append_cql_value(f, v[i].value, field_types[i]);
                    }
                }
            } else {
                static_assert(!SameAs<T, T>, "missing implementation for column value type");
            }
        });
    }

    void append_error_body(Frame& f, engine::ExecutionStatus status, String8 message, String8 keyspace, String8 table) {
        append_be_s32(f, S32(static_cast<U16>(status)));
        append_cql_string(f, message);
        // ALREADY_EXISTS (0x2400) carries two extra [string] fields: the keyspace, and the table
        // (empty for a keyspace-level conflict).
        if (status == engine::ExecutionStatus::AlreadyExists) {
            append_cql_string(f, keyspace);
            append_cql_string(f, table);
        }
    }

    static void append_raw_error(Frame& f, S32 code, String8 message) {
        append_be_s32(f, code);
        append_cql_string(f, message);
    }

    // UNPREPARED (0x2500): [int code][string message][short bytes id]. Distinct from a generic
    // Invalid so the driver re-prepares the statement (keyed on the returned id) and retries.
    void append_unprepared_error(Frame& f, const U8* id, U16 id_len) {
        append_be_s32(f, 0x2500);
        append_cql_string(f, "Prepared statement not found");
        append_cql_short_bytes(f, id, id_len);
    }

    void append_result_void(Frame& f) {
        append_be_s32(f, result_codes::VOID);
    }

    void append_result_set_keyspace(Frame& f, String8 keyspace) {
        append_be_s32(f, result_codes::SET_KEYSPACE);
        append_cql_string(f, keyspace);
    }

    void append_schema_change_body(Frame& f, const engine::ExecutionResult& result) {
        String8 change = result.message.length ? result.message : String8("UPDATED");
        append_cql_string(f, change);
        append_cql_string(f, engine::to_str(result.schema_target));
        append_cql_string(f, String8(result.keyspace));
        if (result.schema_target != engine::SchemaChangeTarget::Keyspace) {
            append_cql_string(f, String8(result.table));
        }
    }

    coroutine::Task<S32> append_result_rows(Frame& f, engine::ExecutionResult& result, schema::Table* tbl) {
        append_be_s32(f, result_codes::ROWS);

        bool              has_select = result.select_col_indices.length > 0;
        DynamicArray<U64> effective_cols;
        if (has_select) {
            for (U64 ci : result.select_col_indices) {
                push_back(effective_cols, ci);
            }
        } else {
            for (U64 ci : tbl->select_star_col_indices) {
                push_back(effective_cols, ci);
            }
        }
        U64 col_count = effective_cols.length;

        append_be_s32(f, 0x0001); // Global_tables_spec flag
        append_be_s32(f, S32(col_count));
        append_cql_string(f, result.keyspace);
        append_cql_string(f, result.table);

        for (U64 i = 0; i < col_count; i++) {
            U64     ci    = effective_cols[i];
            String8 label = tbl->cols[ci].name;
            if (has_select && i < result.select_col_aliases.length && result.select_col_aliases[i].has_value()) {
                const auto& alias = *result.select_col_aliases[i];
                label             = String8(alias.c_str, alias.length);
            }
            append_cql_string(f, label);
            append_type_codes_option(f, tbl->cols[ci].type);
        }

        U64 count_pos = f.body.length;
        append_be_s32(f, 0);

        S32 row_count = 0;
        if (result.rows.has_value()) {
            auto&        rows       = *result.rows;
            U64          row_limit  = result.row_limit_count;
            RowIterator& row_it     = rows.start;
            RowIterator& row_end    = rows.stop;
            const bool   has_filter = result.filter_predicates.length > 0;

            S64 now_unix_ms = S64(os::unix_ms_now());
            while (row_it != row_end && U64(row_count) < row_limit) {
                ColumnRange col_range = co_await row_it.deref();

                if (io::row_is_expired(col_range.start.metadata, now_unix_ms)) {
                    if (result.is_distinct) {
                        co_await row_it.advance_partition();
                    } else {
                        co_await row_it.advance(row_end);
                    }
                    continue;
                }

                // Materialize all columns to support arbitrary output ordering and filter evaluation.
                DynamicArray<ColumnValue> row_values;
                while (col_range.start != col_range.stop && row_values.length < tbl->cols.length) {
                    push_back(row_values, co_await col_range.start.deref());
                    co_await col_range.start.advance();
                }
                while (row_values.length < tbl->cols.length) {
                    push_back(row_values, ColumnValue{Null{}});
                }

                bool pass = true;
                if (has_filter) {
                    EvalContext row_ctx = result.filter_ctx;
                    row_ctx.table       = result.resolved_table;
                    row_ctx.row_values  = row_values.ptr;
                    pass                = evaluate_where(result.filter_predicates, row_ctx);
                }

                if (pass) {
                    for (U64 i = 0; i < col_count; i++) {
                        U64 ci2 = effective_cols[i];
                        append_cql_value(f, row_values.ptr[ci2], tbl->cols[ci2].type);
                    }
                    row_count++;
                }

                if (result.is_distinct) {
                    co_await row_it.advance_partition();
                } else {
                    co_await row_it.advance(row_end);
                }
            }
        }

        patch_be_s32(f, count_pos, row_count);
        co_return row_count;
    }

    void append_result_virtual_rows(Frame& f, engine::VirtualRows& vr) {
        append_be_s32(f, result_codes::ROWS);

        append_be_s32(f, 0x0001); // Global_tables_spec flag
        append_be_s32(f, S32(vr.columns.length));
        append_cql_string(f, vr.keyspace);
        append_cql_string(f, vr.table);

        for (U64 ci = 0; ci < vr.columns.length; ci++) {
            append_cql_string(f, vr.columns[ci].name);
            append_type_codes_option(f, vr.columns[ci].type);
        }

        append_be_s32(f, S32(vr.rows.length));

        for (U64 ri = 0; ri < vr.rows.length; ri++) {
            for (U64 ci = 0; ci < vr.columns.length; ci++) {
                append_cql_value(f, vr.rows[ri].values[ci], vr.columns[ci].type);
            }
        }
    }

    static void append_id_bytes(Frame& f, U64 id) {
        U8 id_bytes[8];
        for (int i = 7; i >= 0; i--) {
            id_bytes[i] = U8(id);
            id >>= 8;
        }
        append_cql_short_bytes(f, id_bytes, 8);
    }

    static void append_result_prepared(Frame& f, U64 id, const engine::PreparedEntry& entry) {
        const U8 version = f.conn->version;
        append_be_s32(f, result_codes::PREPARED);
        append_id_bytes(f, id);

        if (version >= 5) {
            // result_metadata_id: only the table identity, since result metadata is never
            // sent with the PREPARED response (No_metadata below).
            XXHash64 h(0xCA550DA7_u64);
            h.add(entry.keyspace.c_str, entry.keyspace.length);
            h.add(entry.table.c_str, entry.table.length);
            append_id_bytes(f, h.hash());
        }

        append_be_s32(f, 0x0001); // Global_tables_spec
        append_be_s32(f, S32(entry.bind_variables.length));
        if (version >= 4) {
            append_be_s32(f, S32(entry.pk_indices.length));
            for (U16 idx : entry.pk_indices) {
                append_be_u16(f, idx);
            }
        }

        append_cql_string(f, String8(entry.keyspace));
        append_cql_string(f, String8(entry.table));

        for (U64 i = 0; i < entry.bind_variables.length; i++) {
            append_cql_string(f, String8(entry.bind_variables[i].name));
            append_type_codes_option(f, entry.bind_variables[i].type);
        }

        append_be_s32(f, 0x0004); // result metadata: No_metadata
        append_be_s32(f, 0);
    }

    static coroutine::Task<> send_error(ConnectionState& c, S16 stream, S32 code, String8 message) {
        Frame frame{.body = {}, .conn = &c, .op = op_codes::ERROR, .stream = stream};
        append_raw_error(frame, code, message);
        co_await send_frame(frame);
    }

    static coroutine::Task<> send_status_error(ConnectionState& c, S16 stream, engine::ExecutionStatus status, String8 message) {
        Frame frame{.body = {}, .conn = &c, .op = op_codes::ERROR, .stream = stream};
        append_error_body(frame, status, message);
        co_await send_frame(frame);
    }

    // Reports a Reader failure: framing problems as PROTOCOL_ERROR, bad values as INVALID.
    static coroutine::Task<> send_reader_error(ConnectionState& c, S16 stream, const Reader& r) {
        if (r.protocol_error.length) {
            co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, r.protocol_error);
        } else {
            co_await send_status_error(c, stream, engine::ExecutionStatus::Invalid, r.invalid_error);
        }
    }

    // ========================================================================
    // events
    // ========================================================================
    static coroutine::Task<> broadcast_schema_change(Server& server, const engine::ExecutionResult& result) {
        DynamicArray<U64> ids;
        for (ConnectionState* c : server.connections) {
            if (c->events & event_types::SCHEMA_CHANGE) {
                push_back(ids, c->id);
            }
        }
        for (U64 id : ids) {
            // @note re-resolved by id: a connection may close while an earlier send is suspended
            ConnectionState* target = nullptr;
            for (ConnectionState* c : server.connections) {
                if (c->id == id) {
                    target = c;
                    break;
                }
            }
            if (target == nullptr || target->write_failed) {
                continue;
            }
            Frame frame{.body = {}, .conn = target, .op = op_codes::EVENT, .stream = -1};
            append_cql_string(frame, "SCHEMA_CHANGE");
            append_schema_change_body(frame, result);
            co_await send_frame(frame);
        }
    }

    // ========================================================================
    // results
    // ========================================================================
    static coroutine::Task<> send_execution_result(ConnectionState& c, S16 stream, engine::ExecutionResult& result) {
        Engine& engine = *c.server->engine;
        if (result.status != engine::ExecutionStatus::Success) {
            Frame   frame{.body = {}, .conn = &c, .op = op_codes::ERROR, .stream = stream};
            String8 msg = result.message.length ? result.message : engine::to_str(result.status);
            append_error_body(frame, result.status, msg, String8(result.keyspace), String8(result.table));
            co_await send_frame(frame);
            co_return;
        }

        switch (result.kind) {
            case engine::ResultKind::Void: {
                Frame frame{.body = {}, .conn = &c, .op = op_codes::RESULT, .stream = stream};
                append_result_void(frame);
                co_await send_frame(frame);
            } break;
            case engine::ResultKind::SchemaChange: {
                Frame frame{.body = {}, .conn = &c, .op = op_codes::RESULT, .stream = stream};
                append_be_s32(frame, result_codes::SCHEMA_CHANGE);
                append_schema_change_body(frame, result);
                co_await send_frame(frame);
                co_await broadcast_schema_change(*c.server, result);
            } break;
            case engine::ResultKind::UseKeyspace: {
                Frame frame{.body = {}, .conn = &c, .op = op_codes::RESULT, .stream = stream};
                append_result_set_keyspace(frame, result.keyspace);
                co_await send_frame(frame);
            } break;
            case engine::ResultKind::Rows: {
                auto ks = schema::read_keyspace(engine.schema, result.keyspace).value;
                assert_true(ks != nullptr, "keyspace not found for rows result");
                auto tbl = schema::read_table(*ks, result.table).value;
                assert_true(tbl != nullptr, "table not found for rows result");
                Frame frame{.body = {}, .conn = &c, .op = op_codes::RESULT, .stream = stream};
                S32   row_count = co_await append_result_rows(frame, result, tbl);
                if (result.deferred_tx.started_transaction) {
                    co_await result.deferred_tx.commit();
                }
                co_await send_frame(frame);
                cql::log::db_response_returned_rows(row_count);
            } break;
            case engine::ResultKind::VirtualRows: {
                assert_true(result.virtual_rows.has_value(), "virtual rows missing");
                Frame frame{.body = {}, .conn = &c, .op = op_codes::RESULT, .stream = stream};
                append_result_virtual_rows(frame, *result.virtual_rows);
                co_await send_frame(frame);
                cql::log::db_response_returned_rows(result.virtual_rows->rows.length);
            } break;
        }
    }

    // Executes with `keyspace` as the statement's current keyspace; a successful USE updates
    // the connection's keyspace.
    static coroutine::Task<engine::ExecutionResult> execute_in_keyspace(ConnectionState& c, Statement& stmt, DynamicArray<Term>&& values, String8 keyspace) {
        AutoString8 ks{keyspace};
        auto        result = co_await engine::execute(*c.server->engine, stmt, move(values), ks);
        if (result.status == engine::ExecutionStatus::Success && result.kind == engine::ResultKind::UseKeyspace) {
            c.keyspace = ks;
        }
        co_return result;
    }

    static U64 prepared_id_from_bytes(TArrayView<const U8> id) {
        if (id.length != 8) {
            return 0;
        }
        U64 v = 0;
        for (U64 i = 0; i < 8; i++) {
            v = (v << 8) | U64(id.ptr[i]);
        }
        return v;
    }

    // ========================================================================
    // message handlers
    // ========================================================================
    static void append_supported(Frame& f) {
        String8 cql_versions[]      = {"3.4.7"};
        String8 compressions[]      = {"lz4"};
        String8 protocol_versions[] = {"3/v3", "4/v4", "5/v5"};
        append_be_u16(f, 3);
        append_cql_string(f, "CQL_VERSION");
        append_string_list(f, cql_versions, 1);
        append_cql_string(f, "COMPRESSION");
        append_string_list(f, compressions, 1);
        append_cql_string(f, "PROTOCOL_VERSIONS");
        append_string_list(f, protocol_versions, 3);
    }

    static coroutine::Task<> handle_startup(ConnectionState& c, S16 stream, Reader& r) {
        bool    want_lz4 = false;
        String8 bad_compression{};
        String8 bad_cql_version{};
        U16     n = read_u16(r);
        for (U16 i = 0; i < n && reader_ok(r); i++) {
            String8 key = read_string(r);
            String8 val = read_string(r);
            if (key == "COMPRESSION") {
                if (val == "lz4") {
                    want_lz4 = true;
                } else if (val.length > 0) {
                    bad_compression = val;
                }
            } else if (key == "CQL_VERSION") {
                if (val.length < 2 || val.data[0] != '3' || val.data[1] != '.') {
                    bad_cql_version = val;
                }
            }
            // @note DRIVER_NAME, DRIVER_VERSION, NO_COMPACT, THROW_ON_OVERLOAD are accepted and ignored
        }
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }
        if (bad_compression.length) {
            co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Unknown compression algorithm: %s", (const char*)AutoString8(bad_compression).c_str)));
            co_return;
        }
        if (bad_cql_version.length) {
            co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Unsupported CQL_VERSION %s; supported versions are 3.x", (const char*)AutoString8(bad_cql_version).c_str)));
            co_return;
        }

        c.started       = true;
        c.compressed    = want_lz4;
        c.authenticated = c.server->config->credentials.length == 0;

        if (c.authenticated) {
            Frame frame{.body = {}, .conn = &c, .op = op_codes::READY, .stream = stream};
            co_await send_frame(frame);
        } else {
            Frame frame{.body = {}, .conn = &c, .op = op_codes::AUTHENTICATE, .stream = stream};
            append_cql_string(frame, "org.apache.cassandra.auth.PasswordAuthenticator");
            co_await send_frame(frame);
        }
        // @note v5 switches to outer framing right after the STARTUP response
        c.framed = c.version >= 5;
    }

    static bool constant_time_equals(String8 a, String8 b) {
        U8  diff = U8(a.length != b.length);
        U64 n    = max(a.length, b.length);
        for (U64 i = 0; i < n; i++) {
            U8 x = i < a.length ? U8(a.data[i]) : 0;
            U8 y = i < b.length ? U8(b.data[i]) : 0;
            diff |= U8(x ^ y);
        }
        return diff == 0;
    }

    // SASL PLAIN: [authzid] NUL authcid NUL passwd
    static coroutine::Task<> handle_auth_response(ConnectionState& c, S16 stream, Reader& r) {
        const U8* token = nullptr;
        S32       len   = read_bytes(r, token);
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }
        String8 parts[3] = {};
        U64     part     = 0;
        U64     start    = 0;
        bool    ok       = len >= 0;
        for (S32 i = 0; ok && i <= len; i++) {
            if (i == len || token[i] == 0) {
                if (part >= 3) {
                    ok = false;
                    break;
                }
                parts[part++] = String8(reinterpret_cast<const char*>(token) + start, U64(i) - start);
                start         = U64(i) + 1;
            }
        }
        if (!ok || part != 3) {
            co_await send_error(c, stream, error_codes::BAD_CREDENTIALS, "Malformed SASL PLAIN authentication token");
            co_return;
        }
        String8 username = parts[1];
        String8 password = parts[2];

        bool authenticated = false;
        for (const AuthCredential& cred : c.server->config->credentials) {
            if (String8(cred.username) == username) {
                authenticated = constant_time_equals(String8(cred.password), password);
                break;
            }
        }
        if (!authenticated) {
            co_await send_error(c, stream, error_codes::BAD_CREDENTIALS, String8(fmt("Provided username %s and/or password are incorrect", (const char*)AutoString8(username).c_str)));
            co_return;
        }
        c.authenticated = true;
        Frame frame{.body = {}, .conn = &c, .op = op_codes::AUTH_SUCCESS, .stream = stream};
        append_be_s32(frame, -1);
        co_await send_frame(frame);
    }

    static coroutine::Task<> handle_register(ConnectionState& c, S16 stream, Reader& r) {
        U16 n      = read_u16(r);
        U8  events = 0;
        for (U16 i = 0; i < n && reader_ok(r); i++) {
            String8 type = read_string(r);
            if (!reader_ok(r)) {
                break;
            }
            if (type == "TOPOLOGY_CHANGE") {
                events |= event_types::TOPOLOGY_CHANGE;
            } else if (type == "STATUS_CHANGE") {
                events |= event_types::STATUS_CHANGE;
            } else if (type == "SCHEMA_CHANGE") {
                events |= event_types::SCHEMA_CHANGE;
            } else {
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Invalid value '%s' for Event.Type", (const char*)AutoString8(type).c_str)));
                co_return;
            }
        }
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }
        // @note single node: TOPOLOGY_CHANGE and STATUS_CHANGE are accepted but never fire
        c.events |= events;
        Frame frame{.body = {}, .conn = &c, .op = op_codes::READY, .stream = stream};
        co_await send_frame(frame);
    }

    // Executes a statement whose bind values are still raw; the bind specs depend on the keyspace.
    static coroutine::Task<> run_statement(ConnectionState& c, S16 stream, Statement& stmt, Reader& r, const RawQueryParameters& raw, String8 keyspace, const DynamicArray<engine::BindVariableSpec>& specs) {
        DynamicArray<Term> values = decode_bound_values(r, raw, specs, c.version);
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }
        auto result = co_await execute_in_keyspace(c, stmt, move(values), keyspace);
        co_await send_execution_result(c, stream, result);
    }

    static coroutine::Task<> handle_query(ConnectionState& c, S16 stream, Reader& r) {
        Engine& engine = *c.server->engine;
        String8 query  = read_long_string(r);
        auto    raw    = read_raw_query_parameters(r, c.version);
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }
        if (raw.params.has_paging) {
            co_await send_status_error(c, stream, engine::ExecutionStatus::Invalid, "Paging state is not supported: this server never returns one");
            co_return;
        }

        auto pr = parsers::parse(query);
        if (!pr.statement) {
            String8 msg = pr.err.length ? String8(pr.err) : String8("Failed to parse CQL");
            co_await send_status_error(c, stream, engine::ExecutionStatus::SyntaxError, msg);
            co_return;
        }

        String8 keyspace = raw.params.keyspace ? *raw.params.keyspace : String8(c.keyspace);
        auto    binds    = engine::collect_bind_variables(engine, *pr.statement, keyspace);
        if (binds.error.length > 0) {
            co_await send_status_error(c, stream, engine::ExecutionStatus::Invalid, String8(binds.error));
            co_return;
        }
        co_await run_statement(c, stream, *pr.statement, r, raw, keyspace, binds.specs);
    }

    static coroutine::Task<> handle_prepare(ConnectionState& c, S16 stream, Reader& r) {
        Engine& engine   = *c.server->engine;
        String8 query    = read_long_string(r);
        String8 keyspace = String8(c.keyspace);
        if (c.version >= 5) {
            U32 prepare_flags = U32(read_s32(r));
            if (prepare_flags & 0x01u) {
                keyspace = read_string(r);
            }
        }
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }

        auto result = engine::prepare(engine, query, keyspace);
        if (result.status != engine::ExecutionStatus::Success) {
            String8 msg = result.message.length ? result.message : engine::to_str(result.status);
            co_await send_status_error(c, stream, result.status, msg);
            co_return;
        }

        Frame frame{.body = {}, .conn = &c, .op = op_codes::RESULT, .stream = stream};
        append_result_prepared(frame, result.id, *result.entry);
        co_await send_frame(frame);
    }

    static coroutine::Task<> handle_execute(ConnectionState& c, S16 stream, Reader& r) {
        Engine&              engine = *c.server->engine;
        TArrayView<const U8> id     = read_short_bytes(r);
        if (c.version >= 5) {
            (void)read_short_bytes(r); // result_metadata_id
        }
        auto raw = read_raw_query_parameters(r, c.version);
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }
        if (raw.params.has_paging) {
            co_await send_status_error(c, stream, engine::ExecutionStatus::Invalid, "Paging state is not supported: this server never returns one");
            co_return;
        }

        U64   prepared_id = prepared_id_from_bytes(id);
        auto* entry       = engine::try_get_prepared(engine, prepared_id);
        if (entry == nullptr) {
            Frame frame{.body = {}, .conn = &c, .op = op_codes::ERROR, .stream = stream};
            append_unprepared_error(frame, id.ptr, U16(id.length));
            co_await send_frame(frame);
            co_return;
        }

        DynamicArray<Term> values = decode_bound_values(r, raw, entry->bind_variables, c.version);
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }
        auto result = co_await engine::execute(engine, prepared_id, move(values), c.keyspace);
        co_await send_execution_result(c, stream, result);
    }

    static coroutine::Task<> handle_batch(ConnectionState& c, S16 stream, Reader& r) {
        Engine& engine = *c.server->engine;

        struct RawChild {
            U8                     kind = 0;
            String8                query;
            TArrayView<const U8>   id{};
            DynamicArray<RawValue> values;
        };

        U8 batch_type = read_u8(r);
        if (reader_ok(r) && batch_type > 2) {
            reader_fail_protocol(r, "Invalid BATCH type");
        }
        U16                    n = read_u16(r);
        DynamicArray<RawChild> children;
        for (U16 i = 0; i < n && reader_ok(r); i++) {
            RawChild child{};
            child.kind = read_u8(r);
            if (child.kind == 0) {
                child.query = read_long_string(r);
            } else if (child.kind == 1) {
                child.id = read_short_bytes(r);
            } else if (reader_ok(r)) {
                reader_fail_protocol(r, "Invalid BATCH child kind");
            }
            U16 n_values = read_u16(r);
            for (U16 vi = 0; vi < n_values && reader_ok(r); vi++) {
                push_back(child.values, RawValue{.name = {}, .at = r.p});
                skip_value(r);
            }
            push_back(children, move(child));
        }
        (void)read_u16(r); // consistency
        U32 flags = c.version >= 5 ? U32(read_s32(r)) : U32(read_u8(r));
        if (reader_ok(r) && (flags & query_flags::NAMES_FOR_VALUES)) {
            reader_fail_protocol(r, "Named values are not supported in BATCH");
        }
        if (flags & query_flags::SERIAL_CONSISTENCY) {
            (void)read_u16(r);
        }
        if (flags & query_flags::DEFAULT_TIMESTAMP) {
            (void)read_s64(r); // @note treated as arrival time, see engine USING TIMESTAMP handling
        }
        String8 keyspace = String8(c.keyspace);
        if (c.version >= 5) {
            if (flags & query_flags::KEYSPACE) {
                keyspace = read_string(r);
            }
            if (flags & query_flags::NOW_IN_SECONDS) {
                (void)read_s32(r);
            }
        }
        if (!reader_ok(r)) {
            co_await send_reader_error(c, stream, r);
            co_return;
        }

        Batch batch{};
        batch.kind = batch_type == 0 ? Batch::Kind::LOGGED
                   : batch_type == 1 ? Batch::Kind::UNLOGGED
                                     : Batch::Kind::COUNTER;

        for (RawChild& child : children) {
            Optional<Statement>                    stmt;
            DynamicArray<engine::BindVariableSpec> specs;
            if (child.kind == 0) {
                auto parsed = parsers::parse(child.query);
                if (!parsed.statement) {
                    String8 msg = parsed.err.length ? String8(parsed.err) : String8("Failed to parse CQL in BATCH child");
                    co_await send_status_error(c, stream, engine::ExecutionStatus::SyntaxError, msg);
                    co_return;
                }
                auto binds = engine::collect_bind_variables(engine, *parsed.statement, keyspace);
                if (binds.error.length > 0) {
                    co_await send_status_error(c, stream, engine::ExecutionStatus::Invalid, String8(binds.error));
                    co_return;
                }
                specs = move(binds.specs);
                engine::qualify_statement(*parsed.statement, keyspace);
                stmt = move(*parsed.statement);
            } else {
                auto* entry = engine::try_get_prepared(engine, prepared_id_from_bytes(child.id));
                if (entry == nullptr) {
                    Frame frame{.body = {}, .conn = &c, .op = op_codes::ERROR, .stream = stream};
                    append_unprepared_error(frame, child.id.ptr, U16(child.id.length));
                    co_await send_frame(frame);
                    co_return;
                }
                auto parsed = parsers::parse(String8(entry->query_string));
                if (!parsed.statement) {
                    co_await send_status_error(c, stream, engine::ExecutionStatus::ServerError, "Failed to re-parse prepared BATCH child");
                    co_return;
                }
                specs = entry->bind_variables;
                engine::qualify_statement(*parsed.statement, String8(entry->prepare_keyspace));
                stmt = move(*parsed.statement);
            }

            RawQueryParameters raw{};
            raw.has_values = true;
            raw.raw_values = move(child.values);
            DynamicArray<Term> values = decode_bound_values(r, raw, specs, c.version);
            if (!reader_ok(r)) {
                co_await send_reader_error(c, stream, r);
                co_return;
            }
            if (AutoString8 bind_error = engine::bind_values_to_statement(*stmt, values); bind_error.length > 0) {
                co_await send_status_error(c, stream, engine::ExecutionStatus::Invalid, String8(bind_error));
                co_return;
            }

            Batch::ModificationStatement mod;
            bool                         is_mutation = visit(stmt->value, [&](auto& s) -> bool {
                using ST = RemoveCVRef<decltype(s)>;
                if constexpr (SameAs<ST, Insert> || SameAs<ST, Update> || SameAs<ST, Delete>) {
                    mod.value = move(s);
                    return true;
                } else {
                    return false;
                }
            });
            if (!is_mutation) {
                co_await send_status_error(c, stream, engine::ExecutionStatus::Invalid, "BATCH children must be INSERT, UPDATE, or DELETE");
                co_return;
            }
            push_back(batch.statements, move(mod));
        }

        Statement stmt;
        stmt.value = move(batch);
        auto result = co_await execute_in_keyspace(c, stmt, DynamicArray<Term>{}, keyspace);
        co_await send_execution_result(c, stream, result);
    }

    // ========================================================================
    // envelope dispatch
    // ========================================================================
    // Returns false when the connection must be closed.
    static coroutine::Task<bool> handle_envelope(ConnectionState& c, const U8* header, const U8* body, U64 body_len) {
        ZoneScopedN("handle_envelope");
        const U8  version = header[0] & 0x7Fu;
        const U8  flags   = header[1];
        const S16 stream  = read_be_s16(header + 2);
        const U8  op      = header[4];
        S64       t0      = os::monotonic_us();

        log::native_info(fmt("RX frame: op=%02x(%s) version=%d stream=%d body_len=%llu", op, (const char*)op_codes_to_str(op), (int)version, (int)stream, (unsigned long long)body_len));

        if (header[0] & 0x80u) {
            co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "Received a response envelope; expected a request");
            co_return false;
        }

        if (!c.started) {
            if (version < MIN_PROTOCOL_VERSION || version > MAX_PROTOCOL_VERSION) {
                c.version = 4;
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Invalid or unsupported protocol version (%d); supported versions are (3/v3, 4/v4, 5/v5)", (int)version)));
                co_return false;
            }
            c.version = version;
            if (flags & envelope_flags::COMPRESSION) {
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "Compression is not negotiated before STARTUP");
                co_return false;
            }
        } else if (version != c.version) {
            co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Invalid message version. Got %d but previous messages on this connection had version %d", (int)version, (int)c.version)));
            co_return false;
        }

        DynamicArray<U8> decompressed{};
        if (flags & envelope_flags::COMPRESSION) {
            if (c.version >= 5 || !c.compressed) {
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "Envelope compression flag set but compression is not in use for this envelope format");
                co_return false;
            }
            // v3/v4 lz4 body: [int] uncompressed length, then an LZ4 block
            if (body_len < 4) {
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "Truncated compressed envelope body");
                co_return false;
            }
            S32 uncompressed = read_be_s32(body);
            if (uncompressed < 0 || U64(uncompressed) > MAX_ENVELOPE_BODY_BYTE_COUNT) {
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "Invalid uncompressed envelope length");
                co_return false;
            }
            resize(decompressed, U64(uncompressed));
            S32 got = uncompressed == 0 ? 0 : lz4::decompress(body + 4, decompressed.ptr, S32(body_len - 4), uncompressed);
            if (got != uncompressed) {
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "LZ4 decompression of the envelope body failed");
                co_return false;
            }
            body     = decompressed.ptr;
            body_len = decompressed.length;
        }

        Reader r{.p = body, .end = body + body_len};
        if ((flags & envelope_flags::CUSTOM_PAYLOAD) && c.version >= 4) {
            skip_bytes_map(r);
        }

        if (!c.started && op != op_codes::OPTIONS && op != op_codes::STARTUP) {
            co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Unexpected message %s, expecting STARTUP or OPTIONS", (const char*)op_codes_to_str(op))));
            co_return true;
        }
        if (c.started && !c.authenticated && op != op_codes::AUTH_RESPONSE && op != op_codes::OPTIONS) {
            co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Unexpected message %s, expecting SASL response", (const char*)op_codes_to_str(op))));
            co_return true;
        }

        switch (op) {
            case op_codes::OPTIONS: {
                Frame frame{.body = {}, .conn = &c, .op = op_codes::SUPPORTED, .stream = stream};
                append_supported(frame);
                co_await send_frame(frame);
            } break;
            case op_codes::STARTUP: {
                if (c.started) {
                    co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "STARTUP sent after the connection was already started");
                    break;
                }
                co_await handle_startup(c, stream, r);
            } break;
            case op_codes::AUTH_RESPONSE: {
                if (c.authenticated) {
                    co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, "Unexpected AUTH_RESPONSE: the connection is already authenticated");
                    break;
                }
                co_await handle_auth_response(c, stream, r);
            } break;
            case op_codes::REGISTER:
                co_await handle_register(c, stream, r);
                break;
            case op_codes::QUERY:
                co_await handle_query(c, stream, r);
                cql::log::db_operation_duration(os::monotonic_us() - t0);
                break;
            case op_codes::PREPARE:
                co_await handle_prepare(c, stream, r);
                cql::log::db_operation_duration(os::monotonic_us() - t0);
                break;
            case op_codes::EXECUTE:
                co_await handle_execute(c, stream, r);
                cql::log::db_operation_duration(os::monotonic_us() - t0);
                break;
            case op_codes::BATCH:
                co_await handle_batch(c, stream, r);
                cql::log::db_operation_duration(os::monotonic_us() - t0);
                break;
            default:
                co_await send_error(c, stream, error_codes::PROTOCOL_ERROR, String8(fmt("Unknown or unexpected opcode 0x%02x", (unsigned)op)));
                break;
        }
        co_return !c.write_failed;
    }

    // ========================================================================
    // connection loop
    // ========================================================================
    static void consume_front(DynamicArray<U8>& buf, U64 n) {
        U64 rem = buf.length - n;
        if (rem > 0) {
            os::memory_move(buf.ptr, buf.ptr + n, rem);
        }
        resize(buf, rem);
    }

    static coroutine::Task<bool> fill_to(ConnectionState& c, DynamicArray<U8>& buf, U64 n) {
        while (buf.length < n) {
            if (!co_await try_append_tcp_read(*c.req, buf)) {
                co_return false;
            }
        }
        co_return true;
    }

    // Validates an envelope header's body length and returns the total envelope size, or 0.
    static U64 envelope_size(const U8* header) {
        S32 body_len = read_be_s32(header + 5);
        if (body_len < 0 || U64(body_len) > MAX_ENVELOPE_BODY_BYTE_COUNT) {
            return 0;
        }
        return V4_FRAME_HEADER_BYTE_COUNT + U64(body_len);
    }

    // Reads one v5 outer frame and appends its (decompressed) payload to `payload`.
    static coroutine::Task<bool> read_v5_frame(ConnectionState& c, DynamicArray<U8>& buf, DynamicArray<U8>& payload, bool& self_contained) {
        const U64 hdr_len = c.compressed ? V5_COMP_HDR_BYTE_COUNT : V5_UNCOMP_HDR_BYTE_COUNT;
        if (!co_await fill_to(c, buf, hdr_len + V5_HDR_CRC24_BYTE_COUNT)) {
            co_return false;
        }
        U32 hdr_crc_computed = crc::crc24(buf.ptr, hdr_len);
        U32 hdr_crc_stored   = U32(buf[hdr_len]) | (U32(buf[hdr_len + 1]) << 8) | (U32(buf[hdr_len + 2]) << 16);
        if (hdr_crc_computed != hdr_crc_stored) {
            log::native_error("v5 frame header CRC24 mismatch, dropping connection");
            co_return false;
        }

        U32 payload_len = U32(buf[0]) | (U32(buf[1]) << 8) | ((U32(buf[2]) & 0x01u) << 16);
        U32 uncomp_len  = 0;
        if (c.compressed) {
            uncomp_len     = ((U32(buf[2]) >> 1) & 0x7Fu) | (U32(buf[3]) << 7) | ((U32(buf[4]) & 0x03u) << 15);
            self_contained = ((buf[4] >> 2) & 1u) != 0;
        } else {
            self_contained = ((buf[2] >> 1) & 1u) != 0;
        }
        consume_front(buf, hdr_len + V5_HDR_CRC24_BYTE_COUNT);

        if (!co_await fill_to(c, buf, payload_len + V5_PAYLOAD_CRC32_BYTE_COUNT)) {
            co_return false;
        }
        U32 payload_crc_computed = crc::crc32(buf.ptr, payload_len, crc::CRC32_CQL_V5_INIT);
        U32 payload_crc_stored   = U32(buf[payload_len]) | (U32(buf[payload_len + 1]) << 8) | (U32(buf[payload_len + 2]) << 16) | (U32(buf[payload_len + 3]) << 24);
        if (payload_crc_computed != payload_crc_stored) {
            log::native_error("v5 frame payload CRC32 mismatch, dropping connection");
            co_return false;
        }

        U64 old_len = payload.length;
        if (c.compressed && uncomp_len > 0) {
            if (payload.length + uncomp_len > MAX_ENVELOPE_BODY_BYTE_COUNT + V4_FRAME_HEADER_BYTE_COUNT) {
                co_return false;
            }
            resize(payload, old_len + uncomp_len);
            S32 got = lz4::decompress(buf.ptr, payload.ptr + old_len, S32(payload_len), S32(uncomp_len));
            if (got != S32(uncomp_len)) {
                log::native_error("v5 frame LZ4 decompression failed, dropping connection");
                co_return false;
            }
        } else {
            if (payload.length + payload_len > MAX_ENVELOPE_BODY_BYTE_COUNT + V4_FRAME_HEADER_BYTE_COUNT) {
                co_return false;
            }
            resize(payload, old_len + payload_len);
            os::memory_copy(payload.ptr + old_len, buf.ptr, payload_len);
        }
        consume_front(buf, payload_len + V5_PAYLOAD_CRC32_BYTE_COUNT);
        co_return true;
    }

    static coroutine::Task<> connection_loop(ConnectionState& c) {
        DynamicArray<U8> buf{};
        DynamicArray<U8> payload{};
        DynamicArray<U8> large{}; // a v5 envelope spread over non-self-contained frames

        while (!c.write_failed) {
            if (!c.framed) {
                if (!co_await fill_to(c, buf, 1)) {
                    co_return;
                }
                // @note v1/v2 headers are 8 bytes; answer before trying to parse a v3+ header
                if (!c.started && (buf[0] & 0x7Fu) < MIN_PROTOCOL_VERSION) {
                    c.version = 4;
                    co_await send_error(c, 0, error_codes::PROTOCOL_ERROR, String8(fmt("Invalid or unsupported protocol version (%d); supported versions are (3/v3, 4/v4, 5/v5)", (int)(buf[0] & 0x7Fu))));
                    co_return;
                }
                if (!co_await fill_to(c, buf, V4_FRAME_HEADER_BYTE_COUNT)) {
                    co_return;
                }
                U64 size = envelope_size(buf.ptr);
                if (size == 0) {
                    co_await send_error(c, read_be_s16(buf.ptr + 2), error_codes::PROTOCOL_ERROR, "Invalid envelope body length");
                    co_return;
                }
                if (!co_await fill_to(c, buf, size)) {
                    co_return;
                }
                DynamicArray<U8> env{};
                resize(env, size);
                os::memory_copy(env.ptr, buf.ptr, size);
                consume_front(buf, size);
                if (!co_await handle_envelope(c, env.ptr, env.ptr + V4_FRAME_HEADER_BYTE_COUNT, size - V4_FRAME_HEADER_BYTE_COUNT)) {
                    co_return;
                }
                continue;
            }

            payload.length      = 0;
            bool self_contained = false;
            if (!co_await read_v5_frame(c, buf, payload, self_contained)) {
                co_return;
            }

            if (self_contained) {
                if (large.length > 0) {
                    co_await send_error(c, 0, error_codes::PROTOCOL_ERROR, "Self-contained frame received while a large envelope was incomplete");
                    co_return;
                }
                const U8* p   = payload.ptr;
                const U8* end = payload.ptr + payload.length;
                while (p < end) {
                    if (U64(end - p) < V4_FRAME_HEADER_BYTE_COUNT) {
                        co_await send_error(c, 0, error_codes::PROTOCOL_ERROR, "Truncated envelope in self-contained frame");
                        co_return;
                    }
                    U64 size = envelope_size(p);
                    if (size == 0 || U64(end - p) < size) {
                        co_await send_error(c, read_be_s16(p + 2), error_codes::PROTOCOL_ERROR, "Envelope does not fit its self-contained frame");
                        co_return;
                    }
                    if (!co_await handle_envelope(c, p, p + V4_FRAME_HEADER_BYTE_COUNT, size - V4_FRAME_HEADER_BYTE_COUNT)) {
                        co_return;
                    }
                    p += size;
                }
                continue;
            }

            U64 old = large.length;
            resize(large, old + payload.length);
            os::memory_copy(large.ptr + old, payload.ptr, payload.length);
            if (large.length < V4_FRAME_HEADER_BYTE_COUNT) {
                continue;
            }
            U64 size = envelope_size(large.ptr);
            if (size == 0 || large.length > size) {
                co_await send_error(c, 0, error_codes::PROTOCOL_ERROR, "Invalid large envelope framing");
                co_return;
            }
            if (large.length == size) {
                bool keep = co_await handle_envelope(c, large.ptr, large.ptr + V4_FRAME_HEADER_BYTE_COUNT, size - V4_FRAME_HEADER_BYTE_COUNT);
                large.length = 0;
                if (!keep) {
                    co_return;
                }
            }
        }
    }

    coroutine::Task<void> handle_connection(Server& server, const tcp::Request& req) {
        ConnectionState c{};
        c.id     = server.next_connection_id++;
        c.req    = &req;
        c.server = &server;
        push_back(server.connections, &c);

        co_await connection_loop(c);

        for (U64 i = 0; i < server.connections.length; i++) {
            if (server.connections[i] == &c) {
                server.connections[i] = server.connections[server.connections.length - 1];
                pop_back(server.connections);
                break;
            }
        }
        // @note waits out any event send another connection has in flight on this socket
        co_await lock(c.write_lock);
        unlock(c.write_lock);
    }
}
