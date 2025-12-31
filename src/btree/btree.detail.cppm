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
    CountType max_keys(const Header& h, bool is_leaf);
    CountType min_keys(const Header& h, bool is_leaf);
    void move_from_left(Node* parent, Node* left, Node* node, CountType node_idx, const Header& h, bool is_leaf);
    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx, const Header& h, bool is_leaf);
    void merge(const Header& h, Node* a, Node* b, Node* parent, CountType a_idx, bool is_leaf);
    void delete_from_leaf(const Header& h, Node* node, CountType idx);

    // ========================================================================
    // insert
    // ========================================================================
    template<typename Transaction>
    void insert_child_to_right(Transaction& t, const Header& h, Node* parent, CountType idx, KeyType key, NodeRef child_ref, Node* child) {
        if (idx < parent->key_count) {
            os::memory_shift_right(view_shift_left(keys(parent), idx));
            os::memory_shift_right(view_shift_left(children(parent, h), static_cast<CountType>(idx+1)));
        }
        children(parent, h)[idx+1] = child_ref;
        keys(parent)[idx] = key;
        parent->key_count++;

        // insert into doubly linked list
        NodeRef prev_ref = children(parent, h)[idx];
        Node* prev = rwnode(t, prev_ref);
        child->next = prev->next;
        child->prev = prev_ref;
        prev->next = child_ref;
    }

    // @precondition parent is not full
    template<typename Transaction>
    void split_child(Transaction& t, const Header& h, Node* parent, CountType child_idx, bool is_child_leaf) {
        assert_true(parent->key_count < max_keys(h, false), "parent is not full");

        Node* left = rwnode(t, children(parent, h)[child_idx]);
        NodeRef right_ref = is_child_leaf ? new_leaf(t) : new_internal(t);
        Node* right = rwnode(t, right_ref);

        // copy right half of LEFT's keys+children to RIGHT
        CountType m = (left->key_count+1)/2; // ceil(middle)
        right->key_count = left->key_count - m;
        os::memory_copy(keys(right), view_shift_left(keys(left), m));
        if (is_child_leaf)
            os::memory_copy(values(right, h), view_shift_left(values(left, h), m));
        else
            os::memory_copy(children(right, h), view_shift_left(children(left, h), m));
        // @note for internal nodes we drop the separating key
        left->key_count = is_child_leaf ? m : (m-1);

        // insert RIGHT into PARENT with separating key
        KeyType sep_key = is_child_leaf ? keys(right)[0] : keys(left)[m-1];
        insert_child_to_right(t, h, parent, child_idx, sep_key, right_ref, right);
    }

    // @precondition leaf is not full
    template<bool avoid_duplicates=true>
    U8* insert_in_leaf(Header& h, Node* leaf, KeyType key) {
        assert_true(leaf->key_count < max_keys(h, true), "leaf is not full");

        auto ks = keys(leaf);
        CountType idx = 0;
        if (likely(leaf->key_count > 0)) {
            if constexpr (avoid_duplicates) {
                idx = binary_search_first_geq(keys(leaf), key);
                if (idx < leaf->key_count && keys(leaf)[idx] == key)
                    return values(leaf, h)[idx];
                os::memory_shift_right(view_shift_left(keys(leaf), idx));
            } else {
                idx = leaf->key_count - 1;
                for (; idx >= 0 && key < ks[idx]; idx--)
                    ks[idx+1] = ks[idx];    
                idx++;
            }
            os::memory_shift_right(view_shift_left(values(leaf, h), idx));
        }

        ks[idx] = key;
        leaf->key_count++;
        h.size++;
        return values(leaf, h)[idx];
    }

    // @invariant n is not full
    template<typename Transaction>
    U8* insert_recursive(Transaction& t, const Header& h, NodeRef n_ref, CountType depth, KeyType key) {
        // leaf
        if (depth == h.depth) {
            // acquire lock on returned memory
            Node* n = rwnode(t, n_ref);

            auto t_leaf = scope(t);
            Header& h = *rwheader(t_leaf);
            return insert_in_leaf(h, n, key);
        }

        NodeRef child_ref;
        bool is_child_leaf = (depth + 1) == h.depth;

        // internal overflow
        {
            auto t_int = scope(t);

            const Node* n = rnode(t_int, n_ref);
            CountType child_idx = binary_search_first_gt(keys(n), key);
            child_ref = children(n, h)[child_idx];
            const Node* child = rnode(t_int, child_ref);
            
            if (child->key_count == max_keys(h, is_child_leaf)) {
                split_child(t_int, h, rwnode(t_int, n_ref), child_idx, is_child_leaf);
    
                child_idx = (key > keys(n)[child_idx]) ? (child_idx+1) : child_idx;
                child_ref = children(n, h)[child_idx];
            }
            
            assert_true(child->key_count < max_keys(h, is_child_leaf), "insert maintains max invariant.");
        }
        return insert_recursive(t, h, child_ref, depth+1, key);
    }

    template<typename Transaction>
    U8* insert_impl(Transaction& t, KeyType key) {
        // acquire read on header for duration of insertion
        auto t_header = scope(t);
        const auto& h = *rheader(t_header);
        
        // ensure root is not full
        bool is_root_leaf = h.depth == 0;
        {
            auto t_root = scope(t);
            const Node* root = rnode(t_root, h.root);

            if (root->key_count == max_keys(h, is_root_leaf)) {
                NodeRef new_root_ref = new_internal(t_root);
                Node* new_root = rwnode(t_root, new_root_ref);
                children(new_root, h)[0] = h.root;
                split_child(t_root, h, new_root, 0, is_root_leaf);
                
                auto& h_write = *rwheader(t_root);
                h_write.root = new_root_ref;
                h_write.depth++;
            }
        }

        return insert_recursive(t, h, h.root, 0, key);
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

    template<typename Transaction>
    Search search_impl(Transaction& t, KeyType key) {
        // acquire read on header for duration of search
        // @todo check necessary for duration since depth changing may not break search
        auto t_header = scope(t);
        const auto& h = *rheader(t_header);

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            auto t_node = scope(t);
            const Node* n = rnode(t_node, n_ref);
            CountType idx = binary_search_first_gt(keys(n), key);
            n_ref = children(n, h)[idx];
        }
        
        // acquire lock on returned memory
        // @note still acquires lock if search fails, assuming transaction will 
        // be ended
        Node* n = rwnode(t, n_ref);
        CountType idx = binary_search_first_geq(keys(n), key);
        if (idx < n->key_count && keys(n)[idx] == key)
            return Search{n, idx, values(n, h)[idx]};
        return Search{};
    }

    // ========================================================================
    // remove
    // ========================================================================
    struct RemoveStackItem {
        NodeRef node;
        CountType idx;
    };

    template<typename Transaction>
    bool remove_impl(Transaction& t, KeyType key) {
        auto t_remove = scope(t);

        // acquire read on header for duration of remove
        const auto& h = *rheader(t_remove);
        
        // @todo check. strategy is to release read locks acquired during
        // down traversal and acquire rw as needed when flowing up on the 
        // assumption being that flowing up is rare
        
        // traverse to leaf and store path
        Stack<RemoveStackItem> stack{};
        // @todo @perf temp arena
        Arena stack_arena{/*page_size*/ sizeof(decltype(stack)::Node)*h.depth};
        NodeRef node_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            auto t_down = scope(t_remove);

            const Node* node = rnode(t_down, node_ref);
            CountType idx = binary_search_first_gt(keys(node), key);
            push_front(stack_arena, stack, RemoveStackItem{node_ref, idx});
            node_ref = children(node, h)[idx];
        }

        // r/w lifetimes are somewhat complicated so for now everything is in one
        // transaction, again on the assumption that propogation is not deep

        // find and delete key
        CountType idx;
        {
            const Node* node = rnode(t_remove, node_ref);
            idx = binary_search_first_geq(keys(node), key);
            if (idx >= node->key_count || keys(node)[idx] != key)
                return false;
        }

        Node* node = rwnode(t_remove, node_ref);
        delete_from_leaf(h, node, idx);
        
        // fix underflow, propagating upwards as needed
        bool is_leaf = true;
        while (node->key_count < min_keys(h, is_leaf)) {
            if (stack.length == 0) {
                if (node->key_count != 0 || h.depth == 0)
                    break;

                delete_node(t_remove, node_ref);

                Header& h_write = *rwheader(t_remove);
                h_write.root = children(node, h)[0];
                h_write.depth--;
                h_write.size--;
                return true; // @note, avoids reacquiring header, may not be necessary
            }

            RemoveStackItem* item = front(stack); pop_front(stack);
            NodeRef& parent_ref = item->node;
            CountType& idx = item->idx;
            Node* parent = rwnode(t_remove, parent_ref);

            // borrow from left or right sibling if it does not break invariant
            NodeRef left_ref = (idx > 0) ? children(parent,h)[idx-1] : ~0u;
            Node* left = (left_ref != ~0u) ? rwnode(t_remove, left_ref) : nullptr;
            if (left && left->key_count > min_keys(h, is_leaf)) {
                move_from_left(parent, left, node, idx, h, is_leaf);
                break;
            }
            NodeRef right_ref = (idx < parent->key_count) ? children(parent,h)[idx+1] : ~0u;
            Node* right = (right_ref != ~0u) ? rwnode(t_remove, right_ref) : nullptr;
            if (right && right->key_count > min_keys(h, is_leaf)) {
                move_from_right(parent, right, node, idx, h, is_leaf);
                break;
            }

            assert_true(left || right, "node must have at least one sibling.");

            // otherwise we need to merge, this causes a delete in the parent 
            // which needs to be propagated
            if (left) {
                merge(h, left, node, parent, idx-1, is_leaf);
                delete_node(t, node_ref);
            } else {
                merge(h, node, right, parent, idx, is_leaf);
                delete_node(t, right_ref);
            }

            node_ref = parent_ref;
            node = parent;
            is_leaf = false;
        }
        // @note loop contains a return

        rwheader(t_remove)->size--;
        return true;
    }
}