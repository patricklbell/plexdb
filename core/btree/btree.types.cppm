export module plexdb.btree.types;

import plexdb.base;
import plexdb.coroutine;
import plexdb.btree.policy;

export namespace plexdb::btree {
    using CountType = U16;
    using NodeRef   = U64;

    // @padding
    struct Node {
        CountType key_count;
        NodeRef   prev, next;
    };

    struct Header {
        U64     depth;
        U64     size;
        NodeRef root;
        NodeRef leaves;
        U16     value_stride; // persisted so paged trees survive reopen without re-supplying the policy
    };

    enum class SearchStrategy {
        RequireEquality,
        FirstGreater,
        FirstGreaterEqual,
        LastLess,
        LastLessEqual,
    };

    // @note templated on policies so operator* can reconstruct the right accessor
    template<KeyPolicy KP, ValuePolicy VP>
    struct BasicIteratorImpl {
        const Node* leaf      = nullptr;
        NodeRef     ref       = 0;
        CountType   idx       = 0;
        U32         node_size = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        bool operator==(const BasicIteratorImpl& o) const noexcept {
            return ref == o.ref && idx == o.idx;
        }
        bool operator!=(const BasicIteratorImpl& o) const noexcept { return !(*this == o); }
    };

    // Backward-compat alias used in existing Iterator<BT,T> for U64+fixed-stride
    using IteratorImpl = BasicIteratorImpl<U64KeyPolicy, FixedValuePolicy>;
}
