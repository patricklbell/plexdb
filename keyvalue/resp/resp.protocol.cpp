module keyvalue.resp.protocol;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.os;
import plexdb.tagged_union;

using namespace plexdb;

namespace keyvalue::resp::protocol {
    static void append_bytes(DynamicArray<U8>& buf, const U8* data, U64 n) {
        U64 old = buf.length;
        resize(buf, old + n);
        os::memory_copy(buf.ptr + old, data, n);
    }
    static void append_str(DynamicArray<U8>& buf, String8 s) {
        append_bytes(buf, reinterpret_cast<const U8*>(s.data), s.length);
    }
    static void append_crlf(DynamicArray<U8>& buf) {
        U64 old = buf.length;
        resize(buf, old + 2);
        buf[old] = '\r'; buf[old + 1] = '\n';
    }
    static void append_decimal(DynamicArray<U8>& buf, S64 value) {
        AutoString8 s = to_str(value);
        append_str(buf, s);
    }

    void append_simple_string(DynamicArray<U8>& buf, String8 value) {
        U64 old = buf.length;
        resize(buf, old + 1 + value.length + 2);
        buf[old] = '+';
        os::memory_copy(buf.ptr + old + 1, value.data, value.length);
        buf[old + 1 + value.length]     = '\r';
        buf[old + 1 + value.length + 1] = '\n';
    }
    void append_error(DynamicArray<U8>& buf, String8 kind, String8 message) {
        push_back(buf, U8('-'));
        append_str(buf, kind);
        push_back(buf, U8(' '));
        append_str(buf, message);
        append_crlf(buf);
    }
    void append_integer(DynamicArray<U8>& buf, S64 value) {
        push_back(buf, U8(':'));
        append_decimal(buf, value);
        append_crlf(buf);
    }
    void append_bulk_string(DynamicArray<U8>& buf, String8 value) {
        push_back(buf, U8('$'));
        append_decimal(buf, S64(value.length));
        append_crlf(buf);
        append_str(buf, value);
        append_crlf(buf);
    }
    void append_null_bulk_string(DynamicArray<U8>& buf) { append_str(buf, "$-1\r\n"); }
    void append_array_header(DynamicArray<U8>& buf, S64 count) {
        push_back(buf, U8('*'));
        append_decimal(buf, count);
        append_crlf(buf);
    }
    void append_null_array(DynamicArray<U8>& buf) { append_str(buf, "*-1\r\n"); }

    bool encode_result(const engine::ExecutionResult& result, DynamicArray<U8>& buf) {
        return visit(result, [&](const auto& r) -> bool {
            using T = RemoveCVRef<decltype(r)>;
            if constexpr (SameAs<T, engine::ResultSimpleStr>) {
                append_simple_string(buf, r.value);
                return true;
            } else if constexpr (SameAs<T, engine::ResultNull>) {
                append_null_bulk_string(buf);
                return true;
            } else if constexpr (SameAs<T, engine::ResultEmptyArr>) {
                append_array_header(buf, 0);
                return true;
            } else if constexpr (SameAs<T, engine::ResultBulkStr>) {
                append_bulk_string(buf, r.value);
                return true;
            } else if constexpr (SameAs<T, engine::ResultInt>) {
                append_integer(buf, r.value);
                return true;
            } else if constexpr (SameAs<T, engine::ResultBulkArr>) {
                append_array_header(buf, S64(r.values.length));
                for (U64 i = 0; i < r.values.length; i++)
                    append_bulk_string(buf, r.values[i]);
                return true;
            } else if constexpr (SameAs<T, engine::ResultNullBulkArr>) {
                append_array_header(buf, S64(r.values.length));
                for (U64 i = 0; i < r.values.length; i++) {
                    if (r.values[i]) append_bulk_string(buf, *r.values[i]);
                    else             append_null_bulk_string(buf);
                }
                return true;
            } else if constexpr (SameAs<T, engine::ResultScan>) {
                append_array_header(buf, 2);
                append_bulk_string(buf, r.cursor);
                append_array_header(buf, S64(r.keys.length));
                for (U64 i = 0; i < r.keys.length; i++)
                    append_bulk_string(buf, r.keys[i]);
                return true;
            } else if constexpr (SameAs<T, engine::ResultError>) {
                append_error(buf, "ERR", r.message);
                return true;
            } else if constexpr (SameAs<T, engine::ResultClose>) {
                append_simple_string(buf, "OK");
                return false;
            } else {
                static_assert(!SameAs<T,T>, "missing result case");
            }
        });
    }
}
