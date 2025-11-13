namespace plexdb::os {
    U8* allocate(U64 size) {
        return reinterpret_cast<U8*>(malloc(size));
    }
    
    void deallocate(void* ptr) {
        free(ptr);
    }

    void memory_copy(void* dst, const void* src, U64 bytes) noexcept {
        __builtin_memcpy(dst, src, bytes);
    }

    void memory_move(void* dst, const void* src, U64 bytes) noexcept {
        __builtin_memmove(dst, src, bytes);
    }

    void memory_zero(void* dst, U64 bytes) noexcept {
        __builtin_memset(dst, 0, bytes);
    }
}