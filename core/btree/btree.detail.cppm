module;
#include <profiling/tracy.hpp>

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
    template<Transaction Tx>
    void insert_child_to_right(Tx& t, const Header& h, Node* parent, CountType idx, KeyType key, NodeRef child_ref, Node* child) {
        if (idx < parent->key_count) {
            os::memory_shift_up(view_shift_up(keys(parent), idx));
            os::memory_shift_up(view_shift_up(children(parent, h), static_cast<CountType>(idx+1)));
        }
        parent->key_count++;
        children(parent, h)[idx+1] = child_ref;
        keys(parent)[idx] = key;

        // insert into doubly linked list
        NodeRef prev_ref = children(parent, h)[idx];
        Node* prev = update_node(t, prev_ref);
        child->next = prev->next;
        child->prev = prev_ref;
        prev->next = child_ref;
    }

    // @precondition parent is not full
    template<Transaction Tx>
    void split_child(Tx& t, const Header& h, Node* parent, CountType child_idx, bool is_child_leaf) { ZoneScopedN("btree::split");
        assert_true(parent->key_count < max_keys(h, false), "parent is not full");

        Node* left = update_node(t, children(parent, h)[child_idx]);
        NodeRef right_ref = is_child_leaf ? create_leaf(t) : create_internal(t);
        Node* right = update_node(t, right_ref);

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
        insert_child_to_right(t, h, parent, child_idx, sep_key, right_ref, right);
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
    U8* insert_recursive(Tx& t, const Header& h, NodeRef n_ref, CountType depth, KeyType key) {
        // leaf
        if (depth == h.depth) {
            // acquire lock on returned memory
            Node* n = update_node(t, n_ref);

            auto t_leaf = scope(t);
            Header& h = *update_header(t_leaf);
            return insert_in_leaf(h, n, key);
        }

        NodeRef child_ref;
        bool is_child_leaf = (depth + 1) == h.depth;

        // internal overflow
        {
            auto t_int = scope(t);

            const Node* n = read_node(t_int, n_ref);
            CountType child_idx = binary_search_first_gt(keys(n), key);
            child_ref = children(n, h)[child_idx];
            const Node* child = read_node(t_int, child_ref);

            if (child->key_count == max_keys(h, is_child_leaf)) {
                split_child(t_int, h, update_node(t_int, n_ref), child_idx, is_child_leaf);

                child_idx = (key > keys(n)[child_idx]) ? (child_idx+1) : child_idx;
                child_ref = children(n, h)[child_idx];
            }

            assert_true(child->key_count < max_keys(h, is_child_leaf), "insert maintains max invariant.");
        }
        return insert_recursive(t, h, child_ref, depth+1, key);
    }

    template<Transaction Tx>
    U8* insert_impl(Tx& t, KeyType key) { ZoneScopedN("btree::insert");
        // acquire read on header for duration of insertion
        auto t_header = scope(t);
        const auto& h = *read_header(t_header);

        // ensure root is not full
        bool is_root_leaf = h.depth == 0;
        {
            auto t_root = scope(t);
            const Node* root = read_node(t_root, h.root);

            if (root->key_count == max_keys(h, is_root_leaf)) {
                NodeRef new_root_ref = create_internal(t_root);
                Node* new_root = update_node(t_root, new_root_ref);
                children(new_root, h)[0] = h.root;
                split_child(t_root, h, new_root, 0, is_root_leaf);

                auto& h_write = *update_header(t_root);
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

    template<Transaction Tx>
    Search search_impl(Tx& t, KeyType key) { ZoneScopedN("btree::search");
        // acquire read on header for duration of search
        // @todo check necessary for duration since depth changing may not break search
        auto t_header = scope(t);
        const auto& h = *read_header(t_header);

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            auto t_node = scope(t);
            const Node* n = read_node(t_node, n_ref);
            CountType idx = binary_search_first_gt(keys(n), key);
            n_ref = children(n, h)[idx];
        }

        // acquire lock on returned memory
        // @note still acquires lock if search fails, assuming transaction will
        // be ended
        Node* n = update_node(t, n_ref);
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

    template<Transaction Tx>
    bool remove_impl(Tx& t, KeyType key) { ZoneScopedN("btree::remove");
        auto t_remove = scope(t);

        // acquire read on header for duration of remove
        const auto& h = *read_header(t_remove);

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

            const Node* node = read_node(t_down, node_ref);
            CountType idx = binary_search_first_gt(keys(node), key);
            push_front(stack_arena, stack, RemoveStackItem{node_ref, idx});
            node_ref = children(node, h)[idx];
        }

        // r/w lifetimes are somewhat complicated so for now everything is in one
        // transaction, again on the assumption that propogation is not deep

        // find and delete key
        CountType idx;
        {
            const Node* node = read_node(t_remove, node_ref);
            idx = binary_search_first_geq(keys(node), key);
            if (idx >= node->key_count || keys(node)[idx] != key)
                return false;
        }

        Node* node = update_node(t_remove, node_ref);
        delete_from_leaf(h, node, idx);

        // fix underflow, propagating upwards as needed
        bool is_leaf = true;
        while (node->key_count < min_keys(h, is_leaf)) {
            if (stack.length == 0) {
                if (node->key_count != 0 || h.depth == 0)
                    break;

                delete_node(t_remove, node_ref);

                Header& h_write = *update_header(t_remove);
                h_write.root = children(node, h)[0];
                h_write.depth--;
                h_write.size--;
                return true; // @note, avoids reacquiring header, may not be necessary
            }

            RemoveStackItem* item = front(stack); pop_front(stack);
            NodeRef& parent_ref = item->node;
            CountType& idx = item->idx;
            Node* parent = update_node(t_remove, parent_ref);

            // borrow from left or right sibling if it does not break invariant
            NodeRef left_ref = (idx > 0) ? children(parent,h)[idx-1] : 0;
            Node* left = (left_ref != 0) ? update_node(t_remove, left_ref) : nullptr;
            if (left && left->key_count > min_keys(h, is_leaf)) {
                move_from_left(parent, left, node, idx, h, is_leaf);
                break;
            }
            NodeRef right_ref = (idx < parent->key_count) ? children(parent,h)[idx+1] : 0;
            Node* right = (right_ref != 0) ? update_node(t_remove, right_ref) : nullptr;
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

        update_header(t_remove)->size--;
        return true;
    }

    template<Transaction Tx>
    void truncate_impl(Tx& t) {
        auto t_truncate = scope(t);

        // acquire read/write on header for duration of truncate
        auto& h = *update_header(t_truncate);

        // traverse from first child of root to leaf
        // deleting horizontally in a zig-zag pattern
        Node* n_root = update_node(t_truncate, h.root);
        auto root_children_view = children(n_root, h);
        if (root_children_view.length > 0) {
            NodeRef curr_ref = root_children_view[0];
            for (CountType depth = 1; depth <= h.depth; depth++) {
                const Node* curr = read_node(t_truncate, curr_ref);
                if (curr->prev == 0) {
                    while (curr->next != 0) {
                        NodeRef next_ref = curr->next;
                        delete_node(t_truncate, curr_ref);

                        curr_ref = next_ref;
                        curr = read_node(t_truncate, curr_ref);
                    }

                    if (depth != h.depth) {
                        auto children_view = children(curr, h);
                        NodeRef right_most_child_ref = children_view[children_view.length-1];
                        delete_node(t_truncate, curr_ref);

                        curr_ref = right_most_child_ref;
                    } else {
                        delete_node(t_truncate, curr_ref);
                    }
                } else {
                    assert_true(curr->next == 0, "first node in level must be left-most or right-most");

                    while (curr->prev != 0) {
                        NodeRef prev_ref = curr->prev;
                        delete_node(t_truncate, curr_ref);

                        curr_ref = prev_ref;
                        curr = read_node(t_truncate, curr_ref);
                    }

                    if (depth != h.depth) {
                        auto children_view = children(curr, h);
                        NodeRef left_most_child_ref = children_view[0];
                        delete_node(t_truncate, curr_ref);

                        curr_ref = left_most_child_ref;
                    } else {
                        delete_node(t_truncate, curr_ref);
                    }
                }
            }
        }

        n_root->key_count = 0;
        h.size = 0;
        h.depth = 0;
    }

    // ========================================================================
    // iterators
    // ========================================================================
    struct IteratorImpl {
        const Node* leaf = nullptr;
        NodeRef ref = 0;
        CountType idx = 0;

        bool operator==(const IteratorImpl& other) const { return ref == other.ref && idx == other.idx; }
        bool operator!=(const IteratorImpl& other) const { return !(*this == other); }
    };

    template<Transaction Tx>
    IteratorImpl begin_iterator_impl(Tx& t) {
        // acquire read on header for duration of search
        // @todo check necessary for duration since depth changing may not break search
        auto t_header = scope(t);
        const auto& h = *read_header(t_header);

        if (h.size == 0) {
            return IteratorImpl{};
        }

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            auto t_node = scope(t);
            const Node* n = read_node(t_node, n_ref);

            assert_true(keys(n).length > 0, "internal node must have at least one key");
            n_ref = children(n, h)[0];
        }

        const Node* n = read_node(t, n_ref);
        assert_true(keys(n).length > 0, "leaf node must have at least one value ");
        return IteratorImpl{
            .leaf = n,
            .ref = n_ref,
            .idx = 0_u16,
        };
    }

    inline IteratorImpl end_iterator_impl() {
        return IteratorImpl{};
    }

    template<Transaction Tx>
    IteratorImpl next_iterator_impl(Tx& t, const IteratorImpl& it) {
        assert_true(it.leaf != nullptr, "cannot get next iterator after end");

        if (it.idx == it.leaf->key_count - 1) {
            // @todo release previous node from transaction

            if (it.leaf->next != 0) {
                return {
                    .leaf = read_node(t, it.leaf->next),
                    .ref = it.leaf->next,
                    .idx = 0_u16,
                };
            } else {
                return end_iterator_impl();
            }
        }

        return {
            .leaf = it.leaf,
            .ref = it.ref,
            .idx = static_cast<CountType>(it.idx + 1_u16),
        };
    }

    template<SearchStrategy Strategy, Transaction Tx>
    IteratorImpl search_iterator_impl(Tx& t, KeyType key) {
        auto t_header = scope(t);
        const auto& h = *read_header(t_header);
        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            auto t_node = scope(t);
            const Node* n = read_node(t_node, n_ref);
            CountType idx = binary_search_first_gt(keys(n), key);
            n_ref = children(n, h)[idx];
        }

        const Node* n = read_node(t, n_ref);
        if constexpr (Strategy == SearchStrategy::RequireEquality) {
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count && keys(n)[idx] == key) {
                return IteratorImpl{n, n_ref, idx};
            }
            return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::FirstGreaterEqual) {
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count) {
                return IteratorImpl{n, n_ref, idx};
            }
            if (n->next != 0) {
                const Node* next = read_node(t, n->next);
                return IteratorImpl{next, n->next, 0};
            }
            return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::FirstGreater) {
            CountType idx = binary_search_first_gt(keys(n), key);
            if (idx < n->key_count) {
                return IteratorImpl{n, n_ref, idx};
            }
            if (n->next != 0) {
                const Node* next = read_node(t, n->next);
                return IteratorImpl{next, n->next, 0};
            }
            return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::LastLessEqual) {
            CountType idx = binary_search_last_leq(keys(n), key);

            if (idx < n->key_count) {
                return IteratorImpl{n, n_ref, idx};
            }
            if (n->prev != 0) {
                const Node* prev = read_node(t, n->prev);
                if (prev->key_count > 0) {
                    return IteratorImpl{prev, n->prev, static_cast<CountType>(prev->key_count - CountType{1})};
                }
            }
            return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::LastLess) {
            CountType idx = binary_search_last_lt(keys(n), key);

            if (idx < n->key_count) {
                return IteratorImpl{n, n_ref, idx};
            }
            if (n->prev != 0) {
                const Node* prev = read_node(t, n->prev);
                if (prev->key_count > 0) {
                    return IteratorImpl{prev, n->prev, static_cast<CountType>(prev->key_count - CountType{1})};
                }
            }
            return IteratorImpl{};
        }
    }
}
