module;
#include <coroutine>
#include <plexdb/macros/macros.h>

export module plexdb.coroutine.extra;

import plexdb.base;
import plexdb.coroutine.base;

export namespace plexdb::coroutine {
    template<typename F, typename T, typename Length = size_t>
    concept TArrayFlushFunction = requires(F f, TArrayView<T,Length>& buffer, U64 length) {
        {f(buffer, length)} -> SameAs<Task<>>;
    };

    // @note warning, need manual flush before destructor
    template<typename F, typename T, typename Length = size_t>
        requires TArrayFlushFunction<F,T,Length>
    struct FlushableTArray {
        TArrayView<T,Length> buffer;
        U64 length;
        F flush;

        FlushableTArray(TArrayView<T,Length> b, F f) : buffer(b), length(0), flush(f) {}
        ~FlushableTArray() { assert_true(this->length == 0, "coroutine::FlushableTArray needs to be manually flushed before destructor, this should never happen!"); }
    };

    template<typename F, typename T, typename Length = size_t>
        requires TArrayFlushFunction<F,T,Length>
    Task<> flush_if_needed(FlushableTArray<F,T,Length>& arr) {
        assert_true(arr.length <= arr.buffer.length, "invalid flush state, length overflows buffer");

        if (arr.length == arr.buffer.length) {
            co_await arr.flush(arr.buffer, arr.length);
            arr.length = 0;
        }

        co_return;
    }

    template<typename F, typename T, typename Length = size_t>
        requires TArrayFlushFunction<F, T, Length>
    Task<> append(FlushableTArray<F,T,Length>& arr, const TArrayView<T,Length>& postfix) {
        const T* src = postfix.ptr;
        const T* end = postfix.ptr + postfix.length;

        while (src != end) {
            co_await flush_if_needed(arr);

            U64 count = min(static_cast<U64>(end - src), arr.buffer.length - arr.length);
            assert_true(count > 0, "string8 buffer is zero length");

            // @todo memory copy
            for (U64 i = 0; i < count; i++) {
                arr.buffer.ptr[arr.length++] = src[i];
            }

            src += count;
        }
    }

    template<typename F, typename T, typename Length = size_t>
        requires TArrayFlushFunction<F,T,Length>
    Task<> cleanup(FlushableTArray<F,T,Length>& arr) {
        co_await arr.flush(arr.buffer, arr.length);
        arr.length = 0;
    }

}
