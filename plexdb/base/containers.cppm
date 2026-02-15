export module plexdb.base.containers;

import plexdb.base.types;
import plexdb.base.math;

export namespace plexdb {
    // ========================================================================
    // array
    // ========================================================================
    template<typename T, U64 N>
    struct Array {
        static constexpr U64 length = N;

        T values[N]{};

        // ====================================================================
        // Iterators
        // ====================================================================
        struct Iterator {
            T* curr = nullptr;

            constexpr Iterator() = default;
            constexpr explicit Iterator(T* curr) : curr(curr) {}

            constexpr Iterator& operator++() { ++curr; return *this; }
            constexpr Iterator operator++(int) {
                Iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            constexpr T& operator*() const { return *curr; }

            constexpr bool operator==(const Iterator& other) const {
                return curr == other.curr;
            }
            constexpr bool operator!=(const Iterator& other) const {
                return curr != other.curr;
            }
        };

        struct ConstIterator {
            const T* curr = nullptr;

            constexpr ConstIterator() = default;
            constexpr explicit ConstIterator(const T* curr) : curr(curr) {}

            constexpr ConstIterator& operator++() { ++curr; return *this; }
            constexpr ConstIterator operator++(int) {
                ConstIterator tmp = *this;
                ++(*this);
                return tmp;
            }

            constexpr const T& operator*() const { return *curr; }

            constexpr bool operator==(const ConstIterator& other) const {
                return curr == other.curr;
            }
            constexpr bool operator!=(const ConstIterator& other) const {
                return curr != other.curr;
            }
        };

        // ====================================================================
        // Constructors
        // ====================================================================
        constexpr Array() = default;

        constexpr Array(const Array& other) {
            for (U64 i = 0; i < N; ++i)
                values[i] = other.values[i];
        }

        constexpr Array(Array&& other) noexcept {
            for (U64 i = 0; i < N; ++i)
                values[i] = static_cast<T&&>(other.values[i]);
        }

        // ====================================================================
        // Assignment
        // ====================================================================
        constexpr Array& operator=(const Array& other) {
            if (this != &other) {
                for (U64 i = 0; i < N; ++i)
                    values[i] = other.values[i];
            }
            return *this;
        }

        constexpr Array& operator=(Array&& other) noexcept {
            if (this != &other) {
                for (U64 i = 0; i < N; ++i)
                    values[i] = static_cast<T&&>(other.values[i]);
            }
            return *this;
        }

        // ====================================================================
        // Element access
        // ====================================================================
        template<typename I>
        constexpr T& operator[](I i) noexcept {
            assert_true(i >= 0 && i < N, "out of range");
            return values[i];
        }

        template<typename I>
        constexpr const T& operator[](I i) const noexcept {
            assert_true(i >= 0 && i < N, "out of range");
            return values[i];
        }

        // ====================================================================
        // Iteration
        // ====================================================================
        constexpr Iterator begin() noexcept { return Iterator{values}; }
        constexpr Iterator end() noexcept { return Iterator{values + N}; }

        constexpr ConstIterator begin() const noexcept { return ConstIterator{values}; }
        constexpr ConstIterator end() const noexcept { return ConstIterator{values + N}; }

        constexpr ConstIterator cbegin() const noexcept { return ConstIterator{values}; }
        constexpr ConstIterator cend() const noexcept { return ConstIterator{values + N}; }

        // ====================================================================
        // Comparison
        // ====================================================================
        constexpr bool operator==(const Array& other) const {
            for (U64 i = 0; i < N; ++i) {
                if (!(values[i] == other.values[i]))
                    return false;
            }
            return true;
        }

        constexpr bool operator!=(const Array& other) const {
            return !(*this == other);
        }
    };

    // ========================================================================
    // stack
    // ========================================================================
    template<typename T>
    struct Stack {
        struct Node {
            T value;
            Node* next;
        };

        Node* top = nullptr;
        U64 length = 0;

        struct Iterator {
            Node* curr;

            Iterator(Node* curr) : curr(curr) {}

            Iterator& operator++() { curr = curr->next; return *this; }
            Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }

            T& operator*() const { return curr->value; }

            bool operator==(const Iterator& other) const { return curr == other.curr; }
            bool operator!=(const Iterator& other) const { return !(*this == other); }
        };

        Iterator begin() noexcept { return Iterator{top}; }
        Iterator end() noexcept { return Iterator{nullptr}; }

        // @todo const iterator
    };

    template<typename T>
    T* front(Stack<T>& stack) {
        return stack.top == nullptr ? nullptr : &stack.top->value;
    }
    template<typename T>
    void pop_front(Stack<T>& stack) {
        assert_true(stack.top != nullptr, "non-empty");
        stack.top = stack.top->next;
        stack.length--;
    }
    template<typename T>
    void push_front(Stack<T>& stack, typename Stack<T>::Node* node) {
        assert_true(node != nullptr, "valid node");
        node->next = stack.top;
        stack.top = node;
        stack.length++;
    }
    template<typename T>
    void clear(Stack<T>& stack) {
        stack.top = nullptr;
        stack.length = 0;
    }

    // ========================================================================
    // fixed slot map
    // ========================================================================
    template<typename K, typename V>
    struct MapFixedSlots {
        using Node = Stack<Pair<K,V>>::Node;

        Stack<Pair<K, V>>* slots;
        U64 slot_count;
        U64 length = 0;

        MapFixedSlots(Stack<Pair<K, V>>* slots, U64 slot_count) : slots(slots), slot_count(slot_count) {}

        template<U64 N>
        MapFixedSlots(Stack<Pair<K, V>> slots[N]) : slots(slots), slot_count(N) {}

        struct Iterator {
            MapFixedSlots<K,V>* map;
            U64 slot_idx;
            Node* node;

            Iterator(MapFixedSlots<K,V>* map, U64 slot_idx, Node* node) : map(map), slot_idx(slot_idx), node(node) {}

            Iterator& operator++() {
                if (node->next == nullptr) {
                    slot_idx++;
                    node = (slot_idx >= map->slot_count) ? nullptr : map->slots[slot_idx];
                } else {
                    node = node->next;
                }
                return *this; 
            }
            Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }

            Pair<K,V>& operator*() const { return node->value; }

            bool operator==(const Iterator& other) const { return node == other.node; }
            bool operator!=(const Iterator& other) const { return !(*this == other); }
        };

        Iterator begin() noexcept { return Iterator{this, 0, front(slots)}; }
        Iterator end() noexcept { return Iterator{this, slots->slot_count-1, nullptr}; }

        // @todo const iterator
    };

    template<typename K, typename V>
    V* find(MapFixedSlots<K, V>& map, const K& key) {
        U64 slot_idx = hash(key)%map.slot_count;

        for(auto& pair : map.slots[slot_idx]) {
            if (key == pair.key)
                return &pair.value;
        }
        return nullptr;
    }
    template<typename K, typename V>
    void insert(MapFixedSlots<K,V>& map, typename MapFixedSlots<K,V>::Node* node) {
        U64 slot_idx = hash(node->key)%map.slot_count;
        push_front(map.slots[slot_idx], node);
        map.length++;
    }
    template<typename K, typename V>
    void clear(MapFixedSlots<K,V>& map) {
        for(U64 slot_idx = 0; slot_idx < map.slot_count; slot_idx++) {
            clear(map.slots[slot_idx]);
        }
    }

    // ========================================================================
    // occupancy array
    // ========================================================================
    template<typename T, U64 element_count, typename O = U64>
        requires Unsigned<O>
    struct OccupancyArray {
        Array<T, element_count> array;

        static constexpr U64 mask_count = ceil_div(element_count, 8_u64*sizeof(O));
        Array<O, mask_count> occupancy = {0};

        // ====================================================================
        // Comparison
        // ====================================================================
        constexpr bool operator==(const OccupancyArray& other) const {
            return this->array == other.array && this->occupancy == other.occupancy;
        }

        constexpr bool operator!=(const OccupancyArray& other) const {
            return !(*this == other);
        }
    };

    template<typename T, U64 element_count, typename O, typename I>
    constexpr bool is_occupied(OccupancyArray<T, element_count, O>& oarr, I i) {
        U64 bitset_idx = i/oarr.mask_count;
        U64 bitset_bit = i%oarr.mask_count;
        return bool{(oarr.occupancy[bitset_idx] >> bitset_bit) & O{1}};
    }

    template<typename T, U64 element_count, typename O, typename I>
    constexpr void set_occupied(OccupancyArray<T, element_count, O>& oarr, I i, bool value) {
        U64 bitset_idx = i/oarr.mask_count;
        U64 bitset_bit = i%oarr.mask_count;
        O set_mask = O{value ? 1 : 0} << bitset_bit;
        oarr.occupancy[bitset_idx] = (oarr.occupancy[bitset_idx] & ~set_mask) | set_mask;
    }

    template<typename T, U64 element_count, typename O, typename I>
    constexpr void insert(OccupancyArray<T, element_count, O>& oarr, I i, T value) {
        oarr.array[i] = value;
        set_occupied(oarr, i, true);
    }

    template<typename T, U64 element_count, typename O, typename I>
    constexpr void remove(OccupancyArray<T, element_count, O>& oarr, I i) {
        set_occupied(oarr, i, false);
    }

    template<typename T, U64 element_count, typename O, typename I>
    constexpr T& get(OccupancyArray<T, element_count, O>& oarr, I i) {
        assert_true(is_occupied(oarr, i));
        return oarr.array[i];
    }

    template<typename T, U64 element_count, typename O, typename I>
    constexpr const T& get(const OccupancyArray<T, element_count, O>& oarr, I i) {
        assert_true(is_occupied(oarr, i));
        return oarr.array[i];
    }


    // ========================================================================
    // fixed slot map
    // @warn uses 0 as a sentinel value
    // ========================================================================
    template<typename K, typename V, U64 C>
    struct MapFixedSentinel {
        static_assert(C > 0);

        static constexpr K sentinel{0};
        Array<Pair<K, V>, C> key_values;

        MapFixedSentinel() = default;

        struct Iterator {
            MapFixedSentinel<K,V,C>* map;
            U64 slot_idx;

            Iterator(MapFixedSentinel<K,V,C>* map, U64 slot_idx) : map(map), slot_idx(slot_idx) {
                while (this->slot_idx < C && this->map->key_values[slot_idx].key == sentinel) {
                    slot_idx++;
                }
            }

            Iterator& operator++() {
                this->slot_idx++;
                while (this->slot_idx < C && this->map->key_values[slot_idx].key == sentinel) {
                    slot_idx++;
                }
                return *this; 
            }
            Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }

            Pair<K,V>& operator*() const { return this->map->key_values[this->slot_idx]; }

            bool operator==(const Iterator& other) const { return this->map == other.map && this->slot_idx == other.slot_idx; }
            bool operator!=(const Iterator& other) const { return !(*this == other); }
        };

        Iterator begin() noexcept { return Iterator{this, 0}; }
        Iterator end() noexcept { return Iterator{this, C}; }

        // @todo const iterator
    };

    template<typename K, typename V, U64 C>
    V* find(MapFixedSentinel<K,V,C>& map, const K& key) {
        assert_true(key != map.sentinel, "cannot find sentinel in map");

        U64 slot_idx = hash(key) % C;
        U64 end_slot_idx = (slot_idx + C - 1) % C;

        while (slot_idx != end_slot_idx) {
            auto& kv = map.key_values[slot_idx];
            if (kv.key == key) {
                return &kv.value;
            } else if (kv.key == map.sentinel) {
                return nullptr;
            }
            
            slot_idx = (slot_idx + 1) % C;
        }

        assert_true(false, "fixed sentinel map is full");
        return nullptr;
    }
    template<typename K, typename V, U64 C>
    V& insert(MapFixedSentinel<K,V,C>& map, const K& key) {
        assert_true(key != map.sentinel, "cannot insert sentinel into map");

        U64 slot_idx = hash(key) % C;
        U64 end_slot_idx = (slot_idx + C - 1) % C;

        while (slot_idx != end_slot_idx) {
            auto& kv = map.key_values[slot_idx];
            if (kv.key == map.sentinel) {
                kv.key = key;
                return kv.value;
            }

            slot_idx = (slot_idx + 1) % C;
        }

        assert_true(false, "fixed sentinel map is full");
        return map.key_values[0].value;
    }
    template<typename K, typename V, U64 C>
    bool try_remove(MapFixedSentinel<K,V,C>& map, const K& key) {
        assert_true(key != map.sentinel, "cannot remove sentinel from map");

        U64 slot_idx = hash(key) % C;
        U64 end_slot_idx = (slot_idx + C - 1) % C;

        while (slot_idx != end_slot_idx) {
            auto& kv = map.key_values[slot_idx];
            if (kv.key == key) {
                kv.key = map.sentinel;
                return true;
            } else if (kv.key == map.sentinel) {
                return false;
            }

            slot_idx = (slot_idx + 1) % C;
        }

        return false;
    }
    template<typename K, typename V, U64 C>
    void remove(MapFixedSentinel<K,V,C>& map, const K& key) {
        assert_true(try_remove(map, key) != false, "attempting to remove key which is not present");
    }
}