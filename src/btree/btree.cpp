module plexdb.btree;

import plexdb.os;
import plexdb.arena;

namespace plexdb::btree {
    BTreeInMemory::BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride):
        settings(max_keys_per_internal, max_keys_per_leaf, value_stride) {
        assert_true(max_keys_per_internal >= 3, "positive min keys.");
        assert_true(max_keys_per_leaf >= 3, "positive min keys.");

        leaves = make_leaf(settings);
        root = leaves;
    }

    void deallocate_tree(Node* node, CountType depth, CountType max_depth, const BTreeSettings& s) {
        if (depth == max_depth) {
            os::deallocate(node);
        } else {
            for (const auto& child : children(node, s))
                deallocate_tree(child, depth + 1, max_depth, s);
            os::deallocate(node);
        }
    }

    BTreeInMemory::~BTreeInMemory() {
        deallocate_tree(root, 0, depth, settings);
    }

    CountType max_keys(const BTreeSettings& s, bool is_leaf) {
        return is_leaf ? s.max_keys_per_leaf : s.max_keys_per_internal;
    }
    CountType min_keys(const BTreeSettings& s, bool is_leaf) {
        return (max_keys(s, is_leaf) + 1)/2 - 1;
    }

    // 
    // insert
    //

    // inserts a child to the right of the provided key index and add a new key
    void insert_child_to_right(Node* parent, CountType idx, KeyType key, Node* child, const BTreeSettings& s) {
        if (idx < parent->key_count) {
            os::memory_shift_right(view_shift_left(keys(parent), idx));
            os::memory_shift_right(view_shift_left(children(parent, s), static_cast<CountType>(idx+1)));
        }
        children(parent, s)[idx+1] = child;
        keys(parent)[idx] = key;
        parent->key_count++;

        // insert into doubly linked list
        Node* prev = children(parent, s)[idx];
        child->next = prev->next;
        child->prev = prev;
        prev->next = child;
    }

    // @precondition parent is not full
    void split_child(Node* parent, CountType child_idx, const BTreeSettings& s, bool is_child_leaf) {
        assert_true(parent->key_count < max_keys(s, false), "parent is not full");

        Node* left = children(parent, s)[child_idx];
        Node* right = is_child_leaf ? make_leaf(s) : make_internal(s);

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
        insert_child_to_right(parent, child_idx, is_child_leaf ? keys(right)[0] : keys(left)[t-1], right, s);
    }

    // @precondition leaf is not full
    template<bool avoid_duplicates=true>
    U8* insert_in_leaf(Node* leaf, KeyType key, const BTreeSettings& s, CountType& size) {
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
        size++;
        return values(leaf, s)[idx];
    }

    // @invariant n is not full
    U8* insert_recursive(Node* n, CountType depth, CountType max_depth, KeyType key, const BTreeSettings& s, CountType& size) {
        if (depth == max_depth)
            return insert_in_leaf(n, key, s, size);

        CountType child_idx = binary_search_first_gt(keys(n), key);
        bool is_child_leaf = depth + 1 == max_depth;
        Node* child = children(n, s)[child_idx];
        
        if (child->key_count == max_keys(s, is_child_leaf)) {
            split_child(n, child_idx, s, is_child_leaf);
            child_idx = (key > keys(n)[child_idx]) ? (child_idx+1) : child_idx;
        }
        assert_true(child->key_count < max_keys(s, is_child_leaf), "insert maintains max invariant.");

        return insert_recursive(children(n, s)[child_idx], depth+1, max_depth, key, s, size);
    }

    U8* insert(BTreeInMemory& btree, KeyType key) {
        const auto& s = btree.settings;

        // ensure root is not full
        bool is_root_leaf = btree.depth == 0;
        if (btree.root->key_count == max_keys(s, is_root_leaf)) {
            Node* new_root = make_internal(s);
            children(new_root, s)[0] = btree.root;
            split_child(new_root, 0, s, is_root_leaf);
            btree.root = new_root;
            btree.depth++;
        }

        return insert_recursive(btree.root, 0, btree.depth, key, btree.settings, btree.size);
    }

    void insert(BTreeInMemory& btree, KeyType key, U8* value) {
        U8* view = insert(btree, key);
        os::memory_copy(view, value, btree.settings.value_stride);
    }

    // 
    // search
    // 
    Search search(const BTreeInMemory& btree, KeyType key) {
        Node* node = btree.root;
        for (CountType depth = 0; depth < btree.depth; depth++) {
            CountType idx = binary_search_first_gt(keys(node), key);
            node = children(node, btree.settings)[idx];
        }
        
        CountType idx = binary_search_first_geq(keys(node), key);
        if (idx < node->key_count && keys(node)[idx] == key)
            return Search{node, idx, values(node, btree.settings)[idx]};
        return Search{};
    }

    // 
    // remove
    // 
    
    // @todo remove unnecessary memory shift left -> right when removing then moving
    void move_from_left(Node* parent, Node* left, Node* node, CountType node_idx, const BTreeSettings& s, bool is_leaf) {
        assert_true(node_idx > 0, "has left sibling");
        assert_true(left->key_count > 0, "left sibling has keys");
        assert_true(left == children(parent,s)[node_idx-1] && node == children(parent,s)[node_idx], "valid argument");
        assert_true(node->key_count < max_keys(s, is_leaf), "enough space");

        left->key_count--;
        os::memory_shift_right(keys(node));
        if (is_leaf) {
            os::memory_shift_right(values(node,s));

            // move last key & value in LEFT to start of NODE
            os::memory_copy(values(node,s)[0], values(left,s)[left->key_count], s.value_stride);
            keys(node)[0] = keys(left)[left->key_count];
            keys(parent)[node_idx-1] = keys(node)[0];
        } else {
            os::memory_shift_right(children(node,s));

            // rotate last key in LEFT through parent to NODE
            children(node,s)[0] = children(left,s)[left->key_count+1];
            keys(node)[0] = keys(parent)[node_idx-1];
            keys(parent)[node_idx-1] = keys(left)[left->key_count];
        }
        node->key_count++;
    }

    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx, const BTreeSettings& s, bool is_leaf) {
        assert_true(node_idx < parent->key_count, "has right sibling");
        assert_true(right->key_count > 0, "right sibling has keys");
        assert_true(right == children(parent,s)[node_idx+1] && node == children(parent,s)[node_idx], "valid argument");
        assert_true(node->key_count < max_keys(s, is_leaf), "enough space");
        
        if (is_leaf) {
            // move first key & value in RIGHT to end of NODE
            os::memory_copy(values(node,s)[node->key_count], values(right,s)[0], s.value_stride);
            keys(node)[node->key_count] = keys(right)[0];
            keys(parent)[node_idx] = keys(right)[1];

            os::memory_shift_left(view_shift_left(values(right,s)));
        } else {
            // rotate first key in RIGHT through parent to NODE
            children(node,s)[node->key_count+1] = children(right,s)[0];
            keys(node)[node->key_count] = keys(parent)[node_idx];
            keys(parent)[node_idx] = keys(right)[0];

            os::memory_shift_left(view_shift_left(children(right,s)));
        }
        os::memory_shift_left(view_shift_left(keys(right)));
        right->key_count--;
        node->key_count++;
    }

    void merge(Node* a, Node* b, Node* parent, CountType a_idx, const BTreeSettings& s, bool is_leaf) {
        assert_true(a_idx < parent->key_count, "valid argument");
        assert_true(children(parent,s)[a_idx] == a, "valid argument");
        assert_true(children(parent,s)[a_idx+1] == b, "valid argument");
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
        os::deallocate(b);
        
        // delete key from parent
        CountType b_idx = a_idx+1;
        os::memory_shift_left(view_shift_left(keys(parent), b_idx));
        os::memory_shift_left(view_shift_left(children(parent,s), static_cast<CountType>(b_idx+1)));
        parent->key_count--;
    }

    struct RemoveStackItem {
        Node* node;
        CountType idx;
    };

    bool remove(BTreeInMemory& btree, KeyType key) {
        const auto& s = btree.settings;

        // traverse to leaf and store path
        Stack<RemoveStackItem> stack{};
        // @todo @perf temp arena
        Arena stack_arena{/*page_size*/ sizeof(decltype(stack)::Node)*btree.depth};
        Node* node = btree.root;
        for (CountType depth = 0; depth < btree.depth; depth++) {
            CountType idx = binary_search_first_gt(keys(node), key);
            push_front(stack_arena, stack, RemoveStackItem{node, idx});
            node = children(node,s)[idx];
        }

        // find and delete key
        CountType idx = binary_search_first_geq(keys(node), key);
        if (idx >= node->key_count || keys(node)[idx] != key)
            return false;
        os::memory_shift_left(view_shift_left(keys(node), static_cast<CountType>(idx+1)));
        os::memory_shift_left(view_shift_left(values(node, s), static_cast<CountType>(idx+1)));
        node->key_count--;

        // fix underflow, propagating upwards as needed
        bool is_leaf = true;
        while (node->key_count < min_keys(s, is_leaf)) {
            if (stack.length == 0) {
                if (node->key_count != 0 || btree.depth == 0)
                    break;

                btree.root = children(node,s)[0];
                os::deallocate(node);
                btree.depth--;
                break;
            }

            RemoveStackItem* item = front(stack); pop_front(stack);
            Node* parent = item->node;
            CountType idx = item->idx;

            Node* left = (idx > 0) ? children(parent,s)[idx-1] : nullptr;
            Node* right = (idx < parent->key_count) ? children(parent,s)[idx+1] : nullptr;
            assert_true(left || right, "node must have at least one sibling.");

            // borrow from left or right sibling if it does not break invariant
            if (left && left->key_count > min_keys(s, is_leaf)) {
                move_from_left(parent, left, node, idx, s, is_leaf);
                break;
            }
            else if (right && right->key_count > min_keys(s, is_leaf)) {
                move_from_right(parent, right, node, idx, s, is_leaf);
                break;
            }

            // otherwise we need to merge, this causes a delete in the parent 
            // which needs to be propagated
            if (left) {
                merge(left, node, parent, idx-1, s, is_leaf);
            } else {
                merge(node, right, parent, idx, s, is_leaf);
            }

            node = parent;
            is_leaf = false;
        }

        btree.size--;
        return true;
    }
}