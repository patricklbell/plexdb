export module plexdb.base.containers;

import plexdb.base.types;

export namespace plexdb {
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
    struct FixedSlotMap {
        using Node = Stack<Pair<K,V>>::Node;

        Stack<Pair<K, V>>* slots;
        U64 slot_count;
        U64 length = 0;

        FixedSlotMap(Stack<Pair<K, V>>* slots, U64 slot_count) : slots(slots), slot_count(slot_count) {}

        struct Iterator {
            FixedSlotMap<K,V>* map;
            U64 slot_idx;
            Node* node;

            Iterator(FixedSlotMap<K,V>* map, U64 slot_idx, Node* node) : map(map), slot_idx(slot_idx), node(node) {}

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

            Pair<K,V> operator*() const { return node->value; }

            bool operator==(const Iterator& other) const { return node == other.node; }
            bool operator!=(const Iterator& other) const { return !(*this == other); }
        };

        Iterator begin() noexcept { return Iterator{this, 0, front(slots)}; }
        Iterator end() noexcept { return Iterator{this, slots->slot_count-1, nullptr}; }

        // @todo const iterator
    };

    template<typename K, typename V>
    V* find(FixedSlotMap<K, V>& map, const K& key) {
        U64 slot_idx = hash(key)%map.slot_count;

        for(auto& pair : map.slots[slot_idx]) {
            if (key == pair.key)
                return &pair.value;
        }
        return nullptr;
    }
    template<typename K, typename V>
    void insert(FixedSlotMap<K,V>& map, typename FixedSlotMap<K,V>::Node* node) {
        U64 slot_idx = hash(node->key)%map.slot_count;
        push_front(map.slots[slot_idx], node);
        map.length++;
    }
    template<typename K, typename V>
    void clear(FixedSlotMap<K,V>& map) {
        for(U64 slot_idx = 0; slot_idx < map.slot_count; slot_idx++) {
            clear(map.slots[slot_idx]);
        }
    }
}