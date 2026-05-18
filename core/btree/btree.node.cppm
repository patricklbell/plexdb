export module plexdb.btree.node;

import plexdb.base;
import plexdb.arena;
import plexdb.os;
import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.btree.slots;

export namespace plexdb::btree {
    using KeyType = U64;  // internal alias for the fixed-key layout helpers

    // ========================================================================
    // raw pointer accessors (fixed U64 key layout)
    // ========================================================================
    KeyType*       keys_raw(Node* node);
    NodeRef*       children_raw(Node* node, U32 node_size);
    U8*            values_raw(Node* node, U32 node_size, U16 val_stride);

    const KeyType* keys_raw(const Node* node);
    const NodeRef* children_raw(const Node* node, U32 node_size);
    const U8*      values_raw(const Node* node, U32 node_size, U16 val_stride);

    // ========================================================================
    // view accessors (fixed U64 key layout)
    // ========================================================================
    TArrayView<KeyType, CountType>       keys(Node* node);
    TArrayView<NodeRef, CountType>       children(Node* node, U32 node_size);
    ArrayView<CountType, U16, U8>        values(Node* node, U32 node_size, U16 val_stride);

    TArrayView<const KeyType, CountType> keys(const Node* node);
    TArrayView<const NodeRef, CountType> children(const Node* node, U32 node_size);
    ArrayView<CountType, U16, const U8>  values(const Node* node, U32 node_size, U16 val_stride);

    // ========================================================================
    // capacity helpers
    // ========================================================================
    CountType max_keys_internal(U32 node_size) noexcept;
    CountType max_keys_leaf(U32 node_size, U16 val_stride) noexcept;

    // ========================================================================
    // allocation
    // ========================================================================
    Node* alloc_internal(U32 node_size);
    Node* alloc_leaf(U32 node_size);
    void  init_node(Node* node);

    // ========================================================================
    // policy-parameterized slot accessors
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
        requires (KP::is_fixed_size && VP::is_fixed_size)
    FixedLeafSlots leaf_page(Node* node, U32 node_size, KP, VP vp) noexcept {
        CountType cap = max_keys_leaf(node_size, vp.stride);
        return FixedLeafSlots{
            .base      = reinterpret_cast<U8*>(node) + sizeof(Node),
            .count     = node->key_count,
            .capacity  = cap,
            .key_stride = KP::key_stride,
            .val_stride = vp.stride,
        };
    }

    template<KeyPolicy KP, ValuePolicy VP>
        requires (KP::is_fixed_size && VP::is_fixed_size)
    FixedLeafSlots leaf_page(const Node* node, U32 node_size, KP, VP vp) noexcept {
        CountType cap = max_keys_leaf(node_size, vp.stride);
        return FixedLeafSlots{
            .base      = reinterpret_cast<U8*>(const_cast<Node*>(node)) + sizeof(Node),
            .count     = node->key_count,
            .capacity  = cap,
            .key_stride = KP::key_stride,
            .val_stride = vp.stride,
        };
    }

    template<KeyPolicy KP>
        requires KP::is_fixed_size
    FixedInternalSlots internal_page(Node* node, U32 node_size, KP) noexcept {
        CountType cap = max_keys_internal(node_size);
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
    SlottedLeafPage<true, !VP::is_fixed_size> leaf_page(Node* node, U32 node_size, KP, VP vp) noexcept {
        constexpr bool VV = !VP::is_fixed_size;
        using Page = SlottedLeafPage<true, VV>;
        U8* base = reinterpret_cast<U8*>(node) + sizeof(Node);
        U16 page_cap = static_cast<U16>(node_size - sizeof(Node));
        CountType n = node->key_count;
        U16 slot_end = static_cast<U16>(n * sizeof(typename Page::Entry));
        // Reconstruct data_low: not stored in Node, must be computed from slots
        // Actually node->key_count slots exist; scan slots to find minimum offset
        Page p{};
        p.base      = base;
        p.count     = n;
        p.page_size = page_cap;
        p.key_stride = 0;
        p.val_stride = VP::is_fixed_size ? vp.stride : 0_u16;
        p.slot_end  = slot_end;
        // data_low: find lowest used offset from slot directory
        if (n == 0) {
            p.data_low = page_cap;
        } else {
            p.data_low = page_cap;
            const auto* slots = p.slots();
            for (CountType i = 0; i < n; i++) {
                const auto& e = slots[i];
                if constexpr (true)  if (e.key_off < p.data_low) p.data_low = e.key_off;
                if constexpr (VV)    if (e.val_off < p.data_low) p.data_low = e.val_off;
            }
        }
        return p;
    }

    template<KeyPolicy KP, ValuePolicy VP>
        requires (!KP::is_fixed_size)
    SlottedLeafPage<true, !VP::is_fixed_size> leaf_page(const Node* node, U32 node_size, KP kp, VP vp) noexcept {
        return leaf_page(const_cast<Node*>(node), node_size, kp, vp);
    }

    // Varlen internal page
    template<KeyPolicy KP>
        requires (!KP::is_fixed_size)
    SlottedInternalPage<true> internal_page(Node* node, U32 node_size, KP) noexcept {
        using Page = SlottedInternalPage<true>;
        U8* base = reinterpret_cast<U8*>(node) + sizeof(Node);
        U16 page_cap = static_cast<U16>(node_size - sizeof(Node));
        // max_children: conservative bound — each slot takes at least sizeof(SlotEntry<true,false>) + 1 byte key + 1 child ref
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
    // fullness checks
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    bool is_leaf_full(const Node* node, U32 node_size, KP kp, VP vp,
                      U16 next_key_bytes, U16 next_val_bytes) noexcept {
        auto page = leaf_page(node, node_size, kp, vp);
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            return page.count >= page.capacity;
        } else {
            U16 needed = static_cast<U16>(sizeof(SlotEntry<!KP::is_fixed_size, !VP::is_fixed_size>)
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
            U16 needed = static_cast<U16>(sizeof(SlotEntry<true,false>) + next_key_bytes + sizeof(NodeRef));
            return free_bytes(page) < needed;
        }
    }

    // ========================================================================
    // split index: position that splits the node ~evenly by bytes
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    CountType split_index_leaf(const Node* node, U32 node_size, KP kp, VP vp) noexcept {
        if constexpr (KP::is_fixed_size && VP::is_fixed_size) {
            return static_cast<CountType>((node->key_count + 1) / 2);
        } else {
            auto page = leaf_page(node, node_size, kp, vp);
            U16 half = static_cast<U16>(used_bytes(page) / 2);
            U16 acc = 0;
            for (CountType i = 0; i < page.count; i++) {
                acc += static_cast<U16>(key_at(page, i).length + value_at(page, i).length
                                        + sizeof(SlotEntry<!KP::is_fixed_size, !VP::is_fixed_size>));
                if (acc >= half) return static_cast<CountType>(i + 1);
            }
            return static_cast<CountType>(page.count / 2);
        }
    }

    template<KeyPolicy KP>
    CountType split_index_internal(const Node* node, U32 node_size, KP kp) noexcept {
        if constexpr (KP::is_fixed_size) {
            return static_cast<CountType>((node->key_count + 1) / 2);
        } else {
            auto page = internal_page(node, node_size, kp);
            U16 half = static_cast<U16>(used_bytes(page) / 2);
            U16 acc = 0;
            for (CountType i = 0; i < page.count; i++) {
                acc += static_cast<U16>(key_at(page, i).length
                                        + sizeof(SlotEntry<true,false>) + sizeof(NodeRef));
                if (acc >= half) return static_cast<CountType>(i + 1);
            }
            return static_cast<CountType>(page.count / 2);
        }
    }
}
