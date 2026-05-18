module;
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

export module plexdb.btree.detail;

export import plexdb.btree.in_memory.detail;
export import plexdb.btree.paged.detail;

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.coroutine;

import plexdb.btree.constraint;
import plexdb.btree.policy;
import plexdb.btree.types;
import plexdb.btree.node;

export namespace plexdb::btree {
    // ========================================================================
    // helpers — capacity/threshold, using node_size(t)
    // ========================================================================
    CountType max_keys(U32 node_size, U16 val_stride, bool is_leaf);
    CountType min_keys(U32 node_size, U16 val_stride, bool is_leaf);

    void move_from_left (Node* parent, Node* left,  Node* node, CountType node_idx,
                         U32 node_size, U16 val_stride, bool is_leaf);
    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx,
                         U32 node_size, U16 val_stride, bool is_leaf);
    void merge(Node* a, Node* b, Node* parent, CountType a_idx,
               U32 node_size, U16 val_stride, bool is_leaf);
    void delete_from_leaf(Node* node, U32 node_size, U16 val_stride, CountType idx);

    // ========================================================================
    // insert
    // ========================================================================
    template<BTree BT>
    coroutine::Task<void> insert_child_to_right(BT& t, U32 ns, Node* parent, CountType idx,
                                                 U64 key, NodeRef child_ref, Node* child) {
        if (idx < parent->key_count) {
            os::memory_shift_up(view_shift_up(keys(parent), idx));
            os::memory_shift_up(view_shift_up(children(parent, ns), static_cast<CountType>(idx+1)));
        }
        parent->key_count++;
        children(parent, ns)[idx+1] = child_ref;
        keys(parent)[idx] = key;

        NodeRef prev_ref = children(parent, ns)[idx];
        Node* prev = co_await update_node(t, prev_ref);
        child->next = prev->next;
        child->prev = prev_ref;
        prev->next = child_ref;
    }

    template<BTree BT>
    coroutine::Task<void> split_child(BT& t, U32 ns, U16 val_stride,
                                       Node* parent, CountType child_idx, bool is_child_leaf) {
        ZoneScopedN("btree::split");
        assert_true(parent->key_count < max_keys(ns, val_stride, false), "parent is not full");

        Node* left = co_await update_node(t, children(parent, ns)[child_idx]);
        NodeRef right_ref;
        if (is_child_leaf) right_ref = co_await create_leaf(t);
        else               right_ref = co_await create_internal(t);
        Node* right = co_await update_node(t, right_ref);

        CountType m = static_cast<CountType>((left->key_count + 1) / 2);
        right->key_count = static_cast<CountType>(left->key_count - m);
        os::memory_copy(keys(right), view_shift_up(keys(left), m));
        if (is_child_leaf)
            os::memory_copy(values(right, ns, val_stride), view_shift_up(values(left, ns, val_stride), m));
        else
            os::memory_copy(children(right, ns), view_shift_up(children(left, ns), m));

        U64 sep_key = is_child_leaf ? keys(right)[0] : keys(left)[m-1];
        left->key_count = is_child_leaf ? m : static_cast<CountType>(m - 1);

        co_await insert_child_to_right(t, ns, parent, child_idx, sep_key, right_ref, right);
    }

    template<bool avoid_duplicates = true>
    U8* insert_in_leaf(Node* leaf, U32 ns, U16 val_stride, U64 key) {
        assert_true(leaf->key_count < max_keys(ns, val_stride, true), "leaf is not full");

        CountType idx = 0;
        if (likely(leaf->key_count > 0)) {
            if constexpr (avoid_duplicates) {
                idx = binary_search_first_geq(keys(leaf), key);
                if (idx < leaf->key_count && keys(leaf)[idx] == key)
                    return values(leaf, ns, val_stride)[idx];
                os::memory_shift_up(view_shift_up(keys(leaf), idx));
            } else {
                idx = static_cast<CountType>(leaf->key_count - 1);
                auto ks = keys(leaf);
                for (; idx >= 0 && key < ks[idx]; idx--)
                    ks[idx+1] = ks[idx];
                idx++;
            }
            os::memory_shift_up(view_shift_up(values(leaf, ns, val_stride), idx));
        }

        leaf->key_count++;
        keys(leaf)[idx] = key;
        return values(leaf, ns, val_stride)[idx];
    }

    template<BTree BT>
    coroutine::Task<U8*> insert_recursive(BT& t, const Header& h, U32 ns, U16 val_stride,
                                           NodeRef n_ref, CountType depth, U64 key) {
        if (depth == h.depth) {
            Node* n = co_await update_node(t, n_ref);
            CountType old_count = n->key_count;
            U8* val = insert_in_leaf(n, ns, val_stride, key);
            if (n->key_count > old_count)
                (co_await update_header(t))->size++;
            co_return val;
        }

        NodeRef child_ref;
        bool is_child_leaf = (depth + 1) == h.depth;

        {
            const Node* n = co_await read_node(t, n_ref);
            CountType child_idx = binary_search_first_gt(keys(n), key);
            child_ref = children(n, ns)[child_idx];
            const Node* child = co_await read_node(t, child_ref);

            if (child->key_count == max_keys(ns, val_stride, is_child_leaf)) {
                co_await split_child(t, ns, val_stride,
                                     co_await update_node(t, n_ref), child_idx, is_child_leaf);
                child_idx = (key > keys(n)[child_idx]) ? static_cast<CountType>(child_idx+1) : child_idx;
                child_ref = children(n, ns)[child_idx];
            }
            assert_true(child->key_count < max_keys(ns, val_stride, is_child_leaf),
                        "insert maintains max invariant.");
        }
        co_return co_await insert_recursive(t, h, ns, val_stride, child_ref, static_cast<CountType>(depth+1), key);
    }

    template<BTree BT>
    coroutine::Task<U8*> insert_impl(BT& t, U64 key) { ZoneScopedN("btree::insert");
        const auto& h = *(co_await read_header(t));
        U32 ns = node_size(t);
        U16 vs = static_cast<U16>(h.value_stride);

        bool is_root_leaf = h.depth == 0;
        {
            const Node* root = co_await read_node(t, h.root);
            if (root->key_count == max_keys(ns, vs, is_root_leaf)) {
                NodeRef new_root_ref = co_await create_internal(t);
                Node* new_root = co_await update_node(t, new_root_ref);
                children(new_root, ns)[0] = h.root;
                co_await split_child(t, ns, vs, new_root, 0, is_root_leaf);

                auto& h_write = *(co_await update_header(t));
                h_write.root = new_root_ref;
                h_write.depth++;
            }
        }
        co_return co_await insert_recursive(t, h, ns, vs, h.root, 0, key);
    }

    // ========================================================================
    // search
    // ========================================================================
    template<bool Mutable>
    struct Search {
        Conditional<Mutable, Node*, const Node*> leaf = nullptr;
        CountType idx   = 0;
        Conditional<Mutable, U8*, const U8*> value = nullptr;
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return leaf != nullptr; }
    };

    template<bool Mutable, BTree BT>
    coroutine::Task<Search<Mutable>> search_impl(BT& t, U64 key) { ZoneScopedN("btree::search");
        const auto& h = *(co_await read_header(t));
        U32 ns = node_size(t);
        U16 vs = static_cast<U16>(h.value_stride);

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n = co_await read_node(t, n_ref);
            CountType idx = binary_search_first_gt(keys(n), key);
            n_ref = children(n, ns)[idx];
        }

        if constexpr (Mutable) {
            Node* n = co_await update_node(t, n_ref);
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count && keys(n)[idx] == key)
                co_return Search<true>{n, idx, values(n, ns, vs)[idx]};
            co_return Search<true>{};
        } else {
            const Node* n = co_await read_node(t, n_ref);
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count && keys(n)[idx] == key)
                co_return Search<false>{n, idx, values(n, ns, vs)[idx]};
            co_return Search<false>{};
        }
    }

    // ========================================================================
    // remove
    // ========================================================================
    struct RemoveStackItem { NodeRef node; CountType idx; };

    template<BTree BT>
    coroutine::Task<bool> remove_impl(BT& t, U64 key) { ZoneScopedN("btree::remove");
        const auto& h = *(co_await read_header(t));
        U32 ns = node_size(t);
        U16 vs = static_cast<U16>(h.value_stride);

        Stack<RemoveStackItem> stack{};
        Arena stack_arena{sizeof(decltype(stack)::Node) * h.depth};
        NodeRef node_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* node = co_await read_node(t, node_ref);
            CountType idx = binary_search_first_gt(keys(node), key);
            push_front(stack_arena, stack, RemoveStackItem{node_ref, idx});
            node_ref = children(node, ns)[idx];
        }

        CountType idx;
        {
            const Node* node = co_await read_node(t, node_ref);
            idx = binary_search_first_geq(keys(node), key);
            if (idx >= node->key_count || keys(node)[idx] != key)
                co_return false;
        }

        Node* node = co_await update_node(t, node_ref);
        delete_from_leaf(node, ns, vs, idx);

        bool is_leaf = true;
        while (node->key_count < min_keys(ns, vs, is_leaf)) {
            if (stack.length == 0) {
                if (node->key_count != 0 || h.depth == 0)
                    break;

                NodeRef new_root = children(node, ns)[0];
                co_await delete_node(t, node_ref);

                Header& h_write = *(co_await update_header(t));
                h_write.root = new_root;
                h_write.depth--;
                h_write.size--;
                co_return true;
            }

            RemoveStackItem* item = front(stack); pop_front(stack);
            NodeRef& parent_ref  = item->node;
            CountType& item_idx  = item->idx;
            Node* parent = co_await update_node(t, parent_ref);

            NodeRef left_ref = (item_idx > 0) ? children(parent, ns)[item_idx-1] : 0;
            Node* left = nullptr;
            if (left_ref != 0) left = co_await update_node(t, left_ref);
            if (left && left->key_count > min_keys(ns, vs, is_leaf)) {
                move_from_left(parent, left, node, item_idx, ns, vs, is_leaf);
                break;
            }
            NodeRef right_ref = (item_idx < parent->key_count) ? children(parent, ns)[item_idx+1] : 0;
            Node* right = nullptr;
            if (right_ref != 0) right = co_await update_node(t, right_ref);
            if (right && right->key_count > min_keys(ns, vs, is_leaf)) {
                move_from_right(parent, right, node, item_idx, ns, vs, is_leaf);
                break;
            }

            assert_true(left || right, "node must have at least one sibling.");

            if (left) {
                merge(left, node, parent, static_cast<CountType>(item_idx-1), ns, vs, is_leaf);
                co_await delete_node(t, node_ref);
            } else {
                merge(node, right, parent, item_idx, ns, vs, is_leaf);
                co_await delete_node(t, right_ref);
            }

            node_ref = parent_ref;
            node = parent;
            is_leaf = false;
        }

        (co_await update_header(t))->size--;
        co_return true;
    }

    template<BTree BT>
    coroutine::Task<void> truncate_impl(BT& t) {
        auto& h = *(co_await update_header(t));
        U32 ns = node_size(t);

        Node* n_root = co_await update_node(t, h.root);
        auto root_children_view = children(n_root, ns);
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
                        auto cv = children(curr, ns);
                        NodeRef right_most = cv[cv.length-1];
                        co_await delete_node(t, curr_ref);
                        curr_ref = right_most;
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
                        NodeRef left_most = children(curr, ns)[0];
                        co_await delete_node(t, curr_ref);
                        curr_ref = left_most;
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
    template<BTree BT>
    coroutine::Task<IteratorImpl> begin_iterator_impl(BT& t) {
        const auto& h = *(co_await read_header(t));

        if (h.size == 0) {
            co_return IteratorImpl{};
        }

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n = co_await read_node(t, n_ref);
            assert_true(keys(n).length > 0, "internal node must have at least one key");
            n_ref = children(n, node_size(t))[0];
        }

        const Node* n = co_await read_node(t, n_ref);
        assert_true(keys(n).length > 0, "leaf node must have at least one value");

        IteratorImpl it{};
        it.leaf      = n;
        it.ref       = n_ref;
        it.idx       = 0_u16;
        it.node_size = node_size(t);
        it.vp        = FixedValuePolicy{.stride = static_cast<U16>(h.value_stride)};
        co_return it;
    }

    inline IteratorImpl end_iterator_impl() { return IteratorImpl{}; }

    template<BTree BT>
    coroutine::Task<IteratorImpl> next_iterator_impl(BT& t, const IteratorImpl& it) {
        assert_true(it.leaf != nullptr, "cannot get next iterator after end");

        if (it.idx == it.leaf->key_count - 1) {
            if (it.leaf->next != 0) {
                NodeRef next_ref = it.leaf->next;
                IteratorImpl nit{};
                nit.leaf      = co_await read_node(t, next_ref);
                nit.ref       = next_ref;
                nit.idx       = 0_u16;
                nit.node_size = it.node_size;
                nit.vp        = it.vp;
                co_return nit;
            }
            co_return end_iterator_impl();
        }

        IteratorImpl nit = it;
        nit.idx = static_cast<CountType>(it.idx + 1_u16);
        co_return nit;
    }

    template<SearchStrategy Strategy, BTree BT>
    coroutine::Task<IteratorImpl> search_iterator_impl(BT& t, U64 key) {
        const auto& h = *(co_await read_header(t));
        U32 ns = node_size(t);
        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n = co_await read_node(t, n_ref);
            CountType idx = binary_search_first_gt(keys(n), key);
            n_ref = children(n, ns)[idx];
        }

        const Node* n = co_await read_node(t, n_ref);

        if constexpr (Strategy == SearchStrategy::RequireEquality) {
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count && keys(n)[idx] == key) {
                IteratorImpl it{}; it.leaf=n; it.ref=n_ref; it.idx=idx;
                it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                co_return it;
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::FirstGreaterEqual) {
            CountType idx = binary_search_first_geq(keys(n), key);
            if (idx < n->key_count) {
                IteratorImpl it{}; it.leaf=n; it.ref=n_ref; it.idx=idx;
                it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                co_return it;
            }
            if (n->next != 0) {
                NodeRef next_ref = n->next;
                const Node* next = co_await read_node(t, next_ref);
                IteratorImpl it{}; it.leaf=next; it.ref=next_ref; it.idx=0_u16;
                it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                co_return it;
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::FirstGreater) {
            CountType idx = binary_search_first_gt(keys(n), key);
            if (idx < n->key_count) {
                IteratorImpl it{}; it.leaf=n; it.ref=n_ref; it.idx=idx;
                it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                co_return it;
            }
            if (n->next != 0) {
                NodeRef next_ref = n->next;
                const Node* next = co_await read_node(t, next_ref);
                IteratorImpl it{}; it.leaf=next; it.ref=next_ref; it.idx=0_u16;
                it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                co_return it;
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::LastLessEqual) {
            CountType idx = binary_search_last_leq(keys(n), key);
            if (idx < n->key_count) {
                IteratorImpl it{}; it.leaf=n; it.ref=n_ref; it.idx=idx;
                it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                co_return it;
            }
            if (n->prev != 0) {
                NodeRef prev_ref = n->prev;
                const Node* prev = co_await read_node(t, prev_ref);
                if (prev->key_count > 0) {
                    IteratorImpl it{}; it.leaf=prev; it.ref=prev_ref;
                    it.idx=static_cast<CountType>(prev->key_count - CountType{1});
                    it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                    co_return it;
                }
            }
            co_return IteratorImpl{};
        } else if constexpr (Strategy == SearchStrategy::LastLess) {
            CountType idx = binary_search_last_lt(keys(n), key);
            if (idx < n->key_count) {
                IteratorImpl it{}; it.leaf=n; it.ref=n_ref; it.idx=idx;
                it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                co_return it;
            }
            if (n->prev != 0) {
                NodeRef prev_ref = n->prev;
                const Node* prev = co_await read_node(t, prev_ref);
                if (prev->key_count > 0) {
                    IteratorImpl it{}; it.leaf=prev; it.ref=prev_ref;
                    it.idx=static_cast<CountType>(prev->key_count - CountType{1});
                    it.node_size=ns; it.vp=FixedValuePolicy{.stride=static_cast<U16>(h.value_stride)};
                    co_return it;
                }
            }
            co_return IteratorImpl{};
        }
        co_return IteratorImpl{};
    }
}
