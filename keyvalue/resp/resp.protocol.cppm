export module keyvalue.resp.protocol;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.os;
import plexdb.tagged_union;

import keyvalue.engine;

using namespace plexdb;

export namespace keyvalue::resp::protocol {
    void append_simple_string   (DynamicArray<U8>& buf, String8 value);
    void append_error           (DynamicArray<U8>& buf, String8 kind, String8 message);
    void append_integer         (DynamicArray<U8>& buf, S64 value);
    void append_bulk_string     (DynamicArray<U8>& buf, String8 value);
    void append_null_bulk_string(DynamicArray<U8>& buf);
    void append_array_header    (DynamicArray<U8>& buf, S64 count);
    void append_null_array      (DynamicArray<U8>& buf);
    bool encode_result          (const engine::ExecutionResult& result, DynamicArray<U8>& buf);
}
