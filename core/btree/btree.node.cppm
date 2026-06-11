export module plexdb.btree.node;

import plexdb.base;
import plexdb.arena;
import plexdb.os;

import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.btree.slots;

export namespace plexdb::btree {
    // ========================================================================
    // capacity helpers
    // ========================================================================
    inline CountType max_keys_internal(U32 node_size, U16 key_stride) noexcept {
        assert_true(node_size >= sizeof(Node) + key_stride + 2*sizeof(NodeRef),
                    "enough bytes for at least one key in internal node");
        U64 count = min(
            static_cast<U64>((node_size - sizeof(Node) - sizeof(NodeRef)) / (key_stride + sizeof(NodeRef))),
            static_cast<U64>(NumericLimits<CountType>::max())
        );
        return static_cast<CountType>(count);
    }

    inline CountType max_keys_leaf(U32 node_size, U16 key_stride, U16 val_stride) noexcept {
        assert_true(node_size >= sizeof(Node) + key_stride + val_stride,
                    "enough bytes for at least one key in leaf node");
        U64 count = min(
            static_cast<U64>((node_size - sizeof(Node)) / (key_stride + val_stride)),
            static_cast<U64>(NumericLimits<CountType>::max())
        );
        return static_cast<CountType>(count);
    }

    // ========================================================================
    // raw pointer accessors (fixed layout)
    // ========================================================================
    template<typename T>
    T* keys_raw(Node* node) noexcept {
        return reinterpret_cast<T*>(reinterpret_cast<U8*>(node) + sizeof(Node));
    }
    template<typename T>
    const T* keys_raw(const Node* node) noexcept {
        return reinterpret_cast<const T*>(reinterpret_cast<const U8*>(node) + sizeof(Node));
    }

    inline NodeRef* children_raw(Node* node, U32 node_size, U16 key_stride) noexcept {
        CountType mk = max_keys_internal(node_size, key_stride);
        return reinterpret_cast<NodeRef*>(reinterpret_cast<U8*>(node) + sizeof(Node) + mk * key_stride);
    }
    inline const NodeRef* children_raw(const Node* node, U32 node_size, U16 key_stride) noexcept {
        CountType mk = max_keys_internal(node_size, key_stride);
        return reinterpret_cast<const NodeRef*>(reinterpret_cast<const U8*>(node) + sizeof(Node) + mk * key_stride);
    }

    inline U8* values_raw(Node* node, U32 node_size, U16 key_stride, U16 val_stride) noexcept {
        CountType mk = max_keys_leaf(node_size, key_stride, val_stride);
        return reinterpret_cast<U8*>(node) + sizeof(Node) + mk * key_stride;
    }
    inline const U8* values_raw(const Node* node, U32 node_size, U16 key_stride, U16 val_stride) noexcept {
        CountType mk = max_keys_leaf(node_size, key_stride, val_stride);
        return reinterpret_cast<const U8*>(node) + sizeof(Node) + mk * key_stride;
    }

    // ========================================================================
    // view accessors (fixed layout)
    // ========================================================================
    template<typename T>
    TArrayView<T, CountType> keys(Node* node) noexcept {
        return {keys_raw<T>(node), node->key_count};
    }
    template<typename T>
    TArrayView<const T, CountType> keys(const Node* node) noexcept {
        return {keys_raw<T>(node), node->key_count};
    }

    inline TArrayView<NodeRef, CountType> children(Node* node, U32 node_size, U16 key_stride) noexcept {
        return {children_raw(node, node_size, key_stride), static_cast<CountType>(node->key_count + 1)};
    }
    inline TArrayView<const NodeRef, CountType> children(const Node* node, U32 node_size, U16 key_stride) noexcept {
        return {children_raw(node, node_size, key_stride), static_cast<CountType>(node->key_count + 1)};
    }

    inline ArrayView<CountType, U16, U8> values(Node* node, U32 node_size, U16 key_stride, U16 val_stride) noexcept {
        return ArrayView{values_raw(node, node_size, key_stride, val_stride), val_stride, node->key_count};
    }
    inline ArrayView<CountType, U16, const U8> values(const Node* node, U32 node_size, U16 key_stride, U16 val_stride) noexcept {
        return ArrayView{values_raw(node, node_size, key_stride, val_stride), val_stride, node->key_count};
    }

    // ========================================================================
    // policy-parameterized slot accessors
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
        requires (KP::is_fixed_size && VP::is_fixed_size)
    FixedLeafSlots leaf_page(Node* node, U32 node_size, KP, VP) noexcept {
        CountType cap = max_keys_leaf(node_size, KP::key_stride, VP::value_stride);
        return FixedLeafSlots{
            .base       = reinterpret_cast<U8*>(node) + sizeof(Node),
            .count      = node->key_count,
            .capacity   = cap,
            .key_stride = KP::key_stride,
            .val_stride = VP::value_stride,
        };
    }

    template<KeyPolicy KP, ValuePolicy VP>
        requires (KP::is_fixed_size && VP::is_fixed_size)
    FixedLeafSlots leaf_page(const Node* node, U32 node_size, KP kp, VP vp) noexcept {
        return leaf_page(const_cast<Node*>(node), node_size, kp, vp);
    }

    template<KeyPolicy KP>
        requires KP::is_fixed_size
    FixedInternalSlots internal_page(Node* node, U32 node_size, KP) noexcept {
        CountType cap = max_keys_internal(node_size, KP::key_stride);
        return FixedInternalSlots{
            .base       = reinterpret_cast<U8*>(node) + sizeof(Node),
            .count      = node->key_count,
            .capacity   = cap,
            .key_stride = KP::key_stride,
        };
    }

    template<KeyPolicy KP>
        requires KP::is_fixed_size
    FixedInternalSlots internal_page(const Node* node, U32 node_size, KP kp) noexcept {
        return internal_page(const_cast<Node*>(node), node_size, kp);
    }

    // Varlen leaf page
    template<KeyPolicy KP, ValuePolicy VP>
        requires (!KP::is_fixed_size)
    SlottedLeafPage<true> leaf_page(Node* node, U32 node_size, KP, VP) noexcept {
        using Page = SlottedLeafPage<true>;
        U8* base = reinterpret_cast<U8*>(node) + sizeof(Node);
        U16 page_cap = static_cast<U16>(node_size - sizeof(Node));
        CountType n = node->key_count;
        U16 slot_end = static_cast<U16>(n * sizeof(typename Page::Entry));
        Page p{};
        p.base       = base;
        p.count      = n;
        p.page_size  = page_cap;
        p.key_stride = 0;
        p.slot_end   = slot_end;
        if (n == 0) {
            p.data_low = page_cap;
        } else {
            p.data_low = page_cap;
            const auto* slots = p.slots();
            for (CountType i = 0; i < n; i++) {
                const auto& e = slots[i];
                if (e.key_off < p.data_low) p.data_low = e.key_off;
                if (e.val_off < p.data_low) p.data_low = e.val_off;
            }
        }
        return p;
    }

    template<KeyPolicy KP, ValuePolicy VP>
        requires (!KP::is_fixed_size)
    SlottedLeafPage<true> leaf_page(const Node* node, U32 node_size, KP kp, VP vp) noexcept {
        return leaf_page(const_cast<Node*>(node), node_size, kp, vp);
    }

    // Varlen internal page
    template<KeyPolicy KP>
        requires (!KP::is_fixed_size)
    SlottedInternalPage<true> internal_page(Node* node, U32 node_size, KP) noexcept {
        using Page = SlottedInternalPage<true>;
        U8* base = reinterpret_cast<U8*>(node) + sizeof(Node);
        U16 page_cap = static_cast<U16>(node_size - sizeof(Node));
        CountType max_children = static_cast<CountType>(
            (page_cap) / (sizeof(SlotEntry<true,false>) + 1 + sizeof(NodeRef)) + 1
        );
        U16 children_region = static_cast<U16>(max_children * sizeof(NodeRef));
        U16 usable_cap = static_cast<U16>(page_cap - children_region);
        CountType n = node->key_count;
        U16 slot_end = static_cast<U16>(n * sizeof(SlotEntry<true,false>));
        Page p{};
        p.base         = base;
        p.count        = n;
        p.page_size    = page_cap;
        p.key_stride   = 0;
        p.max_children = max_children;
        p.slot_end     = slot_end;
        if (n == 0) {
            p.data_low = usable_cap;
        } else {
            p.data_low = usable_cap;
            const auto* slots = p.slots();
            for (CountType i = 0; i < n; i++) {
                if (slots[i].key_off < p.data_low) p.data_low = slots[i].key_off;
            }
        }
        return p;
    }

    template<KeyPolicy KP>
        requires (!KP::is_fixed_size)
    SlottedInternalPage<true> internal_page(const Node* node, U32 node_size, KP kp) noexcept {
        return internal_page(const_cast<Node*>(node), node_size, kp);
    }

    // ========================================================================
    // binary search on nodes
    // ========================================================================
    template<BinarySearchPolicy Policy, KeyPolicy KP, ValuePolicy VP>
    CountType leaf_bsearch(const Node* node, U32 ns, KP kp, VP vp,
                           typename KP::key_type key) noexcept {
        if constexpr (KP::is_fixed_size) {
            return binary_search<Policy>(keys<typename KP::key_type>(node).cbegin(),
                                         node->key_count, key);
        } else {
            auto page = leaf_page(node, ns, kp, vp);
            CountType lo = 0, hi = page.count;
            while (lo < hi) {
                CountType mid = static_cast<CountType>((lo + hi) / 2);
                auto k = key_at(page, mid);
                auto kval = read_key(kp, k.ptr, k.length);
                auto ord = compare_key(kp, kval, key);
                if constexpr (Policy == BinarySearchPolicy::GreaterEqual) {
                    if (ord == Ordering::Less) lo = static_cast<CountType>(mid + 1);
                    else hi = mid;
                } else {
                    if (ord != Ordering::Greater) lo = static_cast<CountType>(mid + 1);
                    else hi = mid;
                }
            }
            return lo;
        }
    }

    template<BinarySearchPolicy Policy, KeyPolicy KP>
    CountType internal_bsearch(const Node* node, U32 ns, KP kp,
                                typename KP::key_type key) noexcept {
        if constexpr (KP::is_fixed_size) {
            return binary_search<Policy>(keys<typename KP::key_type>(node).cbegin(),
                                         node->key_count, key);
        } else {
            auto page = internal_page(node, ns, kp);
            CountType lo = 0, hi = page.count;
            while (lo < hi) {
                CountType mid = static_cast<CountType>((lo + hi) / 2);
                auto k = key_at(page, mid);
                auto kval = read_key(kp, k.ptr, k.length);
                auto ord = compare_key(kp, kval, key);
                if constexpr (Policy == BinarySearchPolicy::GreaterEqual) {
                    if (ord == Ordering::Less) lo = static_cast<CountType>(mid + 1);
                    else hi = mid;
                } else {
                    if (ord != Ordering::Greater) lo = static_cast<CountType>(mid + 1);
                    else hi = mid;
                }
            }
            return lo;
        }
    }

    // named wrappers
    template<KeyPolicy KP, ValuePolicy VP>
    CountType leaf_bsearch_geq(const Node* node, U32 ns, KP kp, VP vp,
                                typename KP::key_type key) noexcept {
        return leaf_bsearch<BinarySearchPolicy::GreaterEqual>(node, ns, kp, vp, key);
    }

    template<KeyPolicy KP, ValuePolicy VP>
    CountType leaf_bsearch_gt(const Node* node, U32 ns, KP kp, VP vp,
                               typename KP::key_type key) noexcept {
        return leaf_bsearch<BinarySearchPolicy::Greater>(node, ns, kp, vp, key);
    }

    template<KeyPolicy KP, ValuePolicy VP>
    CountType leaf_bsearch_leq(const Node* node, U32 ns, KP kp, VP vp,
                                typename KP::key_type key) noexcept {
        auto idx = leaf_bsearch<BinarySearchPolicy::Greater>(node, ns, kp, vp, key);
        return idx == 0 ? node->key_count : static_cast<CountType>(idx - 1);
    }

    template<KeyPolicy KP, ValuePolicy VP>
    CountType leaf_bsearch_lt(const Node* node, U32 ns, KP kp, VP vp,
                               typename KP::key_type key) noexcept {
        auto idx = leaf_bsearch<BinarySearchPolicy::GreaterEqual>(node, ns, kp, vp, key);
        return idx == 0 ? node->key_count : static_cast<CountType>(idx - 1);
    }

    template<KeyPolicy KP>
    CountType internal_bsearch_gt(const Node* node, U32 ns, KP kp,
                                   typename KP::key_type key) noexcept {
        return internal_bsearch<BinarySearchPolicy::Greater>(node, ns, kp, key);
    }

    // ========================================================================
    // generic internal node helpers
    // ========================================================================
    template<KeyPolicy KP>
    NodeRef* internal_children(Node* node, U32 ns, KP kp) noexcept {
        if constexpr (KP::is_fixed_size) {
            return children_raw(node, ns, KP::key_stride);
        } else {
            return internal_page(node, ns, kp).children();
        }
    }

    template<KeyPolicy KP>
    const NodeRef* internal_children(const Node* node, U32 ns, KP kp) noexcept {
        if constexpr (KP::is_fixed_size) {
            return children_raw(node, ns, KP::key_stride);
        } else {
            return internal_page(node, ns, kp).children();
        }
    }

    template<KeyPolicy KP>
    typename KP::key_type internal_get_key(const Node* node, U32 ns, KP kp,
                                            CountType i) noexcept {
        if constexpr (KP::is_fixed_size) {
            return keys<typename KP::key_type>(node)[i];
        } else {
            auto page = internal_page(node, ns, kp);
            auto k = key_at(page, i);
            return read_key(kp, k.ptr, k.length);
        }
    }

    template<KeyPolicy KP>
    void internal_set_sep(Node* node, U32 ns, KP kp, CountType i,
                          TArrayView<const U8, U16> key_bytes) noexcept {
        auto page = internal_page(node, ns, kp);
        replace_key(page, i, key_bytes);
    }

    template<KeyPolicy KP>
    void internal_insert_entry(Node* node, U32 ns, KP kp, CountType idx,
                               TArrayView<const U8, U16> key_bytes, NodeRef right_child) noexcept {
        auto page = internal_page(node, ns, kp);
        bool ok = insert_key(page, idx, key_bytes);
        assert_true(ok, "internal_insert_entry: insert_key failed — internal node unexpectedly full");
        insert_child(page, idx + 1, right_child);
        if constexpr (KP::is_fixed_size) node->key_count++;
        else node->key_count = page.count;
    }

    template<KeyPolicy KP>
    void internal_remove_entry(Node* node, U32 ns, KP kp, CountType idx) noexcept {
        auto page = internal_page(node, ns, kp);
        remove(page, idx);
        node->key_count = page.count;
    }

    template<KeyPolicy KP>
    void internal_copy_suffix(const Node* src, Node* dst, U32 ns, KP kp, CountType from) noexcept {
        auto sp = internal_page(src, ns, kp);
        auto dp = internal_page(dst, ns, kp);
        copy_suffix_to(sp, from, dp);
        dst->key_count = dp.count;
    }

    // bytes of internal key at index i (raw bytes, for passing to parent during split)
    template<KeyPolicy KP>
    TArrayView<const U8, U16> internal_get_key_bytes(const Node* node, U32 ns, KP kp,
                                                      CountType i) noexcept {
        auto page = internal_page(node, ns, kp);
        return key_at(page, i);
    }

    // ========================================================================
    // generic leaf node helpers
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    bool leaf_key_eq(const Node* node, U32 ns, KP kp, VP vp,
                     CountType idx, typename KP::key_type key) noexcept {
        if constexpr (KP::is_fixed_size) {
            return idx < node->key_count && keys<typename KP::key_type>(node)[idx] == key;
        } else {
            if (idx >= node->key_count) return false;
            auto page = leaf_page(node, ns, kp, vp);
            auto k = key_at(page, idx);
            auto kval = read_key(kp, k.ptr, k.length);
            return compare_key(kp, kval, key) == Ordering::Equal;
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    TArrayView<const U8, U16> leaf_get_key_bytes(const Node* node, U32 ns, KP kp, VP vp,
                                                   CountType idx) noexcept {
        if constexpr (KP::is_fixed_size) {
            return {reinterpret_cast<const U8*>(keys_raw<typename KP::key_type>(node))
                    + idx * KP::key_stride, KP::key_stride};
        } else {
            auto page = leaf_page(node, ns, kp, vp);
            return key_at(page, idx);
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    TArrayView<U8, U16> leaf_value_mut(Node* node, U32 ns, KP kp, VP vp,
                                        CountType idx) noexcept {
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            return {values_raw(node, ns, KP::key_stride, VP::value_stride) + idx * VP::value_stride, VP::value_stride};
        } else {
            auto page = leaf_page(node, ns, kp, vp);
            return value_at(page, idx);
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    TArrayView<const U8, U16> leaf_value_const(const Node* node, U32 ns, KP kp, VP vp,
                                                CountType idx) noexcept {
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            return {values_raw(node, ns, KP::key_stride, VP::value_stride) + idx * VP::value_stride, VP::value_stride};
        } else {
            auto page = leaf_page(node, ns, kp, vp);
            return value_at(page, idx);
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    bool leaf_upsert(Node* node, U32 ns, KP kp, VP vp,
                     CountType idx, typename KP::key_type key,
                     TArrayView<const U8, U16> val_bytes) noexcept {
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            using KT = typename KP::key_type;
            constexpr U16 ks = KP::key_stride;
            constexpr U16 vs = VP::value_stride;
            assert_true(val_bytes.length <= vs, "value too large for fixed stride");
            auto write_val = [&](U8* dst) {
                os::memory_copy(dst, val_bytes.ptr, val_bytes.length);
                os::memory_zero(dst + val_bytes.length, vs - val_bytes.length);
            };
            if (idx < node->key_count && keys<KT>(node)[idx] == key) {
                write_val(values_raw(node, ns, ks, vs) + idx * vs);
                return false;
            }
            if (idx < node->key_count) {
                os::memory_shift_up(view_shift_up(keys<KT>(node), idx));
                os::memory_shift_up(view_shift_up(values(node, ns, ks, vs), idx));
            }
            node->key_count++;
            keys<KT>(node)[idx] = key;
            write_val(values_raw(node, ns, ks, vs) + idx * vs);
            return true;
        } else {
            auto page = leaf_page(node, ns, kp, vp);
            if (idx < page.count) {
                auto k = key_at(page, idx);
                auto kval = read_key(kp, k.ptr, k.length);
                if (compare_key(kp, kval, key) == Ordering::Equal) {
                    if constexpr (VP::is_fixed_size) {
                        assert_true(val_bytes.length <= VP::value_stride, "value too large for fixed stride");
                        auto v = value_at(page, idx);
                        os::memory_copy(v.ptr, val_bytes.ptr, val_bytes.length);
                        os::memory_zero(v.ptr + val_bytes.length, VP::value_stride - val_bytes.length);
                    } else {
                        remove(page, idx);
                        insert(page, idx, key, val_bytes);
                    }
                    node->key_count = page.count;
                    return false;
                }
            }
            bool ok = insert(page, idx, key, val_bytes);
            assert_true(ok, "leaf_upsert: insert into leaf failed — leaf was unexpectedly full");
            node->key_count = page.count;
            return true;
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    void leaf_remove(Node* node, U32 ns, KP kp, VP vp, CountType idx) noexcept {
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            using KT = typename KP::key_type;
            constexpr U16 ks = KP::key_stride;
            U16 vs = VP::value_stride;
            os::memory_shift_down(view_shift_up(keys<KT>(node), static_cast<CountType>(idx+1)));
            os::memory_shift_down(view_shift_up(values(node, ns, ks, vs), static_cast<CountType>(idx+1)));
            node->key_count--;
        } else {
            auto page = leaf_page(node, ns, kp, vp);
            remove(page, idx);
            node->key_count = page.count;
        }
    }

    template<KeyPolicy KP, ValuePolicy VP>
    void leaf_copy_suffix(const Node* src, Node* dst, U32 ns, KP kp, VP vp,
                          CountType from) noexcept {
        auto sp = leaf_page(src, ns, kp, vp);
        auto dp = leaf_page(dst, ns, kp, vp);
        copy_suffix_to(sp, from, dp);
        dst->key_count = dp.count;
    }

    template<KeyPolicy KP, ValuePolicy VP>
    bool is_underfull_leaf(const Node* node, U32 ns, KP kp, VP vp) noexcept {
        auto page = leaf_page(node, ns, kp, vp);
        return static_cast<U32>(used_bytes(page)) * 2 < capacity_bytes(page);
    }

    template<KeyPolicy KP>
    bool is_underfull_internal(const Node* node, U32 ns, KP kp) noexcept {
        auto page = internal_page(node, ns, kp);
        return static_cast<U32>(used_bytes(page)) * 2 < capacity_bytes(page);
    }

    // ========================================================================
    // fullness checks
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    bool is_leaf_full(const Node* node, U32 node_size, KP kp, VP vp,
                      U16 next_key_bytes, U16 next_val_bytes) noexcept {
        auto page = leaf_page(node, node_size, kp, vp);
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            return page.count >= page.capacity;
        } else {
            // leaf_page for varlen KP always returns SlottedLeafPage<true,true>
            U16 needed = static_cast<U16>(sizeof(SlotEntry<true, true>)
                                          + next_key_bytes + next_val_bytes);
            return free_bytes(page) < needed;
        }
    }

    template<KeyPolicy KP>
    bool is_internal_full(const Node* node, U32 node_size, KP kp, U16 next_key_bytes) noexcept {
        auto page = internal_page(node, node_size, kp);
        if constexpr (KP::is_fixed_size) {
            return page.count >= page.capacity;
        } else {
            // @perf O(n): sums key lengths to compute compacted free space; insert_key
            // compacts on demand so fragmented free_space() underestimates actual space.
            const auto* slots = page.slots();
            U16 total_key_bytes = 0;
            for (CountType i = 0; i < page.count; i++) {
                total_key_bytes += slots[i].key_len;
            }
            U16 compacted_free = static_cast<U16>(page.usable_capacity() - total_key_bytes - page.slot_end);
            U16 needed = static_cast<U16>(sizeof(InternalSlotEntry) + next_key_bytes);
            return compacted_free < needed;
        }
    }

    // ========================================================================
    // split index
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    CountType split_index_leaf(const Node* node, U32 node_size, KP kp, VP vp) noexcept {
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            return static_cast<CountType>((node->key_count + 1) / 2);
        } else {
            // @perf O(n): scans all entries to find byte-balanced split point
            auto page = leaf_page(node, node_size, kp, vp);
            U16 half = static_cast<U16>(used_bytes(page) / 2);
            U16 acc = 0;
            for (CountType i = 0; i < page.count; i++) {
                acc += static_cast<U16>(key_at(page, i).length + value_at(page, i).length
                                        + sizeof(LeafSlotEntry));
                if (acc >= half)
                    return static_cast<CountType>(i + 1);
            }
            return static_cast<CountType>(page.count / 2);
        }
    }

    template<KeyPolicy KP>
    CountType split_index_internal(const Node* node, U32 node_size, KP kp) noexcept {
        if constexpr (KP::is_fixed_size) {
            return static_cast<CountType>((node->key_count + 1) / 2);
        } else {
            // @perf O(n): two passes over slots to compute actual used bytes and split point
            auto page = internal_page(node, node_size, kp);
            const auto* slots = page.slots();
            U16 total_key_bytes = 0;
            for (CountType i = 0; i < page.count; i++) {
                total_key_bytes += slots[i].key_len;
            }
            U16 actual_used = static_cast<U16>(page.slot_end + total_key_bytes);
            U16 half = static_cast<U16>(actual_used / 2);
            U16 acc = 0;
            CountType ret = 0;
            for (CountType i = 0; i < page.count; i++) {
                acc += static_cast<U16>(slots[i].key_len + sizeof(InternalSlotEntry));
                if (acc >= half) {
                    ret = static_cast<CountType>(i + 1);
                    break;
                }
            }
            if (ret == 0)
                ret = static_cast<CountType>(page.count / 2);
            // Safety: never leave right half empty
            if (ret >= page.count)
                ret = static_cast<CountType>(page.count - 1);
            return ret;
        }
    }
}
