module plexdb.arena;

import plexdb.os;

namespace plexdb::arena {
    ArenaPage* allocate(U64 page_size, void* optional_backing_buffer) {
        // allocate initial block
        void* base = optional_backing_buffer;
        if (base == nullptr) {
            base = os::allocate(page_size + HEADER_SIZE);
        }
        
        // extract arena header & fill
        ArenaPage* arena = reinterpret_cast<ArenaPage*>(base);
        *arena = (ArenaPage) {
            .prev = nullptr,
            .current = arena,
            .free_stack = nullptr,
            .page_offset = HEADER_SIZE,
            .page_size = page_size,
            .base_offset = 0,
        };
        return arena;
    }

    void deallocate(ArenaPage* arena) {
        // @note assumes free pages are only marked on bottom
        for (ArenaPage *x = arena->free_stack, *prev = nullptr; x != nullptr && x != arena; x = prev) {
            prev = x->prev;
            os::deallocate(x);
        }

        for (ArenaPage *x = arena, *prev = nullptr; x != nullptr; x = prev) {
            prev = x->prev;
            os::deallocate(x);
        }
    }

    Arena::Arena(U64 page_size, void* optional_backing_buffer)
      : page(allocate(page_size, optional_backing_buffer)) {}
    Arena::Arena(ArenaPage* page) : page(page) {}
    Arena::~Arena() {
        if (page != nullptr)
            deallocate(page);
    }
    Arena::Arena(Arena&& other) noexcept {
        this->page = other.page;
        other.page = nullptr;
    }
    Arena& Arena::operator=(Arena&& other) noexcept {
        ArenaPage* tmp = this->page;
        this->page = other.page;
        other.page = tmp;
        return *this;
    }

    void* push(Arena& arena, U64 size, U64 align) {
        ArenaPage* current = arena.page->current;
        U64 page_offset_before = align_pow2(current->page_offset, align);
        U64 page_offset_after = page_offset_before + size;

        // add a new page if needed
        if (page_offset_after > HEADER_SIZE + current->page_size) {
            ArenaPage* new_page;

            // if there is a large enough page on top of the free list use it
            if (arena.page->free_stack != nullptr && arena.page->free_stack->page_size > size) {
                new_page = arena.page->free_stack;
                arena.page->free_stack = (new_page->prev == arena.page) ? nullptr : new_page->prev;

                new_page->current = new_page;
                new_page->page_offset = HEADER_SIZE;
            } else {
                U64 new_page_size = current->page_size;
                if(size > new_page_size) {
                    new_page_size = align_pow2(size, align);
                }
        
                new_page = allocate(new_page_size, nullptr);
            }

            new_page->base_offset = current->base_offset + HEADER_SIZE + current->page_size;
            new_page->prev = current;
            arena.page->current = new_page;
            current = new_page;

            page_offset_before = align_pow2(current->page_offset, align);
            page_offset_after = page_offset_before + size;
            assert_true(page_offset_after <= HEADER_SIZE + current->page_size, "page not large enough"); // @todo reserve across pages
        }

        // push onto current page
        current->page_offset = page_offset_after;
        return reinterpret_cast<U8*>(current) + page_offset_before;
    }

    U64 offset(Arena& arena) {
        return arena.page->current->base_offset + arena.page->current->page_offset;
    }

    void pop_to(Arena& arena, U64 offset) {
        ArenaPage* current = arena.page->current;

        // free pages if needed @note assumes free pages are only marked on bottom
        while (offset < current->base_offset + HEADER_SIZE) {
            arena.page->current = arena.page->current->prev;
            os::deallocate(current);
            current = arena.page->current;
        }
        assert_true(!(offset > current->base_offset && offset < current->base_offset + HEADER_SIZE), "offset is in header portion");

        current->page_offset = max(offset - current->base_offset, HEADER_SIZE);
    }

    void clear(Arena& arena) {
        arena.page->page_offset = HEADER_SIZE;
        // os::memory_zero(reinterpret_cast<U8*>(arena) + HEADER_SIZE, arena.page->page_size - HEADER_SIZE);

        // add pages to free list for use
        if (arena.page->current != arena.page) {
            arena.page->free_stack = arena.page->current;
            arena.page->current = arena.page;
        }
    }

    // temporary arena scope
    Scope::Scope(Arena* arena) : arena(arena), off(offset(*arena)) {}
    Scope::~Scope() {
        pop_to(*this->arena, this->off);
    }
}