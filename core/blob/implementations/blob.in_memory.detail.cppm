module;
#include <coroutine>

export module plexdb.blob.in_memory.detail;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.coroutine;
import plexdb.blob.in_memory;

using namespace plexdb;

export namespace plexdb::blob {
    coroutine::Task<TArrayView<const U8>> rblock(BlobInMemory& blob, U64 offset) {
        co_return TArrayView<const U8>{blob.data.ptr + offset, blob.size_bytes - offset};
    }

    coroutine::Task<TArrayView<U8>> rwblock(BlobInMemory& blob, U64 offset) {
        co_return TArrayView<U8>{blob.data.ptr + offset, blob.size_bytes - offset};
    }

    coroutine::Task<void> resize_impl(BlobInMemory& blob, U64 new_size) {
        resize(blob.data, new_size);
        blob.size_bytes = new_size;
        co_return;
    }

    coroutine::Task<void> remove_impl(BlobInMemory& blob) {
        clear(blob.data);
        blob.size_bytes = 0;
        co_return;
    }
}
