export module plexdb.btree.slots;

import plexdb.base;
import plexdb.os;

import plexdb.btree.policy;
import plexdb.btree.types;

export namespace plexdb::btree {
    // ========================================================================
    // SlotEntry — conditional fields based on varlen-ness
    // ========================================================================
    template<bool VarlenKey, bool VarlenValue>
    struct SlotEntry;

    template<> struct SlotEntry<false, false> {};
    template<> struct SlotEntry<false, true>  { U16 val_off; U16 val_len; };
    template<> struct SlotEntry<true,  false> { U16 key_off; U16 key_len; };
    template<> struct SlotEntry<true,  true>  { U16 key_off; U16 key_len; U16 val_off; U16 val_len; };

    // ========================================================================
    // FixedLeafSlots — both key and value are fixed-stride
    // ========================================================================
    struct FixedLeafSlots {
        U8*       base;
        CountType count;
        CountType capacity;
        U16       key_stride;
        U16       val_stride;

        U8* keys_base() const noexcept { return base; }
        U8* vals_base() const noexcept { return base + capacity * key_stride; }
    };

    TArrayView<const U8, U16> key_at(const FixedLeafSlots& s, CountType i) noexcept {
        return {s.keys_base() + i * s.key_stride, s.key_stride};
    }
    TArrayView<U8, U16> value_at(FixedLeafSlots& s, CountType i) noexcept {
        return {s.vals_base() + i * s.val_stride, s.val_stride};
    }
    TArrayView<const U8, U16> value_at(const FixedLeafSlots& s, CountType i) noexcept {
        return {s.vals_base() + i * s.val_stride, s.val_stride};
    }
    bool insert(FixedLeafSlots& s, CountType i,
                TArrayView<const U8, U16> k, TArrayView<const U8, U16> v) noexcept {
        if (s.count >= s.capacity) return false;
        U8* kp = s.keys_base() + i * s.key_stride;
        U8* vp = s.vals_base() + i * s.val_stride;
        os::memory_move(kp + s.key_stride, kp, (s.count - i) * s.key_stride);
        os::memory_move(vp + s.val_stride, vp, (s.count - i) * s.val_stride);
        os::memory_copy(kp, k.ptr, s.key_stride);
        os::memory_copy(vp, v.ptr, s.val_stride);
        s.count++;
        return true;
    }
    void remove(FixedLeafSlots& s, CountType i) noexcept {
        U8* kp = s.keys_base() + (i + 1) * s.key_stride;
        U8* vp = s.vals_base() + (i + 1) * s.val_stride;
        os::memory_move(kp - s.key_stride, kp, (s.count - i - 1) * s.key_stride);
        os::memory_move(vp - s.val_stride, vp, (s.count - i - 1) * s.val_stride);
        s.count--;
    }
    U16 free_bytes(const FixedLeafSlots& s) noexcept {
        return static_cast<U16>((s.capacity - s.count) * (s.key_stride + s.val_stride));
    }
    U16 used_bytes(const FixedLeafSlots& s) noexcept {
        return static_cast<U16>(s.count * (s.key_stride + s.val_stride));
    }
    U16 capacity_bytes(const FixedLeafSlots& s) noexcept {
        return static_cast<U16>(s.capacity * (s.key_stride + s.val_stride));
    }
    void copy_suffix_to(const FixedLeafSlots& src, CountType from, FixedLeafSlots& dst) noexcept {
        CountType n = static_cast<CountType>(src.count - from);
        os::memory_copy(dst.keys_base(), src.keys_base() + from * src.key_stride, n * src.key_stride);
        os::memory_copy(dst.vals_base(), src.vals_base() + from * src.val_stride, n * src.val_stride);
        dst.count = n;
    }

    // ========================================================================
    // FixedInternalSlots — fixed-stride keys + NodeRef children
    // ========================================================================
    struct FixedInternalSlots {
        U8*       base;
        CountType count;
        CountType capacity;
        U16       key_stride;

        U8*      keys_base()     const noexcept { return base; }
        NodeRef* children_base() const noexcept {
            return reinterpret_cast<NodeRef*>(base + capacity * key_stride);
        }
    };

    TArrayView<const U8, U16> key_at(const FixedInternalSlots& s, CountType i) noexcept {
        return {s.keys_base() + i * s.key_stride, s.key_stride};
    }
    NodeRef& child_at(FixedInternalSlots& s, CountType i) noexcept {
        return s.children_base()[i];
    }
    NodeRef child_at(const FixedInternalSlots& s, CountType i) noexcept {
        return s.children_base()[i];
    }
    // overwrite key bytes at index i without touching children
    void replace_key(FixedInternalSlots& s, CountType i, TArrayView<const U8, U16> k) noexcept {
        os::memory_copy(s.keys_base() + i * s.key_stride, k.ptr, s.key_stride);
    }
    // insert key at index i (shifts keys i..count-1 right; children shifted separately)
    bool insert_key(FixedInternalSlots& s, CountType i, TArrayView<const U8, U16> k) noexcept {
        if (s.count >= s.capacity) return false;
        U8* kp = s.keys_base() + i * s.key_stride;
        os::memory_move(kp + s.key_stride, kp, (s.count - i) * s.key_stride);
        os::memory_copy(kp, k.ptr, s.key_stride);
        return true;
    }
    // insert child ref at child index i (i.e., after key i-1), shifts children i..count right
    void insert_child(FixedInternalSlots& s, CountType i, NodeRef ref) noexcept {
        NodeRef* cp = s.children_base() + i;
        os::memory_move(cp + 1, cp, (s.count - i + 1) * sizeof(NodeRef));
        *cp = ref;
    }
    // remove key at index i and child at index i+1
    void remove(FixedInternalSlots& s, CountType i) noexcept {
        U8* kp = s.keys_base() + (i + 1) * s.key_stride;
        os::memory_move(kp - s.key_stride, kp, (s.count - i - 1) * s.key_stride);
        NodeRef* cp = s.children_base() + i + 2;
        os::memory_move(cp - 1, cp, (s.count - i - 1) * sizeof(NodeRef));
        s.count--;
    }
    U16 free_bytes(const FixedInternalSlots& s) noexcept {
        return static_cast<U16>((s.capacity - s.count) * (s.key_stride + sizeof(NodeRef)));
    }
    U16 used_bytes(const FixedInternalSlots& s) noexcept {
        return static_cast<U16>(s.count * (s.key_stride + sizeof(NodeRef)));
    }
    U16 capacity_bytes(const FixedInternalSlots& s) noexcept {
        return static_cast<U16>(s.capacity * (s.key_stride + sizeof(NodeRef)));
    }
    void copy_suffix_to(const FixedInternalSlots& src, CountType from, FixedInternalSlots& dst) noexcept {
        CountType n = static_cast<CountType>(src.count - from);
        os::memory_copy(dst.keys_base(), src.keys_base() + from * src.key_stride, n * src.key_stride);
        // copy children from index from (inclusive) through count (inclusive) = n+1 children
        os::memory_copy(dst.children_base(), src.children_base() + from, (n + 1) * sizeof(NodeRef));
        dst.count = n;
    }

    // ========================================================================
    // SlottedLeafPage<VK, VV>
    // ========================================================================
    template<bool VK, bool VV>
    struct SlottedLeafPage {
        static_assert(VK, "SlottedLeafPage requires varlen keys; use FixedLeafSlots for fixed keys");
        U8*       base;
        CountType count;
        U16       slot_end;   // first free byte after slot directory
        U16       data_low;   // lowest used byte in data region (grows backward from page_end)
        U16       page_size;  // total page size (base[0..page_size))
        U16       key_stride; // only used when VK==false
        U16       val_stride; // only used when VV==false

        using Entry = SlotEntry<VK, VV>;
        Entry* slots() const noexcept { return reinterpret_cast<Entry*>(base); }

        U16 header_bytes() const noexcept { return slot_end; }
        U16 free_space() const noexcept {
            return static_cast<U16>(data_low - slot_end);
        }

        // bytes needed to add one more slot entry
        static constexpr U16 slot_entry_size() noexcept { return static_cast<U16>(sizeof(Entry)); }
    };

    template<bool VK, bool VV>
    TArrayView<const U8, U16> key_at(const SlottedLeafPage<VK, VV>& p, CountType i) noexcept {
        const auto& e = p.slots()[i];
        return {p.base + e.key_off, e.key_len};
    }

    template<bool VK, bool VV>
    TArrayView<U8, U16> value_at(SlottedLeafPage<VK, VV>& p, CountType i) noexcept {
        if constexpr (VV) {
            const auto& e = p.slots()[i];
            return {p.base + e.val_off, e.val_len};
        } else {
            // Fixed value: stored at fixed stride from end of page, indexed by slot
            return {p.base + p.page_size - (i + 1) * p.val_stride, p.val_stride};
        }
    }

    template<bool VK, bool VV>
    TArrayView<const U8, U16> value_at(const SlottedLeafPage<VK, VV>& p, CountType i) noexcept {
        if constexpr (VV) {
            const auto& e = p.slots()[i];
            return {p.base + e.val_off, e.val_len};
        } else {
            return {p.base + p.page_size - (i + 1) * p.val_stride, p.val_stride};
        }
    }

    template<bool VK, bool VV>
    U16 free_bytes(const SlottedLeafPage<VK, VV>& p) noexcept {
        return p.free_space();
    }

    template<bool VK, bool VV>
    U16 used_bytes(const SlottedLeafPage<VK, VV>& p) noexcept {
        return static_cast<U16>(p.page_size - p.free_space());
    }

    template<bool VK, bool VV>
    U16 capacity_bytes(const SlottedLeafPage<VK, VV>& p) noexcept {
        return p.page_size;
    }

    // compact: close all holes in data region, update offsets
    template<bool VK, bool VV>
    void compact(SlottedLeafPage<VK, VV>& p) noexcept {
        U16 write_pos = p.page_size;
        for (CountType i = 0; i < p.count; i++) {
            auto& e = p.slots()[i];
            if constexpr (VV) {
                write_pos -= e.val_len;
                if (p.base + write_pos != p.base + e.val_off)
                    os::memory_move(p.base + write_pos, p.base + e.val_off, e.val_len);
                e.val_off = write_pos;
            }
            if constexpr (VK) {
                write_pos -= e.key_len;
                if (p.base + write_pos != p.base + e.key_off)
                    os::memory_move(p.base + write_pos, p.base + e.key_off, e.key_len);
                e.key_off = write_pos;
            }
        }
        p.data_low = write_pos;
    }

    template<bool VK, bool VV>
    bool insert(SlottedLeafPage<VK, VV>& p, CountType i,
                TArrayView<const U8, U16> k, TArrayView<const U8, U16> v) noexcept {
        static_assert(VK, "SlottedLeafPage insert requires varlen key");
        U16 needed = static_cast<U16>(sizeof(SlotEntry<VK,VV>) + k.length + (VV ? v.length : 0));
        if (p.free_space() < needed) {
            compact(p);
            if (p.free_space() < needed) return false;
        }
        // shift slot dir entries [i..count) right
        auto* slots = p.slots();
        os::memory_move(slots + i + 1, slots + i, (p.count - i) * sizeof(SlotEntry<VK,VV>));

        // write data from data_low downward
        auto& e = slots[i];
        if constexpr (VV) {
            p.data_low -= v.length;
            os::memory_copy(p.base + p.data_low, v.ptr, v.length);
            e.val_off = p.data_low;
            e.val_len = v.length;
        }
        if constexpr (VK) {
            p.data_low -= k.length;
            os::memory_copy(p.base + p.data_low, k.ptr, k.length);
            e.key_off = p.data_low;
            e.key_len = k.length;
        }
        p.slot_end += sizeof(SlotEntry<VK,VV>);
        p.count++;
        return true;
    }

    template<bool VK, bool VV>
    void remove(SlottedLeafPage<VK, VV>& p, CountType i) noexcept {
        static_assert(VK, "SlottedLeafPage remove requires varlen key");
        auto* slots = p.slots();
        auto& e = slots[i];

        // reclaim data bytes by shifting data above the removed bytes downward
        if constexpr (VK) {
            U16 off = e.key_off;
            U16 len = e.key_len;
            os::memory_move(p.base + p.data_low + len, p.base + p.data_low, off - p.data_low);
            p.data_low += len;
            // update offsets of other entries whose data was shifted
            for (CountType j = 0; j < p.count; j++) {
                if (j == i) continue;
                if constexpr (VK) if (slots[j].key_off < off) slots[j].key_off += len;
                if constexpr (VV) if (slots[j].val_off < off) slots[j].val_off += len;
            }
        }
        if constexpr (VV) {
            U16 off = e.val_off;
            U16 len = e.val_len;
            os::memory_move(p.base + p.data_low + len, p.base + p.data_low, off - p.data_low);
            p.data_low += len;
            for (CountType j = 0; j < p.count; j++) {
                if (j == i) continue;
                if constexpr (VK) if (slots[j].key_off < off) slots[j].key_off += len;
                if constexpr (VV) if (slots[j].val_off < off) slots[j].val_off += len;
            }
        }

        // shift slot dir entries [i+1..count) left
        os::memory_move(slots + i, slots + i + 1, (p.count - i - 1) * sizeof(SlotEntry<VK,VV>));
        p.slot_end -= sizeof(SlotEntry<VK,VV>);
        p.count--;
    }

    template<bool VK, bool VV>
    void copy_suffix_to(const SlottedLeafPage<VK, VV>& src, CountType from,
                        SlottedLeafPage<VK, VV>& dst) noexcept {
        static_assert(VK, "copy_suffix_to requires varlen key");
        CountType n = static_cast<CountType>(src.count - from);
        const auto* src_slots = src.slots();
        auto* dst_slots = dst.slots();
        // copy slot entries and data for each slot
        for (CountType i = 0; i < n; i++) {
            const auto& se = src_slots[from + i];
            auto& de = dst_slots[i];
            if constexpr (VK) {
                dst.data_low -= se.key_len;
                os::memory_copy(dst.base + dst.data_low, src.base + se.key_off, se.key_len);
                de.key_off = dst.data_low;
                de.key_len = se.key_len;
            }
            if constexpr (VV) {
                dst.data_low -= se.val_len;
                os::memory_copy(dst.base + dst.data_low, src.base + se.val_off, se.val_len);
                de.val_off = dst.data_low;
                de.val_len = se.val_len;
            }
        }
        dst.slot_end += n * sizeof(SlotEntry<VK,VV>);
        dst.count = n;
    }

    // ========================================================================
    // SlottedInternalPage<VK> — varlen keys + fixed NodeRef children at page end
    // ========================================================================
    template<bool VK>
    struct SlottedInternalPage {
        static_assert(VK, "SlottedInternalPage requires varlen keys; use FixedInternalSlots for fixed keys");
        U8*       base;
        CountType count;
        U16       slot_end;
        U16       data_low;
        U16       page_size;
        U16       key_stride;    // only used when VK==false
        CountType max_children;  // determines where children area starts

        using Entry = SlotEntry<VK, false>;
        Entry*   slots()    const noexcept { return reinterpret_cast<Entry*>(base); }
        NodeRef* children() const noexcept {
            return reinterpret_cast<NodeRef*>(base + page_size - max_children * sizeof(NodeRef));
        }
        U16 children_region_size() const noexcept {
            return static_cast<U16>(max_children * sizeof(NodeRef));
        }
        U16 usable_capacity() const noexcept {
            return static_cast<U16>(page_size - children_region_size());
        }
        U16 free_space() const noexcept {
            return static_cast<U16>(data_low - slot_end);
        }
    };

    template<bool VK>
    TArrayView<const U8, U16> key_at(const SlottedInternalPage<VK>& p, CountType i) noexcept {
        const auto& e = p.slots()[i];
        return {p.base + e.key_off, e.key_len};
    }

    template<bool VK>
    NodeRef& child_at(SlottedInternalPage<VK>& p, CountType i) noexcept {
        return p.children()[i];
    }

    template<bool VK>
    NodeRef child_at(const SlottedInternalPage<VK>& p, CountType i) noexcept {
        return p.children()[i];
    }

    template<bool VK>
    U16 free_bytes(const SlottedInternalPage<VK>& p) noexcept {
        return p.free_space();
    }

    template<bool VK>
    U16 used_bytes(const SlottedInternalPage<VK>& p) noexcept {
        return static_cast<U16>(p.usable_capacity() - p.free_space());
    }

    template<bool VK>
    void compact(SlottedInternalPage<VK>& p) noexcept {
        if constexpr (!VK) return;
        U16 write_pos = p.usable_capacity();
        for (CountType i = 0; i < p.count; i++) {
            auto& e = p.slots()[i];
            write_pos -= e.key_len;
            if (p.base + write_pos != p.base + e.key_off)
                os::memory_move(p.base + write_pos, p.base + e.key_off, e.key_len);
            e.key_off = write_pos;
        }
        p.data_low = write_pos;
    }

    // replace key at index i without touching children
    template<bool VK>
    bool replace_key(SlottedInternalPage<VK>& p, CountType i,
                     TArrayView<const U8, U16> k) noexcept {
        static_assert(VK, "replace_key on SlottedInternalPage requires varlen key");
        auto& e = p.slots()[i];
        // reclaim old key bytes
        U16 old_off = e.key_off, old_len = e.key_len;
        os::memory_move(p.base + p.data_low + old_len, p.base + p.data_low, old_off - p.data_low);
        p.data_low += old_len;
        for (CountType j = 0; j < p.count; j++)
            if (p.slots()[j].key_off < old_off) p.slots()[j].key_off += old_len;
        // allocate new key bytes
        if (p.free_space() < k.length) {
            compact(p);
            if (p.free_space() < k.length) return false;
        }
        p.data_low -= k.length;
        os::memory_copy(p.base + p.data_low, k.ptr, k.length);
        e.key_off = p.data_low;
        e.key_len = k.length;
        return true;
    }

    template<bool VK>
    bool insert_key(SlottedInternalPage<VK>& p, CountType i,
                    TArrayView<const U8, U16> k) noexcept {
        static_assert(VK, "insert_key on SlottedInternalPage requires varlen key");
        U16 needed = static_cast<U16>(sizeof(SlotEntry<VK,false>) + k.length);
        if (p.free_space() < needed) {
            compact(p);
            if (p.free_space() < needed) return false;
        }
        auto* slots = p.slots();
        os::memory_move(slots + i + 1, slots + i, (p.count - i) * sizeof(SlotEntry<VK,false>));
        auto& e = slots[i];
        p.data_low -= k.length;
        os::memory_copy(p.base + p.data_low, k.ptr, k.length);
        e.key_off = p.data_low;
        e.key_len = k.length;
        p.slot_end += sizeof(SlotEntry<VK,false>);
        p.count++;
        return true;
    }

    template<bool VK>
    void insert_child(SlottedInternalPage<VK>& p, CountType i, NodeRef ref) noexcept {
        NodeRef* cp = p.children() + i;
        os::memory_move(cp + 1, cp, (p.count - i + 1) * sizeof(NodeRef));
        *cp = ref;
    }

    template<bool VK>
    void remove(SlottedInternalPage<VK>& p, CountType i) noexcept {
        static_assert(VK, "remove on SlottedInternalPage requires varlen key");
        auto* slots = p.slots();
        auto& e = slots[i];
        // reclaim key data
        U16 off = e.key_off;
        U16 len = e.key_len;
        os::memory_move(p.base + p.data_low + len, p.base + p.data_low, off - p.data_low);
        p.data_low += len;
        for (CountType j = 0; j < p.count; j++) {
            if (j == i) continue;
            if (slots[j].key_off < off) slots[j].key_off += len;
        }
        // shift slot dir left
        os::memory_move(slots + i, slots + i + 1, (p.count - i - 1) * sizeof(SlotEntry<VK,false>));
        p.slot_end -= sizeof(SlotEntry<VK,false>);
        // shift children: remove child at index i+1
        NodeRef* cp = p.children() + i + 2;
        os::memory_move(cp - 1, cp, (p.count - i - 1) * sizeof(NodeRef));
        p.count--;
    }

    // remove first key and leftmost child (children[0]) — used in move_from_right for internal
    template<bool VK>
    void remove_front(SlottedInternalPage<VK>& p) noexcept {
        static_assert(VK, "remove_front requires varlen key");
        auto* slots = p.slots();
        auto& e = slots[0];
        U16 off = e.key_off, len = e.key_len;
        os::memory_move(p.base + p.data_low + len, p.base + p.data_low, off - p.data_low);
        p.data_low += len;
        for (CountType j = 1; j < p.count; j++)
            if (slots[j].key_off < off) slots[j].key_off += len;
        os::memory_move(slots, slots + 1, (p.count - 1) * sizeof(SlotEntry<VK,false>));
        p.slot_end -= sizeof(SlotEntry<VK,false>);
        p.count--;
        // shift children left to remove children[0]
        NodeRef* cp = p.children();
        os::memory_move(cp, cp + 1, p.count * sizeof(NodeRef));
    }

    template<bool VK>
    U16 capacity_bytes(const SlottedInternalPage<VK>& p) noexcept {
        return p.usable_capacity();
    }

    template<bool VK>
    void copy_suffix_to(const SlottedInternalPage<VK>& src, CountType from,
                        SlottedInternalPage<VK>& dst) noexcept {
        static_assert(VK, "copy_suffix_to requires varlen key");
        CountType n = static_cast<CountType>(src.count - from);
        const auto* ss = src.slots();
        auto* ds = dst.slots();
        for (CountType i = 0; i < n; i++) {
            const auto& se = ss[from + i];
            auto& de = ds[i];
            dst.data_low -= se.key_len;
            os::memory_copy(dst.base + dst.data_low, src.base + se.key_off, se.key_len);
            de.key_off = dst.data_low;
            de.key_len = se.key_len;
        }
        dst.slot_end += n * sizeof(SlotEntry<VK,false>);
        dst.count = n;
        // copy children from..count (inclusive) = n+1 children
        os::memory_copy(dst.children(), src.children() + from, (n + 1) * sizeof(NodeRef));
    }
}
