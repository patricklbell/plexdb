module;
#include "macros.h"

export module plexdb.arena;

import plexdb.base;
import plexdb.os;

namespace plexdb::arena {
    export constexpr U64 default_page_size = 1000u;
    export constexpr U64 HEADER_SIZE = 128u;

    export struct ArenaPage {
        ArenaPage* prev;
        ArenaPage* current;

        ArenaPage* free_stack;

        U64 page_offset;
        U64 page_size;
        U64 base_offset;
    };
    static_assert(sizeof(ArenaPage) <= HEADER_SIZE);
    export ArenaPage* allocate(U64 page_size, void* optional_backing_buffer);
    export void deallocate(ArenaPage* page);

    export void* push(ArenaPage** page, U64 size, U64 align = 8ul);
    export U64   offset(ArenaPage* page);
    export void  pop_to(ArenaPage** page, U64 offset);
    export void  clear(ArenaPage* page);

    export struct Arena {
        ArenaPage* page;

        // @note forward arguments?
        Arena(U64 page_size = default_page_size, void* optional_backing_buffer = nullptr);
        Arena(ArenaPage* page);
        ~Arena();

        Arena(const Arena&) = delete;
        Arena(Arena&& other) noexcept;

        Arena& operator=(const Arena&) = delete;
        Arena& operator=(Arena&&) noexcept;
    };

    export inline void* push(Arena& arena, U64 size, U64 align = 8ul) { return push(&arena.page, size, align); }
    export inline U64   offset(Arena& arena) { return offset(arena.page); }
    export inline void  pop_to(Arena& arena, U64 offset) { pop_to(&arena.page, offset); }
    export inline void  clear(Arena& arena) { clear(arena.page); }

    export template<typename El>
    inline El* push_array_no_zero_aligned(ArenaPage** page, U64 count, U64 align) { return (El*)push(page, sizeof(El)*count, align); }
    export template<typename El>
    inline El* push_array_aligned        (ArenaPage** page, U64 count, U64 align) { return (El*)os::memory_zero(push_array_no_zero_aligned<El>(page, count, align), sizeof(El)*count); }
    export template<typename El>
    inline El* push_array_no_zero        (ArenaPage** page, U64 count)            { return (El*)push_array_no_zero_aligned<El>(page, count, max(8ul, alignof(El))); }
    export template<typename El>
    inline El* push_array                (ArenaPage** page, U64 count)            { return (El*)push_array_aligned<El>(page, count, max(8ul, alignof(El))); }

    export template<typename El>
    inline El* push_array_no_zero_aligned(Arena& arena, U64 count, U64 align) { return push_array_no_zero_aligned<El>(&arena.page, count, align); }
    export template<typename El>
    inline El* push_array_aligned        (Arena& arena, U64 count, U64 align) { return push_array_aligned<El>(&arena.page, count, align); }
    export template<typename El>
    inline El* push_array_no_zero        (Arena& arena, U64 count)            { return push_array_no_zero<El>(&arena.page, count); }
    export template<typename El>
    inline El* push_array                (Arena& arena, U64 count)            { return push_array<El>(&arena.page, count); }
}

export namespace plexdb {
    // ========================================================================
    // base
    // ========================================================================
    using arena::Arena;

    // ========================================================================
    // container allocation helpers
    //      try to allocate nodes together instead to avoid frag.
    // ========================================================================
    template<typename C, typename T>
        requires CopyAssignable<T>
    void push_front(Arena& arena, C& container, const T& value) {
        auto node = arena::push_array_no_zero<typename C::Node>(arena, 1);
        node->value = value;
        push_front(container, node);
    }
    template<typename C, typename K, typename V>
        requires CopyAssignable<Pair<K,V>>
    void insert(Arena& arena, C& container, const Pair<K,V>& kv) {
        auto node = arena::push_array_no_zero<typename C::Node>(arena, 1);
        node->value = kv;
        insert(container, node);
    }
}
