module;
#include <atomic>

export module plexdb.shard.spsc;

import plexdb.base;

export namespace plexdb::shard {
    // Lock-free single-producer single-consumer ring buffer.
    // Capacity must be a power of two.
    // Head and tail live on separate cache lines to avoid false sharing.
    template<typename T, U64 Capacity>
    struct SpscQueue {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
        static constexpr U64 mask = Capacity - 1;
        static constexpr U64 capacity = Capacity;

        alignas(64) std::atomic<U64> head{0};
        alignas(64) std::atomic<U64> tail{0};
        T buffer[Capacity];
    };

    template<typename T, U64 Capacity>
    bool try_push(SpscQueue<T, Capacity>& q, const T& value) {
        U64 h = q.head.load(std::memory_order_relaxed);
        U64 t = q.tail.load(std::memory_order_acquire);
        if (h - t >= Capacity)
            return false;
        q.buffer[h & SpscQueue<T, Capacity>::mask] = value;
        q.head.store(h + 1, std::memory_order_release);
        return true;
    }

    template<typename T, U64 Capacity>
    bool try_pop(SpscQueue<T, Capacity>& q, T& out) {
        U64 t = q.tail.load(std::memory_order_relaxed);
        U64 h = q.head.load(std::memory_order_acquire);
        if (t == h)
            return false;
        out = q.buffer[t & SpscQueue<T, Capacity>::mask];
        q.tail.store(t + 1, std::memory_order_release);
        return true;
    }

    template<typename T, U64 Capacity>
    U64 size(const SpscQueue<T, Capacity>& q) {
        U64 h = q.head.load(std::memory_order_acquire);
        U64 t = q.tail.load(std::memory_order_acquire);
        return h - t;
    }
}
