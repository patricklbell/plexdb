#pragma once

// Minimal C++20 coroutine support using compiler builtins.
// Provides the types required by co_await / co_return without depending
// on the standard-library <coroutine> header.
//
// Supported compilers: Clang >= 8, GCC >= 11.
// Required builtins: __builtin_coro_resume, __builtin_coro_done,
//                    __builtin_coro_destroy, __builtin_coro_promise,
//                    __builtin_coro_noop.

namespace std {

// ---------------------------------------------------------------------------
// coroutine_traits  —  maps (ReturnType, ArgTypes...) -> promise_type
// ---------------------------------------------------------------------------
template<typename Ret, typename...>
struct coroutine_traits {
    using promise_type = typename Ret::promise_type;
};

// ---------------------------------------------------------------------------
// coroutine_handle<void>  —  type-erased handle
// ---------------------------------------------------------------------------
template<typename Promise = void>
struct coroutine_handle;

template<>
struct coroutine_handle<void> {
    void* _frame = nullptr;

    coroutine_handle() noexcept = default;
    coroutine_handle(decltype(nullptr)) noexcept {}

    static coroutine_handle from_address(void* addr) noexcept {
        coroutine_handle h;
        h._frame = addr;
        return h;
    }

    void* address()              const noexcept { return _frame; }
    explicit operator bool()     const noexcept { return _frame != nullptr; }
    bool   done()                const noexcept { return __builtin_coro_done(_frame); }
    void   operator()()          const          { resume(); }
    void   resume()              const          { __builtin_coro_resume(_frame); }
    void   destroy()             const          { __builtin_coro_destroy(_frame); }

    friend bool operator==(coroutine_handle a, coroutine_handle b) noexcept {
        return a._frame == b._frame;
    }
    friend bool operator!=(coroutine_handle a, coroutine_handle b) noexcept {
        return !(a == b);
    }
};

// ---------------------------------------------------------------------------
// coroutine_handle<Promise>  —  typed handle
// ---------------------------------------------------------------------------
template<typename Promise>
struct coroutine_handle : coroutine_handle<void> {
    using coroutine_handle<void>::coroutine_handle;

    static coroutine_handle from_address(void* addr) noexcept {
        coroutine_handle h;
        h._frame = addr;
        return h;
    }

    static coroutine_handle from_promise(Promise& p) noexcept {
        coroutine_handle h;
        h._frame = __builtin_coro_promise(&p, alignof(Promise), true);
        return h;
    }

    Promise& promise() const noexcept {
        return *static_cast<Promise*>(
            __builtin_coro_promise(_frame, alignof(Promise), false));
    }
};

// ---------------------------------------------------------------------------
// suspend_always / suspend_never
// ---------------------------------------------------------------------------
struct suspend_always {
    bool await_ready()                           noexcept { return false; }
    void await_suspend(coroutine_handle<>)       noexcept {}
    void await_resume()                          noexcept {}
};

struct suspend_never {
    bool await_ready()                           noexcept { return true; }
    void await_suspend(coroutine_handle<>)       noexcept {}
    void await_resume()                          noexcept {}
};

// ---------------------------------------------------------------------------
// noop_coroutine
// ---------------------------------------------------------------------------
struct noop_coroutine_promise {};

template<>
struct coroutine_handle<noop_coroutine_promise> {
    void* _frame = nullptr;

    coroutine_handle() noexcept : _frame(__builtin_coro_noop()) {}

    void*              address()    const noexcept { return _frame; }
    constexpr explicit operator bool() const noexcept { return true; }
    constexpr bool     done()       const noexcept { return false; }
    constexpr void     operator()() const noexcept {}
    constexpr void     resume()     const noexcept {}
    constexpr void     destroy()    const noexcept {}

    operator coroutine_handle<void>() const noexcept {
        return coroutine_handle<void>::from_address(_frame);
    }

    noop_coroutine_promise& promise() const noexcept {
        return *static_cast<noop_coroutine_promise*>(
            __builtin_coro_promise(_frame, alignof(noop_coroutine_promise), false));
    }

    friend bool operator==(coroutine_handle a, coroutine_handle b) noexcept {
        return a._frame == b._frame;
    }
};

using noop_coroutine_handle = coroutine_handle<noop_coroutine_promise>;

inline noop_coroutine_handle noop_coroutine() noexcept {
    return noop_coroutine_handle{};
}

} // namespace std
