export module plexdb.btree.iterator;

import plexdb.base;
import plexdb.coroutine;

import plexdb.btree.types;
import plexdb.btree.policy;

export namespace plexdb::btree {
    // @note templated on policies so operator* can reconstruct the right accessor
    template<KeyPolicy KP, ValuePolicy VP>
    struct IteratorImpl {
        const Node* leaf      = nullptr;
        NodeRef     ref       = 0;
        CountType   idx       = 0;
        SizeType    node_size = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        bool operator==(const IteratorImpl& o) const noexcept {
            return ref == o.ref && idx == o.idx;
        }
        bool operator!=(const IteratorImpl& o) const noexcept { return !(*this == o); }
    };

}
