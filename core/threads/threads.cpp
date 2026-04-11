module;
#include "macros.h"

module plexdb.threads;

namespace plexdb::threads {
    thread_static ThreadContext* thread_local_ctx;
    
    void equip(ThreadContext* in_ctx) {
        thread_local_ctx = in_ctx;
    }

    ThreadContext* get_context() {
        return thread_local_ctx;
    }

    Arena* get_scratch(TArrayView<Arena*,U64> conflicts) {
        ThreadContext* ctx = get_context();
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