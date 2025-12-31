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

        left->key_count--;
        os::memory_shift_right(keys(node));
        if (is_leaf) {
            os::memory_shift_right(values(node, h));

            // move last key & value in LEFT to start of NODE
            os::memory_copy(values(node, h)[0], values(left, h)[left->key_count], h.value_stride);
            keys(node)[0] = keys(left)[left->key_count];
            keys(parent)[node_idx-1] = keys(node)[0];
        } else {
            os::memory_shift_right(children(node, h));

            // rotate last key in LEFT through parent to NODE
            children(node, h)[0] = children(left, h)[left->key_count+1];
            keys(node)[0] = keys(parent)[node_idx-1];
            keys(parent)[node_idx-1] = keys(left)[left->key_count];
        }
        node->key_count++;
    }

    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx, const Header& h, bool is_leaf) {
        assert_true(node_idx < parent->key_count, "has right sibling");
        assert_true(right->key_count > 0, "right sibling has keys");
        // assert_true(right == children(parent, h)[node_idx+1] && node == children(parent, h)[node_idx], "valid argument");
        assert_true(node->key_count < max_keys(h, is_leaf), "enough space");
        
        if (is_leaf) {
            // move first key & value in RIGHT to end of NODE
            os::memory_copy(values(node, h)[node->key_count], values(right, h)[0], h.value_stride);
            keys(node)[node->key_count] = keys(right)[0];
            keys(parent)[node_idx] = keys(right)[1];

            os::memory_shift_left(view_shift_left(values(right, h)));
        } else {
            // rotate first key in RIGHT through parent to NODE
            children(node, h)[node->key_count+1] = children(right, h)[0];
            keys(node)[node->key_count] = keys(parent)[node_idx];
            keys(parent)[node_idx] = keys(right)[0];

            os::memory_shift_left(view_shift_left(children(right, h)));
        }
        os::memory_shift_left(view_shift_left(keys(right)));
        right->key_count--;
        node->key_count++;
    }

    void merge(const Header& h, Node* a, Node* b, Node* parent, CountType a_idx, bool is_leaf) {
        assert_true(a_idx < parent->key_count, "valid argument");
        assert_true(a->key_count + b->key_count <= max_keys(h, is_leaf), "enough space");

        if (!is_leaf) {
            keys(a)[a->key_count] = keys(parent)[a_idx];
            os::memory_copy</*check_length*/false>(view_shift_left(children(a, h), static_cast<CountType>(a->key_count+1)), children(b, h));
            a->key_count++;
        } else {
            os::memory_copy</*check_length*/false>(view_shift_left(values(a, h), a->key_count), values(b, h));
        }
        os::memory_copy</*check_length*/false>(view_shift_left(keys(a), a->key_count), keys(b));
        a->key_count += b->key_count;
        a->next = b->next;
        
        // delete key from parent
        CountType b_idx = a_idx+1;
        os::memory_shift_left(view_shift_left(keys(parent), b_idx));
        os::memory_shift_left(view_shift_left(children(parent, h), static_cast<CountType>(b_idx+1)));
        parent->key_count--;
    }
    
    void delete_from_leaf(const Header& h, Node* node, CountType idx) {
        os::memory_shift_left(view_shift_left(keys(node), static_cast<CountType>(idx+1)));
        os::memory_shift_left(view_shift_left(values(node, h), static_cast<CountType>(idx+1)));
        node->key_count--;
    }
}