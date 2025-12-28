module plexdb.btree.detail;

namespace plexdb::btree {
    // ========================================================================
    // helpers
    // ========================================================================
    CountType max_keys(const Settings& s, bool is_leaf) {
        return is_leaf ? s.max_keys_per_leaf : s.max_keys_per_internal;
    }

    CountType min_keys(const Settings& s, bool is_leaf) {
        return (max_keys(s, is_leaf) + 1)/2 - 1;
    }

    // @todo remove unnecessary memory shift left -> right when removing then moving
    void move_from_left(Node* parent, Node* left, Node* node, CountType node_idx, const Settings& s, bool is_leaf) {
        assert_true(node_idx > 0, "has left sibling");
        assert_true(left->key_count > 0, "left sibling has keys");
        // assert_true(left == children(parent,s)[node_idx-1] && node == children(parent,s)[node_idx], "valid argument");
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

    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx, const Settings& s, bool is_leaf) {
        assert_true(node_idx < parent->key_count, "has right sibling");
        assert_true(right->key_count > 0, "right sibling has keys");
        // assert_true(right == children(parent,s)[node_idx+1] && node == children(parent,s)[node_idx], "valid argument");
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
}