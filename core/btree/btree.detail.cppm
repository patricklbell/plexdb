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
import plexdb.btree.slots;
import plexdb.btree.iterator;

export namespace plexdb::btree {
    // ========================================================================
    // rebalancing helpers
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    void move_from_left(Node* parent, Node* left, Node* node, CountType node_idx,
                        U32 ns, KP kp, VP vp, bool is_leaf) {
        if (is_leaf) {
            auto      left_page = leaf_page(left, ns, kp, vp);
            auto      node_page = leaf_page(node, ns, kp, vp);
            CountType ll        = static_cast<CountType>(left->key_count - 1);
            auto      lk        = key_at(left_page, ll);
            auto      lv        = value_at(left_page, ll);
            assert_true(insert(node_page, 0, lk, lv), "move_from_left: insert (leaf) failed — target leaf unexpectedly full");
            node->key_count = node_page.count;
            internal_set_sep(parent, ns, kp, static_cast<CountType>(node_idx - 1), lk);
            remove(left_page, ll);
            left->key_count = left_page.count;
        } else {
            if constexpr (KP::is_fixed_size) {
                using KT          = typename KP::key_type;
                constexpr auto ks = KP::key_stride;
                os::memory_shift_up(keys<KT>(node));
                node->key_count++;
                os::memory_shift_up(children(node, ns, ks));
                children(node, ns, ks)[0]      = children(left, ns, ks)[left->key_count];
                keys<KT>(node)[0]              = keys<KT>(parent)[node_idx - 1];
                keys<KT>(parent)[node_idx - 1] = keys<KT>(left)[left->key_count - 1];
                left->key_count--;
            } else {
                auto      parent_page = internal_page(parent, ns, kp);
                auto      left_page   = internal_page(left, ns, kp);
                auto      node_page   = internal_page(node, ns, kp);
                CountType ll          = static_cast<CountType>(left->key_count - 1);
                auto      sep         = key_at(parent_page, static_cast<CountType>(node_idx - 1));
                auto      lk          = key_at(left_page, ll);
                NodeRef   lc          = child_at(left_page, left->key_count);
                assert_true(insert_key(node_page, 0, sep), "move_from_left: insert_key failed — target internal node unexpectedly full");
                insert_child(node_page, 0, lc);
                node->key_count = node_page.count;
                replace_key(parent_page, static_cast<CountType>(node_idx - 1), lk);
                remove(left_page, ll);
                left->key_count = left_page.count;
            }
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    void move_from_right(Node* parent, Node* right, Node* node, CountType node_idx,
                         U32 ns, KP kp, VP vp, bool is_leaf) {
        if (is_leaf) {
            auto right_page = leaf_page(right, ns, kp, vp);
            auto node_page  = leaf_page(node, ns, kp, vp);
            auto rk         = key_at(right_page, 0);
            auto rv         = value_at(right_page, 0);
            assert_true(insert(node_page, node_page.count, rk, rv), "move_from_right: insert (leaf) failed — target leaf unexpectedly full");
            node->key_count = node_page.count;
            remove(right_page, 0);
            right->key_count = right_page.count;
            auto new_sep     = key_at(right_page, 0);
            internal_set_sep(parent, ns, kp, node_idx, new_sep);
        } else {
            if constexpr (KP::is_fixed_size) {
                using KT          = typename KP::key_type;
                constexpr auto ks = KP::key_stride;
                node->key_count++;
                children(node, ns, ks)[node->key_count] = children(right, ns, ks)[0];
                keys<KT>(node)[node->key_count - 1]     = keys<KT>(parent)[node_idx];
                keys<KT>(parent)[node_idx]              = keys<KT>(right)[0];
                os::memory_shift_down(view_shift_up(children(right, ns, ks)));
                os::memory_shift_down(view_shift_up(keys<KT>(right)));
                right->key_count--;
            } else {
                auto    parent_page = internal_page(parent, ns, kp);
                auto    right_page  = internal_page(right, ns, kp);
                auto    node_page   = internal_page(node, ns, kp);
                auto    sep         = key_at(parent_page, node_idx);
                auto    rk          = key_at(right_page, 0);
                NodeRef rc          = child_at(right_page, 0);
                assert_true(insert_key(node_page, node_page.count, sep), "move_from_right: insert_key failed — target internal node unexpectedly full");
                insert_child(node_page, node_page.count, rc);
                node->key_count = node_page.count;
                replace_key(parent_page, node_idx, rk);
                remove_front(right_page);
                right->key_count = right_page.count;
            }
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    void merge(Node* a, Node* b, Node* parent, CountType a_idx,
               U32 ns, KP kp, VP vp, bool is_leaf) {
        a->next = b->next;
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            using KT          = typename KP::key_type;
            constexpr auto ks = KP::key_stride;
            constexpr auto vs = VP::value_stride;
            if (!is_leaf) {
                a->key_count++;
                keys<KT>(a)[a->key_count - 1] = keys<KT>(parent)[a_idx];
                os::memory_copy<false>(view_shift_up(children(a, ns, ks), a->key_count), children(b, ns, ks));
            } else {
                os::memory_copy<false>(view_shift_up(values(a, ns, ks, vs), a->key_count), values(b, ns, ks, vs));
            }
            os::memory_copy<false>(view_shift_up(keys<KT>(a), a->key_count), keys<KT>(b));
            a->key_count += b->key_count;
            CountType b_idx = static_cast<CountType>(a_idx + 1);
            os::memory_shift_down(view_shift_up(keys<KT>(parent), b_idx));
            os::memory_shift_down(view_shift_up(children(parent, ns, ks), static_cast<CountType>(b_idx + 1)));
            parent->key_count--;
        } else {
            if (!is_leaf) {
                auto a_page      = internal_page(a, ns, kp);
                auto b_page      = internal_page(b, ns, kp);
                auto parent_page = internal_page(parent, ns, kp);
                auto ps          = key_at(parent_page, a_idx);
                assert_true(insert_key(a_page, a_page.count, ps), "merge: insert_key (separator) failed — merged node unexpectedly full");
                insert_child(a_page, a_page.count, child_at(b_page, 0));
                for (CountType i = 0; i < b->key_count; i++) {
                    assert_true(insert_key(a_page, a_page.count, key_at(b_page, i)), "merge: insert_key (b key) failed — merged node unexpectedly full");
                    insert_child(a_page, a_page.count, child_at(b_page, static_cast<CountType>(i + 1)));
                }
                a->key_count = a_page.count;
                assert_true(a->key_count >= 1, "merge: merged internal node has 0 keys — at least separator must be present");
                remove(parent_page, a_idx);
                parent->key_count = parent_page.count;
            } else {
                auto a_page      = leaf_page(a, ns, kp, vp);
                auto b_page      = leaf_page(b, ns, kp, vp);
                auto parent_page = internal_page(parent, ns, kp);
                for (CountType i = 0; i < b->key_count; i++) {
                    assert_true(insert(a_page, a_page.count, key_at(b_page, i), value_at(b_page, i)), "merge: insert (leaf) failed — merged leaf unexpectedly full");
                }
                a->key_count = a_page.count;
                remove(parent_page, a_idx);
                parent->key_count = parent_page.count;
            }
        }
    }

    // ========================================================================
    // insert
    // ========================================================================
    template<BTree BT>
    coroutine::Task<void> insert_child_to_right(BT& t, U32 ns, Node* parent, CountType idx,
                                                TArrayView<const U8, U16> key_bytes,
                                                NodeRef child_ref, Node* child) {
        auto kp = key_policy(t);
        internal_insert_entry(parent, ns, kp, idx, key_bytes, child_ref);
        NodeRef prev_ref = internal_children(parent, ns, kp)[idx];
        Node*   prev     = co_await update_node(t, prev_ref);
        child->next      = prev->next;
        child->prev      = prev_ref;
        prev->next       = child_ref;
    }

    template<BTree BT, KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<void> split_child(BT& t, U32 ns, KP kp, VP vp, Node* parent,
                                      CountType child_idx, bool is_child_leaf) {
        ZoneScopedN("btree::split");

        NodeRef left_ref = internal_children(parent, ns, kp)[child_idx];
        Node*   left     = co_await update_node(t, left_ref);

        NodeRef right_ref;
        if (is_child_leaf) {
            right_ref = co_await create_leaf(t);
        } else {
            right_ref = co_await create_internal(t);
        }
        Node* right = co_await update_node(t, right_ref);

        TArrayView<const U8, U16> sep_key_bytes;
        if (is_child_leaf) {
            CountType m   = split_index_leaf(left, ns, kp, vp);
            sep_key_bytes = leaf_get_key_bytes(left, ns, kp, vp, m);
            leaf_copy_suffix(left, right, ns, kp, vp, m);
            left->key_count = m;
            assert_true(left->key_count >= 1, "split_child: leaf left half has 0 keys after split");
        } else {
            CountType m = split_index_internal(left, ns, kp);
            assert_true(m >= 2, "split_child: split_index_internal returned < 2, would leave internal left half empty");
            assert_true(m < left->key_count, "split_child: split_index_internal returned left->key_count, would leave right half empty");
            sep_key_bytes = internal_get_key_bytes(left, ns, kp, static_cast<CountType>(m - 1));
            internal_copy_suffix(left, right, ns, kp, m);
            left->key_count = static_cast<CountType>(m - 1);
            assert_true(left->key_count >= 1, "split_child: internal left half has 0 keys after split");
            assert_true(right->key_count >= 1, "split_child: internal right half has 0 keys after copy");
        }

        co_await insert_child_to_right(t, ns, parent, child_idx, sep_key_bytes, right_ref, right);
    }

    template<BTree BT, KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<bool> insert_recursive(BT& t, const Header& h, U32 ns, KP kp, VP vp,
                                           NodeRef n_ref, CountType depth,
                                           BTreeKeyType<BT> key, TArrayView<const U8, U16> value) {
        if (depth == h.depth) {
            Node*     n      = co_await update_node(t, n_ref);
            CountType idx    = leaf_bsearch_geq(n, ns, kp, vp, key);
            bool      is_new = leaf_upsert(n, ns, kp, vp, idx, key, value);
            if (is_new) {
                (co_await update_header(t))->size++;
            }
            co_return is_new;
        }

        bool is_child_leaf = (depth + 1 == h.depth);
        U16  key_bytes     = stored_key_size(kp, key);
        U16  val_bytes     = static_cast<U16>(value.length);

        NodeRef child_ref;
        {
            const Node* n         = co_await read_node(t, n_ref);
            CountType   child_idx = internal_bsearch_gt(n, ns, kp, key);
            child_ref             = internal_children(n, ns, kp)[child_idx];
            const Node* child     = co_await read_node(t, child_ref);

            bool child_full = is_child_leaf
                                  ? is_leaf_full(child, ns, kp, vp, key_bytes, val_bytes)
                                  : is_internal_full(child, ns, kp, key_bytes);

            if (child_full) {
                co_await split_child(t, ns, kp, vp, co_await update_node(t, n_ref), child_idx, is_child_leaf);
                // Re-read parent to find correct child after split
                const Node* n2       = co_await read_node(t, n_ref);
                auto        sep      = internal_get_key(n2, ns, kp, child_idx);
                bool        go_right = compare_key(kp, key, sep) != Ordering::Less;
                if (go_right) {
                    child_idx = static_cast<CountType>(child_idx + 1);
                }
                child_ref = internal_children(n2, ns, kp)[child_idx];
            }
        }

        co_return co_await insert_recursive(t, h, ns, kp, vp, child_ref,
                                            static_cast<CountType>(depth + 1), key, value);
    }

    template<BTree BT>
    coroutine::Task<bool> insert_impl(BT& t, BTreeKeyType<BT> key, TArrayView<const U8, U16> value) {
        ZoneScopedN("btree::insert");
        auto kp = key_policy(t);
        auto vp = value_policy(t);

        U32 ns        = node_size(t);
        U16 key_bytes = stored_key_size(kp, key);
        U16 val_bytes = static_cast<U16>(value.length);

        {
            const auto& h0           = *(co_await read_header(t));
            bool        is_root_leaf = h0.depth == 0;
            const Node* root         = co_await read_node(t, h0.root);
            bool        root_full    = is_root_leaf
                                           ? is_leaf_full(root, ns, kp, vp, key_bytes, val_bytes)
                                           : is_internal_full(root, ns, kp, key_bytes);
            if (root_full) {
                NodeRef new_root_ref                   = co_await create_internal(t);
                Node*   new_root                       = co_await update_node(t, new_root_ref);
                internal_children(new_root, ns, kp)[0] = h0.root;
                co_await split_child(t, ns, kp, vp, new_root, 0, is_root_leaf);
                auto& hw = *(co_await update_header(t));
                hw.root  = new_root_ref;
                hw.depth++;
            }
        }
        // @note re-read header after potential root split so insert_recursive uses the updated root/depth.
        const auto& h = *(co_await read_header(t));
        co_return co_await insert_recursive(t, h, ns, kp, vp, h.root, 0, key, value);
    }

    // ========================================================================
    // search
    // ========================================================================
    template<bool Mutable>
    struct Search {
        Conditional<Mutable, Node*, const Node*> leaf         = nullptr;
        CountType                                idx          = 0;
        Conditional<Mutable, U8*, const U8*>     value        = nullptr;
        U16                                      value_length = 0;
        [[nodiscard]] constexpr explicit         operator bool() const noexcept {
            return leaf != nullptr;
        }
    };

    template<bool Mutable, BTree BT>
    coroutine::Task<Search<Mutable>> search_impl(BT& t, BTreeKeyType<BT> key) {
        ZoneScopedN("btree::search");
        auto kp = key_policy(t);
        auto vp = value_policy(t);

        const auto& h  = *(co_await read_header(t));
        U32         ns = node_size(t);

        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n   = co_await read_node(t, n_ref);
            CountType   idx = internal_bsearch_gt(n, ns, kp, key);
            n_ref           = internal_children(n, ns, kp)[idx];
        }

        if constexpr (Mutable) {
            Node*     n   = co_await update_node(t, n_ref);
            CountType idx = leaf_bsearch_geq(n, ns, kp, vp, key);
            if (leaf_key_eq(n, ns, kp, vp, idx, key)) {
                auto v = leaf_value_mut(n, ns, kp, vp, idx);
                co_return Search<true>{n, idx, v.ptr, v.length};
            }
            co_return Search<true>{};
        } else {
            const Node* n   = co_await read_node(t, n_ref);
            CountType   idx = leaf_bsearch_geq(n, ns, kp, vp, key);
            if (leaf_key_eq(n, ns, kp, vp, idx, key)) {
                auto v = leaf_value_const(n, ns, kp, vp, idx);
                co_return Search<false>{n, idx, v.ptr, v.length};
            }
            co_return Search<false>{};
        }
    }

    // ========================================================================
    // remove
    // ========================================================================
    struct RemoveStackItem {
        NodeRef   node;
        CountType idx;
    };

    template<BTree BT>
    coroutine::Task<bool> remove_impl(BT& t, BTreeKeyType<BT> key) {
        ZoneScopedN("btree::remove");
        auto kp = key_policy(t);
        auto vp = value_policy(t);

        const auto& h  = *(co_await read_header(t));
        U32         ns = node_size(t);

        Stack<RemoveStackItem> stack{};
        Arena                  stack_arena{sizeof(decltype(stack)::Node) * h.depth};
        NodeRef                node_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* node = co_await read_node(t, node_ref);
            assert_true(node->key_count >= 1, "remove_impl: internal node has key_count=0 — btree is corrupt");
            CountType idx = internal_bsearch_gt(node, ns, kp, key);
            push_front(stack_arena, stack, RemoveStackItem{node_ref, idx});
            node_ref = internal_children(node, ns, kp)[idx];
        }

        CountType idx;
        {
            const Node* node = co_await read_node(t, node_ref);
            idx              = leaf_bsearch_geq(node, ns, kp, vp, key);
            if (!leaf_key_eq(node, ns, kp, vp, idx, key)) {
                co_return false;
            }
        }

        Node* node = co_await update_node(t, node_ref);
        leaf_remove(node, ns, kp, vp, idx);

        bool is_leaf = true;
        while (true) {
            bool underfull = is_leaf
                                 ? is_underfull_leaf(node, ns, kp, vp)
                                 : is_underfull_internal(node, ns, kp);
            if (!underfull) {
                break;
            }

            if (stack.length == 0) {
                if (node->key_count != 0 || h.depth == 0) {
                    break;
                }
                NodeRef new_root = internal_children(node, ns, kp)[0];
                co_await delete_node(t, node_ref);
                auto& hw = *(co_await update_header(t));
                hw.root  = new_root;
                hw.depth--;
                hw.size--;
                co_return true;
            }

            RemoveStackItem* item = front(stack);
            pop_front(stack);
            NodeRef&   parent_ref = item->node;
            CountType& item_idx   = item->idx;
            Node*      parent     = co_await update_node(t, parent_ref);

            NodeRef left_ref = (item_idx > 0)
                                   ? internal_children(parent, ns, kp)[item_idx - 1]
                                   : 0;
            Node*   left     = nullptr;
            if (left_ref != 0) {
                left = co_await update_node(t, left_ref);
            }
            if (left && !(is_leaf
                              ? is_underfull_leaf(left, ns, kp, vp)
                              : is_underfull_internal(left, ns, kp))) {
                move_from_left(parent, left, node, item_idx, ns, kp, vp, is_leaf);
                break;
            }

            NodeRef right_ref = (item_idx < parent->key_count)
                                    ? internal_children(parent, ns, kp)[item_idx + 1]
                                    : 0;
            Node*   right     = nullptr;
            if (right_ref != 0) {
                right = co_await update_node(t, right_ref);
            }
            if (right && !(is_leaf
                               ? is_underfull_leaf(right, ns, kp, vp)
                               : is_underfull_internal(right, ns, kp))) {
                move_from_right(parent, right, node, item_idx, ns, kp, vp, is_leaf);
                break;
            }

            assert_true(left || right, "node must have at least one sibling.");

            if (left) {
                merge(left, node, parent, static_cast<CountType>(item_idx - 1), ns, kp, vp, is_leaf);
                co_await delete_node(t, node_ref);
            } else {
                merge(node, right, parent, item_idx, ns, kp, vp, is_leaf);
                co_await delete_node(t, right_ref);
            }

            node_ref = parent_ref;
            node     = parent;
            is_leaf  = false;
        }

        (co_await update_header(t))->size--;
        co_return true;
    }

    template<BTree BT>
    coroutine::Task<void> truncate_impl(BT& t) {
        auto& h  = *(co_await update_header(t));
        U32   ns = node_size(t);
        auto  kp = key_policy(t);

        Node* n_root = co_await update_node(t, h.root);
        if (h.depth > 0) {
            NodeRef curr_ref = internal_children(n_root, ns, kp)[0];
            for (CountType depth = 1; depth <= h.depth; depth++) {
                const Node* curr = co_await read_node(t, curr_ref);
                if (curr->prev == 0) {
                    while (curr->next != 0) {
                        NodeRef next_ref = curr->next;
                        co_await delete_node(t, curr_ref);
                        curr_ref = next_ref;
                        curr     = co_await read_node(t, curr_ref);
                    }
                    if (depth != h.depth) {
                        NodeRef right_most = internal_children(curr, ns, kp)[curr->key_count];
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
                        curr     = co_await read_node(t, curr_ref);
                    }
                    if (depth != h.depth) {
                        NodeRef left_most = internal_children(curr, ns, kp)[0];
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
        Node*   new_root     = co_await update_node(t, new_root_ref);
        new_root->key_count  = 0;
        new_root->prev       = 0;
        new_root->next       = 0;

        auto& hw  = *(co_await update_header(t));
        hw.root   = new_root_ref;
        hw.leaves = new_root_ref;
        hw.size   = 0;
        hw.depth  = 0;
        co_await delete_node(t, old_root_ref);
    }

    // ========================================================================
    // iterators
    // ========================================================================
    template<BTree BT>
    coroutine::Task<IteratorImpl<BTreeKP<BT>, BTreeVP<BT>>> begin_iterator_impl(BT& t) {
        using Impl    = IteratorImpl<BTreeKP<BT>, BTreeVP<BT>>;
        const auto& h = *(co_await read_header(t));
        if (h.size == 0) {
            co_return Impl{};
        }

        auto    kp    = key_policy(t);
        auto    vp    = value_policy(t);
        U32     ns    = node_size(t);
        NodeRef n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n = co_await read_node(t, n_ref);
            assert_true(n->key_count > 0, "internal node must have at least one key");
            n_ref = internal_children(n, ns, kp)[0];
        }

        const Node* n = co_await read_node(t, n_ref);
        assert_true(n->key_count > 0, "leaf node must have at least one value");

        Impl it{};
        it.node_size = ns;
        it.leaf_buf  = UniquePtr<U8>{os::allocate(ns)};
        os::memory_copy(it.leaf_buf.ptr, n, ns);
        it.leaf = reinterpret_cast<const Node*>(it.leaf_buf.ptr);
        it.ref  = n_ref;
        it.idx  = 0_u16;
        it.kp   = kp;
        it.vp   = vp;
        co_return move(it);
    }

    // Advances the iterator in place; opens no transaction — caller must ensure one is
    // active if the iterator is backed by a paged btree.
    template<BTree BT>
    coroutine::Task<void> next_iterator_inplace(
        BT& t, IteratorImpl<BTreeKP<BT>, BTreeVP<BT>>& it) {
        assert_true(it.leaf != nullptr, "cannot advance past end");

        if (it.idx == it.leaf->key_count - 1) {
            if (it.leaf->next != 0) {
                NodeRef     next_ref  = it.leaf->next;
                const Node* next_node = co_await read_node(t, next_ref);
                os::memory_copy(it.leaf_buf.ptr, next_node, it.node_size);
                it.leaf = reinterpret_cast<const Node*>(it.leaf_buf.ptr);
                it.ref  = next_ref;
                it.idx  = 0_u16;
            } else {
                it.leaf_buf = {};
                it.leaf     = nullptr;
                it.ref      = 0;
                it.idx      = 0;
            }
            co_return;
        }

        it.idx = static_cast<CountType>(it.idx + 1_u16);
    }

    template<SearchStrategy Strategy, BTree BT>
    coroutine::Task<IteratorImpl<BTreeKP<BT>, BTreeVP<BT>>> search_iterator_impl(
        BT& t, BTreeKeyType<BT> key) {
        using Impl        = IteratorImpl<BTreeKP<BT>, BTreeVP<BT>>;
        const auto& h     = *(co_await read_header(t));
        auto        kp    = key_policy(t);
        auto        vp    = value_policy(t);
        U32         ns    = node_size(t);
        NodeRef     n_ref = h.root;
        for (CountType depth = 0; depth < h.depth; depth++) {
            const Node* n   = co_await read_node(t, n_ref);
            CountType   idx = internal_bsearch_gt(n, ns, kp, key);
            n_ref           = internal_children(n, ns, kp)[idx];
        }

        const Node* n     = co_await read_node(t, n_ref);
        auto        mk_it = [&](const Node* src, NodeRef ref, CountType i) {
            Impl it{};
            it.node_size = ns;
            it.leaf_buf  = UniquePtr<U8>{os::allocate(ns)};
            os::memory_copy(it.leaf_buf.ptr, src, ns);
            it.leaf = reinterpret_cast<const Node*>(it.leaf_buf.ptr);
            it.ref  = ref;
            it.idx  = i;
            it.kp   = kp;
            it.vp   = vp;
            return it;
        };

        if constexpr (Strategy == SearchStrategy::RequireEquality) {
            CountType idx = leaf_bsearch_geq(n, ns, kp, vp, key);
            if (leaf_key_eq(n, ns, kp, vp, idx, key)) {
                co_return mk_it(n, n_ref, idx);
            }
            co_return {};
        } else if constexpr (Strategy == SearchStrategy::FirstGreaterEqual) {
            CountType idx = leaf_bsearch_geq(n, ns, kp, vp, key);
            if (idx < n->key_count) {
                co_return mk_it(n, n_ref, idx);
            }
            if (n->next != 0) {
                NodeRef     nr = n->next;
                const Node* nn = co_await read_node(t, nr);
                co_return mk_it(nn, nr, 0_u16);
            }
            co_return {};
        } else if constexpr (Strategy == SearchStrategy::FirstGreater) {
            CountType idx = leaf_bsearch_gt(n, ns, kp, vp, key);
            if (idx < n->key_count) {
                co_return mk_it(n, n_ref, idx);
            }
            if (n->next != 0) {
                NodeRef     nr = n->next;
                const Node* nn = co_await read_node(t, nr);
                co_return mk_it(nn, nr, 0_u16);
            }
            co_return {};
        } else if constexpr (Strategy == SearchStrategy::LastLessEqual) {
            CountType idx = leaf_bsearch_leq(n, ns, kp, vp, key);
            if (idx < n->key_count) {
                co_return mk_it(n, n_ref, idx);
            }
            if (n->prev != 0) {
                NodeRef     pr = n->prev;
                const Node* pp = co_await read_node(t, pr);
                if (pp->key_count > 0) {
                    co_return mk_it(pp, pr, static_cast<CountType>(pp->key_count - CountType{1}));
                }
            }
            co_return {};
        } else if constexpr (Strategy == SearchStrategy::LastLess) {
            CountType idx = leaf_bsearch_lt(n, ns, kp, vp, key);
            if (idx < n->key_count) {
                co_return mk_it(n, n_ref, idx);
            }
            if (n->prev != 0) {
                NodeRef     pr = n->prev;
                const Node* pp = co_await read_node(t, pr);
                if (pp->key_count > 0) {
                    co_return mk_it(pp, pr, static_cast<CountType>(pp->key_count - CountType{1}));
                }
            }
            co_return {};
        }
        co_return {};
    }
}
