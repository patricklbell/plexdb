module;
#include <coroutine>

export module plexdb.blob.types;

import plexdb.base;
import plexdb.coroutine;

using namespace plexdb;

export namespace plexdb::blob {
    template<typename B>
    concept Blob = requires(B& b, U64 offset, U64 new_size) {
        b.size_bytes;
        { rblock(b, offset) }        -> SameAs<coroutine::Task<TArrayView<const U8>>>;
        { rwblock(b, offset) }       -> SameAs<coroutine::Task<TArrayView<U8>>>;
        { resize_impl(b, new_size) } -> SameAs<coroutine::Task<void>>;
        { remove_impl(b) }           -> SameAs<coroutine::Task<void>>;
    };
}
