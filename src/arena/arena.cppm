module;
#include "macros.h"

export module plexdb.arena;

import plexdb.base;

namespace plexdb::arena {
    export U64 default_page_size = 1000u;
    export constexpr U64 HEADER_SIZE = 128u;
    
    export struct Arena {
        Arena* prev;
        Arena* current;
    
        Arena* free_stack;
    
        U64 page_offset;
        U64 page_size;
        U64 base_offset;
    };
    static_assert(sizeof(Arena) <= HEADER_SIZE);
    
    export struct Temp
    {
        Arena *arena;
        U64 offset;
    };
    
    export Arena* alloc(U64 page_size, void* optional_backing_buffer = nullptr);
    export inline Arena* alloc() { return alloc(/*page_size*/ default_page_size, /*optional_backing_buffer*/ nullptr); }
    export void release(Arena* arena);
    
    export void* push(Arena* arena, U64 size, U64 align);
    
    export U64   offset(Arena* arena);
    export void  pop_to(Arena* arena, U64 offset);
    export void  pop(Arena* arena, U64 size);

    export void  clear(Arena* arena);
    
    export Temp temp_begin(Arena* arena);
    export void temp_end(Temp temp);

    export template<typename El>
    inline El* push_array_no_zero_aligned(Arena* arena, U64 count, U64 align) { return (El*)push(arena, sizeof(El)*count, align); }
    export template<typename El>
    inline El* push_array_aligned        (Arena* arena, U64 count, U64 align) { return (El*)memset(push_array_no_zero_aligned<El>(arena, count, align), 0u, sizeof(El)*count); }
    export template<typename El>
    inline El* push_array_no_zero        (Arena* arena, U64 count)            { return (El*)push_array_no_zero_aligned<El>(arena, count, max(8ul, PLEXDB_ALIGNOF(El))); }
    export template<typename El>
    inline El* push_array                (Arena* arena, U64 count)            { return (El*)push_array_aligned<El>(arena, count, max(8ul, PLEXDB_ALIGNOF(El))); }
    export template<typename El>
    inline El* push_array                (Arena* arena)                       { return push_array<El>(arena); }
}