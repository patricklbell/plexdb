module;
#include "macros.h"

module plexdb.threads;

namespace plexdb::threads {
    thread_static Context* thread_local_ctx = nullptr;
    
    void equip(Context* in_ctx) {
        thread_local_ctx = in_ctx;
    }

    Context* get_context() {
        return thread_local_ctx;
    }

    // ========================================================================
    // thread
    // ========================================================================
    Thread::~Thread() {
        if (!os::is_zero_handle(handle))
            os::thread_join(handle);
    }

    Thread::Thread(Thread&& other) noexcept : handle(other.handle) {
        other.handle = os::zero_handle();
    }

    Thread& Thread::operator=(Thread&& other) noexcept {
        if (this != &other) {
            if (!os::is_zero_handle(handle))
                os::thread_join(handle);
            handle = other.handle;
            other.handle = os::zero_handle();
        }
        return *this;
    }

    Thread::operator bool() const {
        return !os::is_zero_handle(handle);
    }

    static void functor_trampoline(void* raw) {
        auto* fn = static_cast<AutoFunctor<void()>*>(raw);
        (*fn)();
        fn->~AutoFunctor<void()>();
        os::deallocate(fn);
    }

    Thread launch(AutoFunctor<void()> fn, const char* name) {
        auto* heap_fn = reinterpret_cast<AutoFunctor<void()>*>(os::allocate(sizeof(AutoFunctor<void()>)));
        new (heap_fn) AutoFunctor<void()>(move(fn));
        Thread t{os::thread_launch(functor_trampoline, heap_fn)};
        if (name) os::thread_set_name(t.handle, name);
        return t;
    }

    // ========================================================================
    // semaphore
    // ========================================================================
    Semaphore::Semaphore(U32 initial_count) : handle(os::semaphore_open(initial_count)) {}

    Semaphore::~Semaphore() {
        if (!os::is_zero_handle(handle))
            os::semaphore_close(handle);
    }

    Semaphore::Semaphore(Semaphore&& other) noexcept : handle(other.handle) {
        other.handle = os::zero_handle();
    }

    Semaphore& Semaphore::operator=(Semaphore&& other) noexcept {
        if (this != &other) {
            if (!os::is_zero_handle(handle))
                os::semaphore_close(handle);
            handle = other.handle;
            other.handle = os::zero_handle();
        }
        return *this;
    }

    void Semaphore::signal() { os::semaphore_signal(handle); }
    void Semaphore::wait()   { os::semaphore_wait(handle); }

    // ========================================================================
    // scratch arena
    // ========================================================================
    Arena* get_scratch(TArrayView<Arena*,U64> conflicts) {
        Context* ctx = get_context();
        assert_true(ctx != nullptr, "cannot acquire scratch because thread context was not set");

        for (auto& arena : TArrayView(ctx->arenas)) {
            bool is_conflicting = false;
            for (const auto& potential_conflict : conflicts) {
                if (&arena == potential_conflict) {
                    is_conflicting = true;
                    break;
                }
            }
            if (!is_conflicting) {
                return &arena;
            }
        }

        assert_true(false, "too many scratch arenas used");
        return nullptr;
    }

    Scope::Scope(Arena* in_arena) : arena(in_arena), offset(arena::offset(*arena)) {}
    Scope::~Scope() {
        if (this->active)
            arena::pop_to(*this->arena, this->offset);
    }

    Scope::Scope(Scope&& other) noexcept {
        this->arena = other.arena;
        this->offset = other.offset;
        other.active = false;
    }
    Scope& Scope::operator=(Scope&& other) noexcept {
        this->arena = other.arena;
        this->offset = other.offset;
        other.active = false;
        this->active = true;
        return *this;
    }
}