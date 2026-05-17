module keyvalue.resp.protocol;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.os;

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
        if (result.status == engine::ExecutionStatus::Close) {
            append_simple_string(buf, "OK");
            return false;
        }
        if (result.status == engine::ExecutionStatus::Error) {
            append_error(buf, "ERR", result.error_message);
            return true;
        }
        if (result.status == engine::ExecutionStatus::NotFound) {
            append_null_bulk_string(buf);
            return true;
        }
        switch (result.kind) {
            case engine::ResultKind::None:
                break;
            case engine::ResultKind::SimpleStr:
                append_simple_string(buf, result.str);
                break;
            case engine::ResultKind::BulkStr:
                append_bulk_string(buf, result.str);
                break;
            case engine::ResultKind::Int:
                append_integer(buf, result.integer);
                break;
            case engine::ResultKind::Arr:
                append_array_header(buf, S64(result.arr.length));
                for (U64 i = 0; i < result.arr.length; i++)
                    append_bulk_string(buf, result.arr[i]);
                break;
            case engine::ResultKind::NullArr:
                append_array_header(buf, S64(result.null_arr.length));
                for (U64 i = 0; i < result.null_arr.length; i++) {
                    if (result.null_arr[i]) append_bulk_string(buf, *result.null_arr[i]);
                    else                    append_null_bulk_string(buf);
                }
                break;
            case engine::ResultKind::Scan:
                append_array_header(buf, 2);
                append_bulk_string(buf, result.cursor);
                append_array_header(buf, S64(result.arr.length));
                for (U64 i = 0; i < result.arr.length; i++)
                    append_bulk_string(buf, result.arr[i]);
                break;
        }
        return true;
    }
}
