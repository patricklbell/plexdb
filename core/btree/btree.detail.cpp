module plexdb.btree.detail;

namespace plexdb::btree {
    // ========================================================================
    // helpers
    // ========================================================================
    CountType max_keys(U32 node_size, U16 val_stride, bool is_leaf) {
        return is_leaf ? max_keys_leaf(node_size, val_stride)
                       : max_keys_internal(node_size);
    }

    CountType min_keys(U32 node_size, U16 val_stride, bool is_leaf) {
        return static_cast<CountType>((max_keys(node_size, val_stride, is_leaf) + 1) / 2 - 1);
    }

    void move_from_left(Node* parent, Node* left, Node* node, CountType node_idx,
                        U32 ns, U16 vs, bool is_leaf) {
        assert_true(node_idx > 0, "has left sibling");
        assert_true(left->key_count > 0, "left sibling has keys");
        assert_true(node->key_count < max_keys(ns, vs, is_leaf), "enough space");

        os::memory_shift_up(keys(node));
        node->key_count++;

        if (is_leaf) {
            os::memory_shift_up(values(node, ns, vs));
            CountType left_last = static_cast<CountType>(left->key_count - 1);
            os::memory_copy(values(node, ns, vs)[0], values(left, ns, vs)[left_last], vs);
            keys(node)[0] = keys(left)[left_last];
            keys(parent)[node_idx-1] = keys(left)[left_last];
        } else {
            os::memory_shift_up(children(node, ns));
            children(node, ns)[0] = children(left, ns)[left->key_count];
            keys(node)[0] = keys(parent)[node_idx-1];
            keys(parent)[node_idx-1] = keys(left)[left->key_count - 1];
        }
        left->key_count--;
    }

    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx,
                         U32 ns, U16 vs, bool is_leaf) {
        assert_true(node_idx < parent->key_count, "has right sibling");
        assert_true(right->key_count > 0, "right sibling has keys");
        assert_true(node->key_count < max_keys(ns, vs, is_leaf), "enough space");

        node->key_count++;
        if (is_leaf) {
            os::memory_copy(values(node, ns, vs)[node->key_count-1], values(right, ns, vs)[0], vs);
            keys(node)[node->key_count-1] = keys(right)[0];
            keys(parent)[node_idx] = keys(right)[1];
            os::memory_shift_down(view_shift_up(values(right, ns, vs)));
        } else {
            children(node, ns)[node->key_count] = children(right, ns)[0];
            keys(node)[node->key_count-1] = keys(parent)[node_idx];
            keys(parent)[node_idx] = keys(right)[0];
            os::memory_shift_down(view_shift_up(children(right, ns)));
        }
        os::memory_shift_down(view_shift_up(keys(right)));
        right->key_count--;
    }

    void merge(Node* a, Node* b, Node* parent, CountType a_idx,
               U32 ns, U16 vs, bool is_leaf) {
        assert_true(a_idx < parent->key_count, "valid argument");
        assert_true(a->key_count + b->key_count <= max_keys(ns, vs, is_leaf), "enough space");

        if (!is_leaf) {
            assert_true(a->key_count < max_keys(ns, vs, is_leaf),
                        "merging internal nodes requires space for extra key in a");
            a->key_count++;
            keys(a)[a->key_count-1] = keys(parent)[a_idx];
            os::memory_copy<false>(view_shift_up(children(a, ns), a->key_count), children(b, ns));
        } else {
            os::memory_copy<false>(view_shift_up(values(a, ns, vs), a->key_count), values(b, ns, vs));
        }
        os::memory_copy<false>(view_shift_up(keys(a), a->key_count), keys(b));
        a->key_count += b->key_count;
        a->next = b->next;

        CountType b_idx = static_cast<CountType>(a_idx + 1);
        os::memory_shift_down(view_shift_up(keys(parent), b_idx));
        os::memory_shift_down(view_shift_up(children(parent, ns), static_cast<CountType>(b_idx+1)));
        parent->key_count--;
    }

    void delete_from_leaf(Node* node, U32 ns, U16 vs, CountType idx) {
        os::memory_shift_down(view_shift_up(keys(node), static_cast<CountType>(idx+1)));
        os::memory_shift_down(view_shift_up(values(node, ns, vs), static_cast<CountType>(idx+1)));
        node->key_count--;
    }
}
