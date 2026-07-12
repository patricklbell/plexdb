module;
#include <coroutine>

export module cql.engine.io.types;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.coroutine;

using namespace plexdb;

export namespace cql::io {
    template<typename F>
    concept Read = requires(F f, U8* src, U64 size) {
        { f(src, size) } -> SameAs<coroutine::Task<void>>;
    };

    template<typename F>
    concept Write = requires(F f, const U8* src, U64 size) {
        f(src, size);
    };

    // Constrains to_checker below to a callable indexable by column.
    template<typename F>
    concept ColumnIndexFn = requires(F f, U64 col_idx) {
        f(col_idx);
    };

    // Type-erased concrete IO handles — 16 bytes, pass by value
    using Reader              = plexdb::Functor<coroutine::Task<void>, U8*, U64>;
    using Writer              = plexdb::Functor<void, const U8*, U64>;
    using ColumnActiveChecker = plexdb::Functor<bool, U64>;

    // Bridge helpers: wrap a concept-satisfying callable into the concrete erased handle.
    // The callable must outlive the handle.
    template<Read F>
    Reader to_reader(F& f) {
        return plexdb::to_functor<coroutine::Task<void>, U8*, U64>(f);
    }
    template<Write F>
    Writer to_writer(F& f) {
        return plexdb::to_functor<void, const U8*, U64>(f);
    }
    // Sync in-memory Writer target: appends to buf. Caller keeps the returned
    // closure alive and feeds it through to_writer (the closure, not the Writer
    // handle, owns the buf reference).
    inline auto sync_buffer_writer(DynamicArray<U8>& buf) {
        return [&buf](const U8* in_value, U64 size) {
            U64 old_len = buf.length;
            resize(buf, old_len + size);
            os::memory_copy(buf.ptr + old_len, in_value, size);
        };
    }

    template<ColumnIndexFn F>
    ColumnActiveChecker to_checker(F& f) {
        return plexdb::to_functor<bool, U64>(f);
    }
}
