export module plexdb.btree.detail;

export import plexdb.btree.types;
export import plexdb.btree.in_memory.detail;
export import plexdb.btree.paged.detail;
export import plexdb.btree.node;

import plexdb.base;
import plexdb.os;
import plexdb.arena;

export namespace plexdb::btree {
    // ========================================================================
    // helpers
    // ========================================================================
    CountType max_keys(const Settings& s, bool is_leaf);
    CountType min_keys(const Settings& s, bool is_leaf);
    void move_from_left(Node* parent, Node* left, Node* node, CountType node_idx, const Settings& s, bool is_leaf);
    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx, const Settings& s, bool is_leaf);
    void merge(Node* a, Node* b, Node* parent, CountType a_idx, const Settings& s, bool is_leaf);

    template<typename BTree>
    void insert_child_to_right(BTree& btree, Node* parent, CountType idx, KeyType key, NodeRef child_ref, Node* child, const Settings& s) {
        if (idx < parent->key_count) {
            os::memory_shift_right(view_shift_left(keys(parent), idx));
            os::memory_shift_right(view_shift_left(children(parent, s), static_cast<CountType>(idx+1)));
        }
        children(parent, s)[idx+1] = child_ref;
        keys(parent)[idx] = key;
        parent->key_count++;

        // insert into doubly linked list
        NodeRef prev_ref = children(parent, s)[idx];
        Node* prev = rwnode(btree, prev_ref);
        child->next = prev->next;
        child->prev = prev_ref;
        prev->next = child_ref;
    }

    // ========================================================================
    // insert
    // ========================================================================
    // @precondition parent is not full
    template<typename BTree>
    void split_child(BTree& btree, Node* parent, CountType child_idx, bool is_child_leaf) {
        auto& s = get_settings(btree);
        assert_true(parent->key_count < max_keys(s, false), "parent is not full");

        Node* left = rwnode(btree, children(parent, s)[child_idx]);
        NodeRef right_ref = is_child_leaf ? create_leaf(btree) : create_internal(btree);
        Node* right = rwnode(btree, right_ref);

        // copy right half of LEFT's keys+children to RIGHT
        CountType t = (left->key_count+1)/2; // ceil(middle)
        right->key_count = left->key_count - t;
        os::memory_copy(keys(right), view_shift_left(keys(left), t));
        if (is_child_leaf)
            os::memory_copy(values(right, s), view_shift_left(values(left, s), t));
        else
            os::memory_copy(children(right, s), view_shift_left(children(left, s), t));
        // @note for internal nodes we drop the separating key
        left->key_count = is_child_leaf ? t : (t-1);

        // insert RIGHT into PARENT with separating key
        KeyType sep_key = is_child_leaf ? keys(right)[0] : keys(left)[t-1];
        insert_child_to_right(btree, parent, child_idx, sep_key, right_ref, right, s);
    }

    // @precondition leaf is not full
    template<bool avoid_duplicates=true>
    U8* insert_in_leaf(Node* leaf, KeyType key, Settings& s) {
        assert_true(leaf->key_count < max_keys(s, true), "leaf is not full");

        auto ks = keys(leaf);
        CountType idx = 0;
        if (likely(leaf->key_count > 0)) {
            if constexpr (avoid_duplicates) {
                idx = binary_search_first_geq(keys(leaf), key);
                if (idx < leaf->key_count && keys(leaf)[idx] == key)
                    return values(leaf, s)[idx];
                os::memory_shift_right(view_shift_left(keys(leaf), idx));
            } else {
                idx = leaf->key_count - 1;
                for (; idx >= 0 && key < ks[idx]; idx--)
                    ks[idx+1] = ks[idx];    
                idx++;
            }
            os::memory_shift_right(view_shift_left(values(leaf, s), idx));
        }

        ks[idx] = key;
        leaf->key_count++;
        s.size++;
        return values(leaf, s)[idx];
    }

    // @invariant n is not full
    template<typename BTree>
    U8* insert_recursive(BTree& btree, Node* n, CountType depth, KeyType key) {
        auto& s = get_settings(btree);
        if (depth == s.depth)
            return insert_in_leaf(n, key, s);

        CountType child_idx = binary_search_first_gt(keys(n), key);
        bool is_child_leaf = (depth + 1) == s.depth;
        Node* child = rwnode(btree, children(n, s)[child_idx]);
        
        if (child->key_count == max_keys(s, is_child_leaf)) {
            split_child(btree, n, child_idx, is_child_leaf);

            child_idx = (key > keys(n)[child_idx]) ? (child_idx+1) : child_idx;
            child = rwnode(btree, children(n, s)[child_idx]);
        }
        
        assert_true(child->key_count < max_keys(s, is_child_leaf), "insert maintains max invariant.");
        return insert_recursive(btree, child, depth+1, key);
    }

    template<typename BTree>
    U8* insert_impl(BTree& btree, KeyType key) {
        auto& s = get_settings(btree);
        
        // ensure root is not full
        Node* root = rwnode(btree, get_root(btree));

        bool is_root_leaf = s.depth == 0;
        if (root->key_count == max_keys(s, is_root_leaf)) {
            NodeRef new_root_ref = create_internal(btree);
            Node* new_root = rwnode(btree, new_root_ref);
            children(new_root, s)[0] = get_root(btree);
            split_child(btree, new_root, 0, is_root_leaf);
            
            set_root(btree, new_root_ref);
            root = new_root;
            s.depth++;
        }

        return insert_recursive(btree, root, 0, key);
    }

    // ========================================================================
    // search
    // ========================================================================
    struct Search {
        Node* leaf = nullptr;
        CountType idx;
        U8* value;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return leaf != nullptr; }
    };

    template<typename BTree>
    Search search_impl(BTree& btree, KeyType key) {
        const auto& s = get_settings(btree);

        Node* node = rnode(btree, get_root(btree));
        for (CountType depth = 0; depth < s.depth; depth++) {
            CountType idx = binary_search_first_gt(keys(node), key);
            node = rnode(btree, children(node, s)[idx]);
        }
        
        CountType idx = binary_search_first_geq(keys(node), key);
        if (idx < node->key_count && keys(node)[idx] == key)
            return Search{node, idx, values(node, s)[idx]};
        return Search{};
    }

    // ========================================================================
    // remove
    // ========================================================================
    template<typename BTree>
    void merge(BTree btree, Node* a, NodeRef b_ref, Node* b, Node* parent, CountType a_idx, bool is_leaf) {
        const auto& s = get_settings(btree);

        assert_true(a_idx < parent->key_count, "valid argument");
        // assert_true(children(parent,s)[a_idx] == a, "valid argument");
        // assert_true(children(parent,s)[a_idx+1] == b, "valid argument");
        assert_true(a->key_count + b->key_count <= max_keys(s, is_leaf), "enough space");

        if (!is_leaf) {
            keys(a)[a->key_count] = keys(parent)[a_idx];
            os::memory_copy</*check_length*/false>(view_shift_left(children(a,s), static_cast<CountType>(a->key_count+1)), children(b,s));
            a->key_count++;
        } else {
            os::memory_copy</*check_length*/false>(view_shift_left(values(a,s), a->key_count), values(b,s));
        }
        os::memory_copy</*check_length*/false>(view_shift_left(keys(a), a->key_count), keys(b));
        a->key_count += b->key_count;
        a->next = b->next;
        delete_node(btree, b_ref);
        
        // delete key from parent
        CountType b_idx = a_idx+1;
        os::memory_shift_left(view_shift_left(keys(parent), b_idx));
        os::memory_shift_left(view_shift_left(children(parent,s), static_cast<CountType>(b_idx+1)));
        parent->key_count--;
    }

    struct RemoveStackItem {
        NodeRef node;
        CountType idx;
    };

    template<typename BTree>
    bool remove_impl(BTree& btree, KeyType key) {
        auto& s = get_settings(btree);

        // traverse to leaf and store path
        Stack<RemoveStackItem> stack{};
        // @todo @perf temp arena
        // @todo hold pages in transaction
        Arena stack_arena{/*page_size*/ sizeof(decltype(stack)::Node)*s.depth};
        NodeRef node_ref = get_root(btree);
        Node* node = rnode(btree, node_ref);
        for (CountType depth = 0; depth < s.depth; depth++) {
            CountType idx = binary_search_first_gt(keys(node), key);

            push_front(stack_arena, stack, RemoveStackItem{node_ref, idx});
            
            node_ref = children(node,s)[idx];
            node = rnode(btree, node_ref);
        }

        // find and delete key
        CountType idx = binary_search_first_geq(keys(node), key);
        if (idx >= node->key_count || keys(node)[idx] != key)
            return false;

        node = rwnode(btree, node_ref);
        os::memory_shift_left(view_shift_left(keys(node), static_cast<CountType>(idx+1)));
        os::memory_shift_left(view_shift_left(values(node, s), static_cast<CountType>(idx+1)));
        node->key_count--;

        // fix underflow, propagating upwards as needed
        bool is_leaf = true;
        while (node->key_count < min_keys(s, is_leaf)) {
            if (stack.length == 0) {
                if (node->key_count != 0 || s.depth == 0)
                    break;

                set_root(btree, children(node,s)[0]);
                delete_node(btree, node_ref);
                s.depth--;
                break;
            }

            RemoveStackItem* item = front(stack); pop_front(stack);
            NodeRef parent_ref = item->node;
            Node* parent = rwnode(btree, parent_ref);
            CountType idx = item->idx;

            // borrow from left or right sibling if it does not break invariant
            NodeRef left_ref = (idx > 0) ? children(parent,s)[idx-1] : ~0u;
            Node* left = (left_ref != ~0u) ? rwnode(btree, left_ref) : nullptr;
            if (left && left->key_count > min_keys(s, is_leaf)) {
                move_from_left(parent, left, node, idx, s, is_leaf);
                break;
            }
            NodeRef right_ref = (idx < parent->key_count) ? children(parent,s)[idx+1] : ~0u;
            Node* right = (right_ref != ~0u) ? rwnode(btree, right_ref) : nullptr;
            if (right && right->key_count > min_keys(s, is_leaf)) {
                move_from_right(parent, right, node, idx, s, is_leaf);
                break;
            }

            assert_true(left || right, "node must have at least one sibling.");

            // otherwise we need to merge, this causes a delete in the parent 
            // which needs to be propagated
            if (left) {
                merge(btree, left, node_ref, node, parent, idx-1, is_leaf);
            } else {
                merge(btree, node, right_ref, right, parent, idx, is_leaf);
            }

            node_ref = parent_ref;
            node = parent;
            is_leaf = false;
        }

        s.size--;
        return true;
    }
}