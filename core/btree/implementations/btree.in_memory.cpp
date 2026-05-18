module;
#include <coroutine>

module plexdb.btree.in_memory;

import plexdb.os;
import plexdb.coroutine;
import plexdb.btree.node;
import plexdb.btree.in_memory.detail;

namespace plexdb::btree {
    // ========================================================================
    // helpers shared by both constructors
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    static void init_tree(BTreeInMemory<KP,VP>& self) {
        Node* root = alloc_leaf(self.node_bytes);
        self.header = Header{
            .depth        = 0,
            .size         = 0,
            .root         = reinterpret_cast<NodeRef>(root),
            .leaves       = reinterpret_cast<NodeRef>(root),
            .value_stride = self.vp.stride,
        };
    }

    template<KeyPolicy KP, ValuePolicy VP>
    static void deallocate_tree(BTreeInMemory<KP,VP>& btree, Node* node, CountType depth) {
        if (depth == btree.header.depth) {
            os::deallocate(node);
        } else {
            auto cs = children(node, btree.node_bytes);
            for (const auto& child_ref : cs)
                deallocate_tree(btree, reinterpret_cast<Node*>(child_ref), static_cast<CountType>(depth + 1));
            os::deallocate(node);
        }
    }

    // ========================================================================
    // BTreeInMemory<KP, VP> — primary constructor
    // ========================================================================
    template<KeyPolicy KP, ValuePolicy VP>
    BTreeInMemory<KP,VP>::BTreeInMemory(KP kp_, VP vp_, U32 nb)
        : node_bytes(nb), kp(kp_), vp(vp_)
    {
        assert_true(node_bytes >= 64, "node_bytes must be large enough for at least a few keys");
        init_tree(*this);
    }

    // Backward-compat constructor
    template<KeyPolicy KP, ValuePolicy VP>
    BTreeInMemory<KP,VP>::BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride)
        requires (SameAs<KP, U64KeyPolicy> && SameAs<VP, FixedStrideValuePolicy>)
    {
        assert_true(max_keys_per_internal >= 3, "positive min keys.");
        assert_true(max_keys_per_leaf >= 3, "positive min keys.");
        // Derive node_bytes from max_keys — pick the larger of the two layouts
        U32 internal_bytes = static_cast<U32>(sizeof(Node)
            + max_keys_per_internal * sizeof(U64)
            + (max_keys_per_internal + 1) * sizeof(NodeRef));
        U32 leaf_bytes = static_cast<U32>(sizeof(Node)
            + max_keys_per_leaf * (sizeof(U64) + value_stride));
        node_bytes = internal_bytes > leaf_bytes ? internal_bytes : leaf_bytes;
        kp = U64KeyPolicy{};
        vp = FixedStrideValuePolicy{.stride = static_cast<U16>(value_stride)};
        init_tree(*this);
    }

    template<KeyPolicy KP, ValuePolicy VP>
    BTreeInMemory<KP,VP>::~BTreeInMemory() {
        deallocate_tree(*this, reinterpret_cast<Node*>(this->header.root), 0);
    }

    template<KeyPolicy KP, ValuePolicy VP>
    BTreeInMemory<KP,VP>::Transaction::Transaction(): t(nullptr) {}
    template<KeyPolicy KP, ValuePolicy VP>
    BTreeInMemory<KP,VP>::Transaction::Transaction(BTreeInMemory* t_): t(t_) {}
    template<KeyPolicy KP, ValuePolicy VP>
    BTreeInMemory<KP,VP>::Transaction::Transaction(Transaction&& other): t(other.t) {}

    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> BTreeInMemory<KP,VP>::Transaction::begin()  { co_return; }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> BTreeInMemory<KP,VP>::Transaction::commit() { co_return; }

    // Explicit instantiations
    template struct BTreeInMemory<U64KeyPolicy, FixedStrideValuePolicy>;
}
