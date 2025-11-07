module plexdb.arena;

import plexdb.os;

namespace plexdb::arena {
    Arena* alloc(U64 page_size, void* optional_backing_buffer) {
        // allocate initial block
        void* base = optional_backing_buffer;
        if (base == nullptr) {
            base = os::allocate(page_size);
        }
        
        // extract arena header & fill
        Arena* arena = (Arena*)base;
        *arena = (Arena){
            .prev = nullptr,
            .current = arena,
            .free_stack = nullptr,
            .page_offset = HEADER_SIZE,
            .page_size = page_size,
            .base_offset = 0,
        };
        return arena;
    }

    void release(Arena* arena) {
        // @note assumes free pages are only marked on bottom
        for (Arena *x = arena->free_stack, *prev = nullptr; x != nullptr && x != arena; x = prev) {
            prev = x->prev;
            os::deallocate(x);
        }

        for (Arena *x = arena, *prev = nullptr; x != nullptr; x = prev) {
            prev = x->prev;
            os::deallocate(x);
        }
    }

    void* push(Arena* arena, U64 size, U64 align) {
        Arena* current = arena->current;
        U64 page_offset_before = align_pow2(current->page_offset, align);
        U64 page_offset_after = page_offset_before + size;

        // add a new page if needed
        if (page_offset_after > current->page_size) {
            Arena* new_page;

            // if there is a large enough page on top of the free list use it
            if (arena->free_stack != nullptr && arena->free_stack->page_size > size) {
                new_page = arena->free_stack;
                arena->free_stack = (new_page->prev == arena) ? nullptr : new_page->prev;

                new_page->current = new_page;
                new_page->page_offset = HEADER_SIZE;
            } else {
                U64 new_page_size = current->page_size;
                if(size + HEADER_SIZE > new_page_size) {
                    new_page_size = align_pow2(size + HEADER_SIZE, align);
                }
        
                new_page = alloc(new_page_size);
            }

            new_page->base_offset = current->base_offset + current->page_size;
            (arena->current)->prev = new_page;
            new_page = arena->current;

            current = new_page;
            page_offset_before = align_pow2(current->page_offset, align);
            page_offset_after = page_offset_before + size;
            // Assert(page_offset_after <= current->page_size); // @todo reserve across pages
        }

        // push onto current page
        current->page_offset = page_offset_after;
        return (U8*)current + page_offset_before;
    }

    U64 offset(Arena *arena) {
        return arena->current->base_offset + arena->current->page_offset;
    }

    void pop_to(Arena* arena, U64 offset) {
        Arena* current = arena->current;

        // free pages if needed @note assumes free pages are only marked on bottom
        while (offset < current->base_offset + HEADER_SIZE) {
            arena->current = arena->current->prev;
            os::deallocate(current);
            current = arena->current;

            // Assert(!(offset > current->base_offset && offset < current->base_offset + HEADER_SIZE));
        }

        current->page_offset = max(offset - current->base_offset, HEADER_SIZE);
    }

    void pop(Arena* arena, U64 size) {
        // @todo consider page header sizes + alignment? currently this is not 
        // guaranteed to reverse a push of the same size
        pop_to(arena, offset(arena) - size);
    }

    void clear(Arena* arena) {
        arena->page_offset = HEADER_SIZE;
        // memset((u8*)arena + HEADER_SIZE, 0, arena->page_size - HEADER_SIZE);

        // add pages to free list for use
        if (arena->current != arena) {
            arena->free_stack = arena->current;
            arena->current = arena;
        }
    }

    // temporary arena scopes
    Temp temp_begin(Arena* arena) {
        return (Temp) {
            .arena = arena,
            .offset = offset(arena),
        };
    }

    void temp_end(Temp temp) {
        pop_to(temp.arena, temp.offset);
    }
}