module;
#include "macros.h"

export module plexdb.threads;

import plexdb.base;
import plexdb.arena;

export namespace plexdb::threads {
    struct ThreadContext {
        Arena arenas[2];
    };
    
    void           equip(ThreadContext* in_ctx);
    ThreadContext* get_context();
    Arena*         get_scratch(TArrayView<Arena*,U64> conflicts);

    struct Scope {
        Arena* arena;
        U64 offset;
        bool active = true;

        Scope(Arena* in_arena);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        Scope(Scope&&) noexcept;
        Scope& operator=(Scope&&) noexcept;
    };
    
    inline Scope scratch(TArrayView<Arena*,U64> conflicts) { return Scope(get_scratch(conflicts)); }
    inline Scope scratch()                                 { return scratch(TArrayView<Arena*,U64>()); }
}

export namespace plexdb {
    using ThreadContext = threads::ThreadContext;
}