// @todo benchmark
module;
#include <initializer_list>
export module plexdb.dynamic.containers;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb {
    // ========================================================================
    // array
    // ========================================================================
    constexpr U64 DYNAMIC_ARRAY_INITIAL_CAPACITY = 4;
    constexpr U64 DYNAMIC_ARRAY_CAPACITY_GROWTH_RATE = 2;

    template <typename T, typename Size = U64>
    struct DynamicArray;

    template <typename T, typename Size = U64>
    void clear(struct DynamicArray<T, Size>& arr);

    template <typename T, typename Size>
    void reserve(DynamicArray<T,Size>& arr, Size new_capacity);

    template <typename T, typename Size>
    T& push_back(DynamicArray<T,Size>& arr, const T& value);

    // @todo move construct vs zero initialize
    template <typename T, typename Size>
    struct DynamicArray {
        using Element = T;

        T*  ptr      = nullptr;
        Size length   = 0;
        Size capacity = 0;

        DynamicArray() = default;

        DynamicArray(Size length) {
            reserve(*this, length);
            for (Size i = 0; i < length; ++i) {
                new (ptr + i) T{};
            }
            this->length = length;
        }

        DynamicArray(const DynamicArray& other) : ptr(nullptr), length(0), capacity(0) {
            if (other.length > 0) {
                reserve(*this, other.length);
                for (Size i = 0; i < other.length; ++i) {
                    new (ptr + i) T(other.ptr[i]);
                }
                length = other.length;
            }
        }

        DynamicArray(DynamicArray&& other) noexcept : ptr(other.ptr), length(other.length), capacity(other.capacity) {
            other.ptr = nullptr;
            other.length = 0;
            other.capacity = 0;
        }

        DynamicArray& operator=(const DynamicArray& other) {
            if (this != &other) {
                clear(*this);
                reserve(*this, other.length);
                for (Size i = 0; i < other.length; ++i) {
                    new (ptr + i) T(other.ptr[i]);
                }
                length = other.length;
            }
            return *this;
        }

        DynamicArray& operator=(DynamicArray&& other) noexcept {
            if (this != &other) {
                clear(*this);
                if (ptr) {
                    os::deallocate(ptr);
                }

                ptr = other.ptr;
                length = other.length;
                capacity = other.capacity;

                other.ptr = nullptr;
                other.length = 0;
                other.capacity = 0;
            }
            return *this;
        }

        ~DynamicArray() {
            clear(*this);
            if (ptr) {
                os::deallocate(ptr);
                ptr = nullptr;
                capacity = 0;
            }
        }

        T* begin() noexcept { return ptr; }
        T* end()   noexcept { return ptr + length; }

        const T* begin() const noexcept { return ptr; }
        const T* end()   const noexcept { return ptr + length; }

        const T* cbegin() const noexcept { return ptr; }
        const T* cend()   const noexcept { return ptr + length; }

        T& operator[](Size index) noexcept {
            assert_true(index >= 0 && index < this->length, "index out of range");
            return ptr[index];
        }

        const T& operator[](Size index) const noexcept {
            assert_true(index >= 0 && index < this->length, "index out of range");
            return ptr[index];
        }

        DynamicArray(std::initializer_list<T> init) {
            reserve(*this, init.size());
            for (const T& v : init)
                plexdb::push_back(*this, v);
        }
    };

    template <typename T, typename Size>
    void reserve(DynamicArray<T,Size>& arr, Size new_capacity) {
        if (new_capacity <= arr.capacity)
            return;

        Size bytes = new_capacity * sizeof(T);
        T* new_ptr = reinterpret_cast<T*>(os::allocate(bytes));

        // move-construct into new memory
        for (Size i = 0; i < arr.length; ++i) {
            new (new_ptr + i) T(move(arr.ptr[i]));
            arr.ptr[i].~T();
        }

        os::deallocate(arr.ptr);

        arr.ptr = new_ptr;
        arr.capacity = new_capacity;
    }

    template <typename T, typename Size>
    void resize(DynamicArray<T,Size>& arr, Size new_length) {
        reserve(arr, new_length);

        // de/construct elements
        if (new_length >= arr.length) {
            for (Size i = arr.length; i < new_length; ++i) {
                new (arr.ptr + i) T();
            }
        } else {
            for (Size i = new_length; i < arr.length; ++i) {
                arr.ptr[i].~T();
            }
        }

        arr.length = new_length;
    }

    template <typename T, typename Size>
    void reserve_for_push(DynamicArray<T,Size>& arr) {
        if (arr.length == arr.capacity) {
            Size new_capacity = arr.capacity == 0 ? DYNAMIC_ARRAY_INITIAL_CAPACITY : arr.capacity * DYNAMIC_ARRAY_CAPACITY_GROWTH_RATE;
            reserve(arr, new_capacity);
        }
    }

    template <typename T, typename Size>
    T& push_back(DynamicArray<T,Size>& arr, const T& value) {
        reserve_for_push(arr);
        T* location = arr.ptr + arr.length;

        new (location) T(value);
        ++arr.length;

        return *location;
    }

    template <typename T, typename Size>
    T& push_back(DynamicArray<T,Size>& arr, T&& value) {
        reserve_for_push(arr);
        T* location = arr.ptr + arr.length;

        new (location) T(move(value));
        ++arr.length;

        return *location;
    }

    template <typename T, typename Size, typename... Args>
    T& emplace_back(DynamicArray<T, Size>& arr, Args&&... args) {
        reserve_for_push(arr);
        T* location = arr.ptr + arr.length;

        new (location) T(forward<Args>(args)...);
        ++arr.length;

        return *location;
    }


    template <typename T, typename Size>
    void pop_back(DynamicArray<T,Size>& arr) {
        if (arr.length == 0)
            return;

        --arr.length;
        arr.ptr[arr.length].~T();
    }

    template <typename T, typename Size>
    void clear(DynamicArray<T,Size>& arr) {
        for (Size i = 0; i < arr.length; ++i)
            arr.ptr[i].~T();

        arr.length = 0;
    }

    // @warn @perf requires shifting all subsequent entries by copy constructing each time, generally use a linked list instead
    template <typename T, typename Size, typename... Args>
        requires TriviallyCopyable<T>
    T& insert(DynamicArray<T,Size>& arr, U64 idx, Args&&... args) {
        // @todo avoid unnecessary double move construct
        reserve_for_push(arr);

        ++arr.length;
        for (Size i = arr.length - 1; i > idx; --i) {
            new (arr.ptr + i) T(move(arr.ptr[i - 1]));
            arr.ptr[i - 1].~T();
        }

        new (arr.ptr + idx) T(forward<Args>(args)...);

        return *(arr.ptr + idx);
    }


    // ========================================================================
    // map
    // ========================================================================
    constexpr U64 DYNAMIC_MAP_MAX_PAIRS_PER_SLOT = 4;
    constexpr U64 DYNAMIC_MAP_INITIAL_SLOTS = 4;
    constexpr U64 DYNAMIC_MAP_SLOT_GROWTH_RATE = 2;

    template<typename K, typename V>
    struct DynamicMap;

    template<typename K, typename V>
    bool find_slot_and_pair(const DynamicMap<K,V>& map, const K& key, U64& slot_idx, U64& pair_idx);

    template<typename K, typename V>
    void reserve_for_push(DynamicMap<K,V>& map);

    template<typename K, typename V, typename KArgs, typename VArgs>
    V& insert(DynamicMap<K,V>& map, KArgs&& key, VArgs&& value);

    template<typename K, typename V>
    struct DynamicMap {
        using Key = K;
        using Value = V;

        DynamicArray<DynamicArray<Pair<K, V>>> slots;

        DynamicMap() = default;

        struct Iterator {
            DynamicMap<K,V>* map = nullptr;
            U64 slot_idx = 0;
            U64 pair_idx = 0;

            bool operator!=(const Iterator& other) const {
                return map != other.map || slot_idx != other.slot_idx || pair_idx != other.pair_idx;
            }

            Pair<K,V>& operator*() {
                return map->slots[slot_idx][pair_idx];
            }

            Iterator& operator++() {
                if (!map) return *this;
                pair_idx++;
                while (slot_idx < map->slots.length && pair_idx >= map->slots[slot_idx].length) {
                    slot_idx++;
                    pair_idx = 0;
                }
                if (slot_idx >= map->slots.length) { map = nullptr; slot_idx = 0; }
                return *this;
            }

        };
        struct ConstIterator {
            const DynamicMap<K,V>* map = nullptr;
            U64 slot_idx = 0;
            U64 pair_idx = 0;

            bool operator!=(const ConstIterator& other) const {
                return map != other.map || slot_idx != other.slot_idx || pair_idx != other.pair_idx;
            }

            const Pair<K,V>& operator*() const {
                return map->slots[slot_idx][pair_idx];
            }

            ConstIterator& operator++() {
                if (!map) return *this;
                pair_idx++;
                while (slot_idx < map->slots.length && pair_idx >= map->slots[slot_idx].length) {
                    slot_idx++;
                    pair_idx = 0;
                }
                if (slot_idx >= map->slots.length) { map = nullptr; slot_idx = 0; }
                return *this;
            }
        };

        Iterator begin() {
            U64 s = 0;
            while (s < slots.length && slots[s].length == 0) s++;
            if (s >= slots.length) return Iterator{ .map = nullptr };
            return Iterator{ .map = this, .slot_idx = s, .pair_idx = 0 };
        }

        Iterator end() {
            return Iterator{ .map = nullptr };
        }

        ConstIterator begin() const {
            U64 s = 0;
            while (s < slots.length && slots[s].length == 0) s++;
            if (s >= slots.length) return ConstIterator{ .map = nullptr };
            return ConstIterator{ .map = this, .slot_idx = s, .pair_idx = 0 };
        }

        ConstIterator end() const {
            return ConstIterator{ .map = nullptr };
        }

        Iterator operator[](const K& key) noexcept {
            U64 slot_idx, pair_idx;
            if (find_slot_and_pair(*this, key, slot_idx, pair_idx)) {
                return Iterator{ .map = this, .slot_idx = slot_idx, .pair_idx = pair_idx };
            }

            reserve_for_push(*this);
            slot_idx = hash(key) % slots.length;
            DynamicArray<Pair<K,V>>& bucket = slots[slot_idx];
            push_back(bucket, Pair<K,V>(key, V{}));
            pair_idx = bucket.length - 1;
            return Iterator{ .map = this, .slot_idx = slot_idx, .pair_idx = pair_idx };
        }

        ConstIterator operator[](const K& key) const noexcept {
            U64 slot_idx, pair_idx;
            bool found = find_slot_and_pair(*this, key, slot_idx, pair_idx);
            assert_true(found, "cannot insert key into const map");
            return ConstIterator{ .map = const_cast<DynamicMap*>(this), .slot_idx = slot_idx, .pair_idx = pair_idx };
        }

        DynamicMap(std::initializer_list<Pair<K,V>> init) {
            for (const Pair<K,V>& p : init)
                insert(*this, p.first, p.second);
        }
    };

    template<typename K, typename V>
    void rehash(DynamicMap<K,V>& map, U64 new_slot_count) {
        assert_true(new_slot_count != 0, "zero rehash not allowed");

        DynamicArray<DynamicArray<Pair<K,V>>> old_slots = map.slots;

        DynamicArray<DynamicArray<Pair<K,V>>> new_slots;
        resize(new_slots, new_slot_count);
        for (U64 i = 0; i < new_slot_count; i++) {
            reserve(new_slots[i], DYNAMIC_MAP_MAX_PAIRS_PER_SLOT);
        }

        for (U64 i = 0; i < old_slots.length; i++) {
            DynamicArray<Pair<K,V>>& bucket = old_slots[i];
            for (U64 j = 0; j < bucket.length; j++) {
                const Pair<K,V>& p = bucket[j];
                U64 new_idx = hash(p.first) % new_slot_count;
                push_back(new_slots[new_idx], Pair<K,V>(p.first, p.second));
            }
        }

        map.slots = move(new_slots);
    }

    template<typename K, typename V>
    void reserve_for_push(DynamicMap<K,V>& map) {
        if (map.slots.length == 0) {
            resize(map.slots, DYNAMIC_MAP_INITIAL_SLOTS);
            return;
        }

        for (U64 i = 0; i < map.slots.length; i++) {
            if (map.slots[i].length > DYNAMIC_MAP_MAX_PAIRS_PER_SLOT) {
                U64 new_slot_count = map.slots.length * DYNAMIC_MAP_SLOT_GROWTH_RATE + 1;
                rehash(map, new_slot_count);
                break;
            }
        }
    }

    template<typename K, typename V>
    bool find_slot_and_pair(const DynamicMap<K,V>& map, const K& key, U64& slot_idx, U64& pair_idx) {
        if (map.slots.length == 0) return false;
        slot_idx = hash(key) % map.slots.length;
        const DynamicArray<Pair<K,V>>& bucket = map.slots[slot_idx];
        for (U64 i = 0; i < bucket.length; i++) {
            if (bucket[i].first == key) {
                pair_idx = i;
                return true;
            }
        }
        return false;
    }

    template<typename K, typename V>
    U64 length(const DynamicMap<K,V>& map) {
        U64 n = 0;
        for (U64 i = 0; i < map.slots.length; i++) n += map.slots[i].length;
        return n;
    }

    template<typename K, typename V>
    typename DynamicMap<K,V>::Iterator find_it(DynamicMap<K,V>& map, const K& key) {
        U64 slot_idx, pair_idx;
        if (!find_slot_and_pair(map, key, slot_idx, pair_idx)) return map.end();
        return typename DynamicMap<K,V>::Iterator{ .map = &map, .slot_idx = slot_idx, .pair_idx = pair_idx };
    }

    template<typename K, typename V>
    typename DynamicMap<K,V>::ConstIterator find_it(const DynamicMap<K,V>& map, const K& key) {
        U64 slot_idx, pair_idx;
        if (!find_slot_and_pair(map, key, slot_idx, pair_idx)) return map.end();
        return typename DynamicMap<K,V>::ConstIterator{ .map = &map, .slot_idx = slot_idx, .pair_idx = pair_idx };
    }

    template<typename K, typename V>
    V* find(DynamicMap<K,V>& map, const K& key) {
        U64 slot_idx, pair_idx;
        if (!find_slot_and_pair(map, key, slot_idx, pair_idx)) return nullptr;
        return &map.slots[slot_idx][pair_idx].second;
    }

    template<typename K, typename V>
    const V* find(const DynamicMap<K,V>& map, const K& key) {
        U64 slot_idx, pair_idx;
        if (!find_slot_and_pair(map, key, slot_idx, pair_idx)) return nullptr;
        return &map.slots[slot_idx][pair_idx].second;
    }

    template<typename K, typename V>
    bool remove(DynamicMap<K,V>& map, const K& key) {
        U64 slot_idx, pair_idx;
        if (!find_slot_and_pair(map, key, slot_idx, pair_idx)) return false;
        DynamicArray<Pair<K,V>>& bucket = map.slots[slot_idx];
        bucket[pair_idx] = move(bucket[bucket.length - 1]);
        pop_back(bucket);
        return true;
    }

    template<typename K, typename V, typename KArgs, typename VArgs>
    V& insert(DynamicMap<K,V>& map, KArgs&& key, VArgs&& value) {
        U64 slot_idx, pair_idx;
        if (find_slot_and_pair(map, key, slot_idx, pair_idx)) {
            map.slots[slot_idx][pair_idx].second = value;
            return map.slots[slot_idx][pair_idx].second;
        }

        reserve_for_push(map);
        slot_idx = hash(key) % map.slots.length;
        DynamicArray<Pair<K,V>>& bucket = map.slots[slot_idx];
        emplace_back(bucket, forward<KArgs>(key), forward<VArgs>(value));
        return bucket[bucket.length - 1].second;
    }

    template<typename K, typename V>
    const K& extract_key(const Pair<K,V>& p) {
        return p.first;
    }

    template<typename K, typename V>
    const K& extract_key(const K& key, const V&) {
        return key;
    }

    template<typename K, typename V>
    const K& extract_key(const K& key) {
        return key;
    }

    template<typename K, typename V, typename... Args>
    V& emplace(DynamicMap<K,V>& map, Args&&... args) {
        U64 slot_idx, pair_idx;
        if (find_slot_and_pair(map, extract_key(forward<Args>(args)...), slot_idx, pair_idx)) {
            return map.slots[slot_idx][pair_idx].second;
        }

        reserve_for_push(map);
        slot_idx = hash(extract_key(forward<Args>(args)...)) % map.slots.length;
        DynamicArray<Pair<K,V>>& bucket = map.slots[slot_idx];
        emplace_back(bucket, forward<Args>(args)...);
        return bucket[bucket.length - 1].second;
    }

    template<typename K, typename V>
    typename DynamicMap<K,V>::Iterator clear(DynamicMap<K,V>& map) {
        for (U64 i = 0; i < map.slots.length; i++) {
            clear(map.slots[i]);
        }
        return map.end();
    }

    template<typename K, typename V>
    void reserve(DynamicMap<K,V>& map, U64 count) {
        reserve(map.slots, 1 + count/DYNAMIC_MAP_MAX_PAIRS_PER_SLOT);
        for (U64 i = 0; i < map.slots.length; i++) {
            reserve(map.slots[i], DYNAMIC_MAP_MAX_PAIRS_PER_SLOT);
        }
    }

    // ========================================================================
    // set
    // ========================================================================
    constexpr U64 DYNAMIC_SET_MAX_KEYS_PER_SLOT = 4;
    constexpr U64 DYNAMIC_SET_INITIAL_SLOTS = 4;
    constexpr U64 DYNAMIC_SET_SLOT_GROWTH_RATE = 2;

    template<typename K>
    struct DynamicSet;

    template<typename K>
    bool find_slot(const DynamicSet<K>& set, const K& key, U64& slot_idx, U64& key_idx);

    template<typename K>
    void reserve_for_push(DynamicSet<K>& set);

    template<typename K>
    bool insert(DynamicSet<K>& set, const K& key);

    template<typename K>
    struct DynamicSet {
        using Key = K;
        DynamicArray<DynamicArray<K>> slots;

        struct Iterator {
            DynamicSet<K>* set = nullptr;
            U64 slot_idx = 0;
            U64 key_idx = 0;

            bool operator!=(const Iterator& other) const {
                return set != other.set || slot_idx != other.slot_idx || key_idx != other.key_idx;
            }

            K& operator*() {
                return set->slots[slot_idx][key_idx];
            }

            Iterator& operator++() {
                if (!set) return *this;
                key_idx++;
                while (slot_idx < set->slots.length && key_idx >= set->slots[slot_idx].length) {
                    slot_idx++;
                    key_idx = 0;
                }
                if (slot_idx >= set->slots.length) { set = nullptr; slot_idx = 0; }
                return *this;
            }
        };

        struct ConstIterator {
            const DynamicSet<K>* set = nullptr;
            U64 slot_idx = 0;
            U64 key_idx = 0;

            bool operator!=(const ConstIterator& other) const {
                return set != other.set || slot_idx != other.slot_idx || key_idx != other.key_idx;
            }

            const K& operator*() const {
                return set->slots[slot_idx][key_idx];
            }

            ConstIterator& operator++() {
                if (!set) return *this;
                key_idx++;
                while (slot_idx < set->slots.length && key_idx >= set->slots[slot_idx].length) {
                    slot_idx++;
                    key_idx = 0;
                }
                if (slot_idx >= set->slots.length) { set = nullptr; slot_idx = 0; }
                return *this;
            }
        };

        Iterator begin() {
            U64 s = 0;
            while (s < slots.length && slots[s].length == 0) s++;
            if (s >= slots.length) return Iterator{ nullptr };
            return Iterator{ this, s, 0 };
        }

        Iterator end() {
            return Iterator{ nullptr };
        }

        ConstIterator begin() const {
            U64 s = 0;
            while (s < slots.length && slots[s].length == 0) s++;
            if (s >= slots.length) return ConstIterator{ nullptr };
            return ConstIterator{ const_cast<DynamicSet*>(this), s, 0 };
        }

        ConstIterator end() const {
            return ConstIterator{ .set = nullptr };
        }

        Iterator operator[](const K& key) {
            U64 slot_idx, key_idx;
            if (find_slot(*this, key, slot_idx, key_idx)) {
                return Iterator{ this, slot_idx, key_idx };
            }

            reserve_for_push(*this);
            slot_idx = hash(key) % slots.length;
            DynamicArray<K>& bucket = slots[slot_idx];
            push_back(bucket, key);
            key_idx = bucket.length - 1;
            return Iterator{ this, slot_idx, key_idx };
        }

        ConstIterator operator[](const K& key) const {
            U64 slot_idx, key_idx;
            bool found = find_slot(*this, key, slot_idx, key_idx);
            assert_true(found, "cannot insert key into const map");
            return ConstIterator{ const_cast<DynamicSet*>(this), slot_idx, key_idx };
        }

        DynamicSet(std::initializer_list<K> init) {
            for (const K& k : init)
                insert(*this, k);
        }
    };

    template<typename K>
    bool find_slot(const DynamicSet<K>& set, const K& key, U64& slot_idx, U64& key_idx) {
        if (set.slots.length == 0) return false;
        slot_idx = hash(key) % set.slots.length;
        const DynamicArray<K>& bucket = set.slots[slot_idx];
        for (U64 i = 0; i < bucket.length; i++) {
            if (bucket[i] == key) {
                key_idx = i;
                return true;
            }
        }
        return false;
    }

    template<typename K>
    void reserve_for_push(DynamicSet<K>& set) {
        if (set.slots.length == 0) {
            resize(set.slots, DYNAMIC_SET_INITIAL_SLOTS);
            return;
        }

        for (U64 i = 0; i < set.slots.length; i++) {
            if (set.slots[i].length > DYNAMIC_SET_MAX_KEYS_PER_SLOT) {
                U64 new_slot_count = set.slots.length * DYNAMIC_SET_SLOT_GROWTH_RATE + 1;
                rehash(set, new_slot_count);
                break;
            }
        }
    }

    template<typename K>
    void rehash(DynamicSet<K>& set, U64 new_slot_count) {
        assert_true(new_slot_count != 0, "zero rehash not allowed");

        DynamicArray<DynamicArray<K>> old_slots = move(set.slots);
        DynamicArray<DynamicArray<K>> new_slots;
        resize(new_slots, new_slot_count);

        for (U64 i = 0; i < new_slot_count; i++) {
            reserve(new_slots[i], DYNAMIC_SET_MAX_KEYS_PER_SLOT);
        }

        for (U64 i = 0; i < old_slots.length; i++) {
            DynamicArray<K>& bucket = old_slots[i];
            for (U64 j = 0; j < bucket.length; j++) {
                const K& key = bucket[j];
                U64 idx = hash(key) % new_slot_count;
                push_back(new_slots[idx], key);
            }
        }

        set.slots = move(new_slots);
    }

    template<typename K>
    U64 length(const DynamicSet<K>& set) {
        U64 n = 0;
        for (U64 i = 0; i < set.slots.length; i++) n += set.slots[i].length;
        return n;
    }

    template<typename K>
    bool contains(const DynamicSet<K>& set, const K& key) {
        U64 slot_idx, key_idx;
        return find_slot(set, key, slot_idx, key_idx);
    }

    template<typename K>
    bool insert(DynamicSet<K>& set, const K& key) {
        U64 slot_idx, key_idx;
        if (find_slot(set, key, slot_idx, key_idx)) return false;
        reserve_for_push(set);
        slot_idx = hash(key) % set.slots.length;
        push_back(set.slots[slot_idx], key);
        return true;
    }

    template<typename K>
    void clear(DynamicSet<K>& set) {
        for (U64 i = 0; i < set.slots.length; i++) {
            clear(set.slots[i]);
        }
    }

    // ========================================================================
    // types
    // ========================================================================
    template<typename T> struct IsDynamicArrayHelper                  { static constexpr bool value = false; };
    template<typename T> struct IsDynamicArrayHelper<DynamicArray<T>> { static constexpr bool value = true;  };
    template<typename T> struct IsDynamicSetHelper                    { static constexpr bool value = false; };
    template<typename T> struct IsDynamicSetHelper<DynamicSet<T>>     { static constexpr bool value = true;  };
    template<typename T> struct IsDynamicMapHelper                              { static constexpr bool value = false; };
    template<typename U, typename V> struct IsDynamicMapHelper<DynamicMap<U,V>> { static constexpr bool value = true;  };

    template<typename T> concept IsDynamicArray = IsDynamicArrayHelper<T>::value;
    template<typename T> concept IsDynamicSet   = IsDynamicSetHelper<T>::value;
    template<typename T> concept IsDynamicMap   = IsDynamicMapHelper<T>::value;

    // array
    template<typename List, typename = void>
    struct ExpandDynamicArrayHelper;

    template<typename... Ts>
    struct ExpandDynamicArrayHelper<TypeList<Ts...>> {
        using type = TypeList<DynamicArray<Ts>...>;
    };

    template<typename Ts>
    using ExpandDynamicArray = ExpandDynamicArrayHelper<Ts>::type;

    // map
    template<typename K, typename ValueList>
    struct ExpandDynamicMapValuesHelper;

    template<typename K, typename... Vs>
    struct ExpandDynamicMapValuesHelper<K, TypeList<Vs...>> {
        using type = TypeList<DynamicMap<K, Vs>...>;
    };

    template<typename Ks, typename Vs>
    struct ExpandDynamicMapHelper;

    template<typename K1, typename... Ks, typename Vs>
    struct ExpandDynamicMapHelper<TypeList<K1, Ks...>, Vs> {
    private:
        using K1Maps = typename ExpandDynamicMapValuesHelper<K1, Vs>::type;
        using K2NMaps = typename ExpandDynamicMapHelper<TypeList<Ks...>, Vs>::type;
    public:
        using type = Concat<K1Maps, K2NMaps>;
    };

    template<typename Vs>
    struct ExpandDynamicMapHelper<TypeList<>, Vs> {
        using type = TypeList<>;
    };

    template<typename Ks, typename Vs>
    using ExpandDynamicMap = ExpandDynamicMapHelper<Ks, Vs>::type;

    // set
    template<typename List, typename = void>
    struct ExpandDynamicSetHelper;

    template<typename... Ts>
    struct ExpandDynamicSetHelper<TypeList<Ts...>> {
        using type = TypeList<DynamicSet<Ts>...>;
    };

    template<typename Ts>
    using ExpandDynamicSet = ExpandDynamicSetHelper<Ts>::type;
}
