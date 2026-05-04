module plexdb.btree.detail;

namespace plexdb::btree {
    // ========================================================================
    // helpers
    // ========================================================================
    CountType max_keys(const Header& h, bool is_leaf) {
        return is_leaf ? h.max_keys_per_leaf : h.max_keys_per_internal;
    }

    CountType min_keys(const Header& h, bool is_leaf) {
        return (max_keys(h, is_leaf) + 1)/2 - 1;
    }

    // @todo remove unnecessary memory shift left -> right when removing then moving
    void move_from_left(Node* parent, Node* left, Node* node, CountType node_idx, const Header& h, bool is_leaf) {
        assert_true(node_idx > 0, "has left sibling");
        assert_true(left->key_count > 0, "left sibling has keys");
        // assert_true(left == children(parent, h)[node_idx-1] && node == children(parent, h)[node_idx], "valid argument");
        assert_true(node->key_count < max_keys(h, is_leaf), "enough space");

        os::memory_shift_up(keys(node));
        node->key_count++;
        
        if (is_leaf) {
            os::memory_shift_up(values(node, h));

            // move last key & value in LEFT to start of NODE
            CountType left_last = static_cast<CountType>(left->key_count - 1);
            os::memory_copy(values(node, h)[0], values(left, h)[left_last], h.value_stride);
            keys(node)[0] = keys(left)[left_last];
            keys(parent)[node_idx-1] = keys(left)[left_last];
        } else {
            os::memory_shift_up(children(node, h));

            // rotate last key in LEFT through parent to NODE
            // children(left) has length key_count+1, so index key_count is the last child
            children(node, h)[0] = children(left, h)[left->key_count];
            keys(node)[0] = keys(parent)[node_idx-1];
            keys(parent)[node_idx-1] = keys(left)[left->key_count - 1];
        }
        left->key_count--;
    }

    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx, const Header& h, bool is_leaf) {
        assert_true(node_idx < parent->key_count, "has right sibling");
        assert_true(right->key_count > 0, "right sibling has keys");
        // assert_true(right == children(parent, h)[node_idx+1] && node == children(parent, h)[node_idx], "valid argument");
        assert_true(node->key_count < max_keys(h, is_leaf), "enough space");
        
        node->key_count++;
        if (is_leaf) {
            // move first key & value in RIGHT to end of NODE
            os::memory_copy(values(node, h)[node->key_count-1], values(right, h)[0], h.value_stride);
            keys(node)[node->key_count-1] = keys(right)[0];
            keys(parent)[node_idx] = keys(right)[1];

            os::memory_shift_down(view_shift_up(values(right, h)));
        } else {
            // rotate first key in RIGHT through parent to NODE
            children(node, h)[node->key_count] = children(right, h)[0];
            keys(node)[node->key_count-1] = keys(parent)[node_idx];
            keys(parent)[node_idx] = keys(right)[0];

            os::memory_shift_down(view_shift_up(children(right, h)));
        }
        os::memory_shift_down(view_shift_up(keys(right)));
        right->key_count--;
    }

    void merge(const Header& h, Node* a, Node* b, Node* parent, CountType a_idx, bool is_leaf) {
        assert_true(a_idx < parent->key_count, "valid argument");
        assert_true(a->key_count + b->key_count <= max_keys(h, is_leaf), "enough space");

        if (!is_leaf) {
            assert_true(a->key_count < max_keys(h, is_leaf), "merging leaves requires space for extra key in a");
            a->key_count++;
            keys(a)[a->key_count-1] = keys(parent)[a_idx];
            os::memory_copy</*check_length*/false>(view_shift_up(children(a, h), a->key_count), children(b, h));
        } else {
            os::memory_copy</*check_length*/false>(view_shift_up(values(a, h), a->key_count), values(b, h));
        }
        os::memory_copy</*check_length*/false>(view_shift_up(keys(a), a->key_count), keys(b));
        a->key_count += b->key_count;
        a->next = b->next;
        
        // delete key from parent
        CountType b_idx = a_idx+1;
        os::memory_shift_down(view_shift_up(keys(parent), b_idx));
        os::memory_shift_down(view_shift_up(children(parent, h), static_cast<CountType>(b_idx+1)));
        parent->key_count--;
    }
    
    void delete_from_leaf(const Header& h, Node* node, CountType idx) {
        os::memory_shift_down(view_shift_up(keys(node), static_cast<CountType>(idx+1)));
        os::memory_shift_down(view_shift_up(values(node, h), static_cast<CountType>(idx+1)));
        node->key_count--;
    }
}