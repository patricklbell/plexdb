module;
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

export module plexdb.btree.detail;

export import plexdb.btree.types;
export import plexdb.btree.in_memory.detail;
export import plexdb.btree.paged.detail;
export import plexdb.btree.node;

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.coroutine;

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
    template<Transaction Tx>
    coroutine::Task<void> insert_child_to_right(Tx& t, const Header& h, Node* parent, CountType idx, KeyType key, NodeRef child_ref, Node* child) {
        if (idx < parent->key_count) {
            os::memory_shift_up(view_shift_up(keys(parent), idx));
            os::memory_shift_up(view_shift_up(children(parent, h), static_cast<CountType>(idx+1)));
        }
        parent->key_count++;
        children(parent, h)[idx+1] = child_ref;
        keys(parent)[idx] = key;

        // insert into doubly linked list
        NodeRef prev_ref = children(parent, h)[idx];
        Node* prev = co_await update_node(t, prev_ref);
        child->next = prev->next;
        child->prev = prev_ref;
        prev->next = child_ref;
    }

    // @precondition parent is not full
    template<Transaction Tx>
    coroutine::Task<void> split_child(Tx& t, const Header& h, Node* parent, CountType child_idx, bool is_child_leaf) { ZoneScopedN("btree::split");
        assert_true(parent->key_count < max_keys(h, false), "parent is not full");

        Node* left = co_await update_node(t, children(parent, h)[child_idx]);
        NodeRef right_ref;
        if (is_child_leaf) right_ref = co_await create_leaf(t);
        else               right_ref = co_await create_internal(t);
        Node* right = co_await update_node(t, right_ref);

        // copy right half of LEFT's keys+children to RIGHT
        CountType m = (left->key_count+1)/2; // ceil(middle)
        right->key_count = left->key_count - m;
        os::memory_copy(keys(right), view_shift_up(keys(left), m));
        if (is_child_leaf)
            os::memory_copy(values(right, h), view_shift_up(values(left, h), m));
        else
            os::memory_copy(children(right, h), view_shift_up(children(left, h), m));

        KeyType sep_key = is_child_leaf ? keys(right)[0] : keys(left)[m-1];
        // @note for internal nodes we drop the separating key
        left->key_count = is_child_leaf ? m : (m-1);

        // insert RIGHT into PARENT with separating key
        co_await insert_child_to_right(t, h, parent, child_idx, sep_key, right_ref, right);
    }

    // @precondition leaf is not full
    template<bool avoid_duplicates=true>
    U8* insert_in_leaf(Header& h, Node* leaf, KeyType key) {
        assert_true(leaf->key_count < max_keys(h, true), "leaf is not full");

        CountType idx = 0;
        if (likely(leaf->key_count > 0)) {
            if constexpr (avoid_duplicates) {
                idx = binary_search_first_geq(keys(leaf), key);
                if (idx < leaf->key_count && keys(leaf)[idx] == key)
                    return values(leaf, h)[idx];
                os::memory_shift_up(view_shift_up(keys(leaf), idx));
            } else {
                idx = leaf->key_count - 1;
                auto ks = keys(leaf);
                for (; idx >= 0 && key < ks[idx]; idx--) // @todo reverse iterator?
                    ks[idx+1] = ks[idx];
                idx++;
            }
            os::memory_shift_up(view_shift_up(values(leaf, h), idx));
        }

        leaf->key_count++;
        keys(leaf)[idx] = key;
        h.size++;
        return values(leaf, h)[idx];
    }

    // @invariant n is not full
    template<Transaction Tx>
    coroutine::Task<U8*> insert_recursive(Tx& t, const Header& h, NodeRef n_ref, CountType depth, KeyType key) {
        // leaf
        if (depth == h.depth) {
            // acquire lock on returned memory
            Node* n = co_await update_node(t, n_ref);

            Header& h_write = *(co_await update_header(t));
            co_return insert_in_leaf(h_write, n, key);
        }

        NodeRef child_ref;
        bool is_child_leaf = (depth + 1) == h.depth;

        // internal overflow
        {
            const Node* n = co_await read_node(t, n_ref);
            CountType child_idx = binary_search_first_gt(keys(n), key);
            child_ref = children(n, h)[child_idx];
            const Node* child = co_await read_node(t, child_ref);

            if (child->key_count == max_keys(h, is_child_leaf)) {
                co_await split_child(t, h, co_await update_node(t, n_ref), child_idx, is_child_leaf);

                child_idx = (key > keys(n)[child_idx]) ? (child_idx+1) : child_idx;
                child_ref = children(n, h)[child_idx];
            }

            assert_true(child->key_count < max_keys(h, is_child_leaf), "insert maintains max invariant.");
        }
        co_return co_await insert_recursive(t, h, child_ref, depth+1, key);
    }

    template<Transaction Tx>
    coroutine::Task<U8*> insert_impl(Tx& t, KeyType key) { ZoneScopedN("btree::insert");
        // acquire read on header for duration of insertion
        const auto& h = *(co_await read_header(t));

        // ensure root is not full
        bool is_root_leaf = h.depth == 0;
        {
            const Node* root = co_await read_node(t, h.root);

            if (root->key_count == max_keys(h, is_root_leaf)) {
                NodeRef new_root_ref = co_await create_internal(t);
                Node* new_root = co_await update_node(t, new_root_ref);
                children(new_root, h)[0] = h.root;
                co_await split_child(t, h, new_root, 0, is_root_leaf);

                auto& h_write = *(co_await update_header(t));
                h_write.root = new_root_ref;
                h_write.depth++;
            }
        }

        co_return co_await insert_recursive(t, h, h.root, 0, key);
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

    template<Transaction Tx>
    coroutine::Task<Search> search_impl(Tx& t, KeyType key) { ZoneScopedN("btree::search");
        // acquire read on header for duration of search
        const auto& h = *(co_await read_header(t));

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n = co_await read_node(t, n_ref);
            CountType idx = binary_search_first_gt(keys(n), key);
            n_ref = children(n, h)[idx];
        }

        // acquire lock on returned memory
        Node* n = co_await update_node(t, n_ref);
        CountType idx = binary_search_first_geq(keys(n), key);
        if (idx < n->key_count && keys(n)[idx] == key)
            co_return Search{n, idx, values(n, h)[idx]};
        co_return Search{};
    }

    // ========================================================================
    // remove
    // ========================================================================
    struct RemoveStackItem {
        NodeRef node;
        CountType idx;
    };

    template<Transaction Tx>
    coroutine::Task<bool> remove_impl(Tx& t, KeyType key) { ZoneScopedN("btree::remove");

        // acquire read on header for duration of remove
        const auto& h = *(co_await read_header(t));

        // traverse to leaf and store path
        Stack<RemoveStackItem> stack{};
        // @todo @perf temp arena
        Arena stack_arena{/*page_size*/ sizeof(decltype(stack)::Node)*h.depth};
        NodeRef node_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {

            const Node* node = co_await read_node(t, node_ref);
            CountType idx = binary_search_first_gt(keys(node), key);
            push_front(stack_arena, stack, RemoveStackItem{node_ref, idx});
            node_ref = children(node, h)[idx];
        }

        // find and delete key
        CountType idx;
        {
            const Node* node = co_await read_node(t, node_ref);
            idx = binary_search_first_geq(keys(node), key);
            if (idx >= node->key_count || keys(node)[idx] != key)
                co_return false;
        }

        Node* node = co_await update_node(t, node_ref);
        delete_from_leaf(h, node, idx);

        // fix underflow, propagating upwards as needed
        bool is_leaf = true;
        while (node->key_count < min_keys(h, is_leaf)) {
            if (stack.length == 0) {
                if (node->key_count != 0 || h.depth == 0)
                    break;

                NodeRef new_root = children(node, h)[0];
                co_await delete_node(t, node_ref);

                Header& h_write = *(co_await update_header(t));
                h_write.root = new_root;
                h_write.depth--;
                h_write.size--;
                co_return true; // @note, avoids reacquiring header, may not be necessary
            }

            RemoveStackItem* item = front(stack); pop_front(stack);
            NodeRef& parent_ref = item->node;
            CountType& item_idx = item->idx;
            Node* parent = co_await update_node(t, parent_ref);

            // borrow from left or right sibling if it does not break invariant
            NodeRef left_ref = (item_idx > 0) ? children(parent,h)[item_idx-1] : 0;
            Node* left = nullptr;
            if (left_ref != 0) left = co_await update_node(t, left_ref);
            if (left && left->key_count > min_keys(h, is_leaf)) {
                move_from_left(parent, left, node, item_idx, h, is_leaf);
                break;
            }
            NodeRef right_ref = (item_idx < parent->key_count) ? children(parent,h)[item_idx+1] : 0;
            Node* right = nullptr;
            if (right_ref != 0) right = co_await update_node(t, right_ref);
            if (right && right->key_count > min_keys(h, is_leaf)) {
                move_from_right(parent, right, node, item_idx, h, is_leaf);
                break;
            }

            assert_true(left || right, "node must have at least one sibling.");

            // otherwise we need to merge, this causes a delete in the parent
            // which needs to be propagated
            if (left) {
                merge(h, left, node, parent, item_idx-1, is_leaf);
                co_await delete_node(t, node_ref);
            } else {
                merge(h, node, right, parent, item_idx, is_leaf);
                co_await delete_node(t, right_ref);
            }

            node_ref = parent_ref;
            node = parent;
            is_leaf = false;
        }
        // @note loop contains a return

        (co_await update_header(t))->size--;
        co_return true;
    }

    template<Transaction Tx>
    coroutine::Task<void> truncate_impl(Tx& t) {

        // acquire read/write on header for duration of truncate
        auto& h = *(co_await update_header(t));

        // traverse from first child of root to leaf
        // deleting horizontally in a zig-zag pattern
        Node* n_root = co_await update_node(t, h.root);
        auto root_children_view = children(n_root, h);
        if (root_children_view.length > 0) {
            NodeRef curr_ref = root_children_view[0];
            for (CountType depth = 1; depth <= h.depth; depth++) {
                const Node* curr = co_await read_node(t, curr_ref);
                if (curr->prev == 0) {
                    while (curr->next != 0) {
                        NodeRef next_ref = curr->next;
                        co_await delete_node(t, curr_ref);

                        curr_ref = next_ref;
                        curr = co_await read_node(t, curr_ref);
                    }

                    if (depth != h.depth) {
                        auto children_view = children(curr, h);
                        NodeRef right_most_child_ref = children_view[children_view.length-1];
                        co_await delete_node(t, curr_ref);

                        curr_ref = right_most_child_ref;
                    } else {
                        co_await delete_node(t, curr_ref);
                    }
                } else {
                    assert_true(curr->next == 0, "first node in level must be left-most or right-most");

                    while (curr->prev != 0) {
                        NodeRef prev_ref = curr->prev;
                        co_await delete_node(t, curr_ref);

                        curr_ref = prev_ref;
                        curr = co_await read_node(t, curr_ref);
                    }

                    if (depth != h.depth) {
                        auto children_view = children(curr, h);
                        NodeRef left_most_child_ref = children_view[0];
                        co_await delete_node(t, curr_ref);

                        curr_ref = left_most_child_ref;
                    } else {
                        co_await delete_node(t, curr_ref);
                    }
                }
            }
        }

        NodeRef old_root_ref = h.root;
        NodeRef new_root_ref = co_await create_leaf(t);
        Node* new_root = co_await update_node(t, new_root_ref);
        init_node(new_root);
        auto& h_write = *(co_await update_header(t));
        h_write.root   = new_root_ref;
        h_write.leaves = new_root_ref;
        h_write.size   = 0;
        h_write.depth  = 0;
        co_await delete_node(t, old_root_ref);
    }

    // ========================================================================
    // iterators
    // ========================================================================
    struct IteratorImpl {
        const Node* leaf = nullptr;
        NodeRef ref = 0;
        CountType idx = 0;
        U64 value_stride = 0;
        CountType max_keys_per_leaf = 0;

        bool operator==(const IteratorImpl& other) const { return ref == other.ref && idx == other.idx; }
        bool operator!=(const IteratorImpl& other) const { return !(*this == other); }
    };

    template<Transaction Tx>
    coroutine::Task<IteratorImpl> begin_iterator_impl(Tx& t) {
        const auto& h = *(co_await read_header(t));

        if (h.size == 0) {
            co_return IteratorImpl{};
        }

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n = co_await read_node(t, n_ref);

            assert_true(keys(n).length > 0, "internal node must have at least one key");
            n_ref = children(n, h)[0];
        }

        const Node* n = co_await read_node(t, n_ref);
        assert_true(keys(n).length > 0, "leaf node must have at least one value ");
        co_return IteratorImpl{
            .leaf = n,
            .ref = n_ref,
            .idx = 0_u16,
            .value_stride = h.value_stride,
            .max_keys_per_leaf = h.max_keys_per_leaf,
        };
    }

    inline IteratorImpl end_iterator_impl() {
        return IteratorImpl{};
    }

    template<Transaction Tx>
    coroutine::Task<IteratorImpl> next_iterator_impl(Tx& t, const IteratorImpl& it) {
        assert_true(it.leaf != nullptr, "cannot get next iterator after end");

        if (it.idx == it.leaf->key_count - 1) {
            if (it.leaf->next != 0) {
                NodeRef next_ref = it.leaf->next;
                co_return IteratorImpl{
                    .leaf = co_await read_node(t, next_ref),
                    .ref = next_ref,
                    .idx = 0_u16,
                    .value_stride = it.value_stride,
                    .max_keys_per_leaf = it.max_keys_per_leaf,
                };
            } else {
                co_return end_iterator_impl();
            }
        }

        co_return IteratorImpl{
            .leaf = it.leaf,
            .ref = it.ref,
            .idx = static_cast<CountType>(it.idx + 1_u16),
            .value_stride = it.value_stride,
            .max_keys_per_leaf = it.max_keys_per_leaf,
        };
    }

    template<SearchStrategy Strategy, Transaction Tx>
    coroutine::Task<IteratorImpl> search_iterator_impl(Tx& t, KeyType key) {
        const auto& h = *(co_await read_header(t));
        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n = co_await read_node(t, n_ref);
            CountType idx = binary_search_first_gt(keys(n), key);
            n_ref = children(n, h)[idx];
        }

        const Node* n = co_await read_node(t, n_ref);
        if constexpr (Strategy == SearchStrategy::RequireEquality) {
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count && keys(n)[idx] == key) {
                co_return IteratorImpl{n, n_ref, idx, h.value_stride, h.max_keys_per_leaf};
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::FirstGreaterEqual) {
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count) {
                co_return IteratorImpl{n, n_ref, idx, h.value_stride, h.max_keys_per_leaf};
            }
            if (n->next != 0) {
                NodeRef next_ref = n->next;
                const Node* next = co_await read_node(t, next_ref);
                co_return IteratorImpl{next, next_ref, 0, h.value_stride, h.max_keys_per_leaf};
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::FirstGreater) {
            CountType idx = binary_search_first_gt(keys(n), key);
            if (idx < n->key_count) {
                co_return IteratorImpl{n, n_ref, idx, h.value_stride, h.max_keys_per_leaf};
            }
            if (n->next != 0) {
                NodeRef next_ref = n->next;
                const Node* next = co_await read_node(t, next_ref);
                co_return IteratorImpl{next, next_ref, 0, h.value_stride, h.max_keys_per_leaf};
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::LastLessEqual) {
            CountType idx = binary_search_last_leq(keys(n), key);

            if (idx < n->key_count) {
                co_return IteratorImpl{n, n_ref, idx, h.value_stride, h.max_keys_per_leaf};
            }
            if (n->prev != 0) {
                NodeRef prev_ref = n->prev;
                const Node* prev = co_await read_node(t, prev_ref);
                if (prev->key_count > 0) {
                    co_return IteratorImpl{prev, prev_ref, static_cast<CountType>(prev->key_count - CountType{1}), h.value_stride, h.max_keys_per_leaf};
                }
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::LastLess) {
            CountType idx = binary_search_last_lt(keys(n), key);

            if (idx < n->key_count) {
                co_return IteratorImpl{n, n_ref, idx, h.value_stride, h.max_keys_per_leaf};
            }
            if (n->prev != 0) {
                NodeRef prev_ref = n->prev;
                const Node* prev = co_await read_node(t, prev_ref);
                if (prev->key_count > 0) {
                    co_return IteratorImpl{prev, prev_ref, static_cast<CountType>(prev->key_count - CountType{1}), h.value_stride, h.max_keys_per_leaf};
                }
            }
            co_return IteratorImpl{};
        }
    }
}
