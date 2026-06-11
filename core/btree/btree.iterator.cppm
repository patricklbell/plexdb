export module plexdb.btree.iterator;

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;

import plexdb.btree.types;
import plexdb.btree.policy;

export namespace plexdb::btree {
    template<KeyPolicy KP, ValuePolicy VP>
    struct IteratorImpl {
        UniquePtr<U8> leaf_buf{};         // owned copy of current leaf page; null = end
        const Node*   leaf      = nullptr;  // points into leaf_buf.ptr
        NodeRef       ref       = 0;
        CountType     idx       = 0;
        SizeType      node_size = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        IteratorImpl() = default;

        IteratorImpl(IteratorImpl&& o) noexcept
            : leaf_buf(plexdb::move(o.leaf_buf)), leaf(o.leaf), ref(o.ref), idx(o.idx),
              node_size(o.node_size), kp(o.kp), vp(o.vp) {
            o.leaf = nullptr;
        }

        IteratorImpl& operator=(IteratorImpl&& o) noexcept {
            if (this != &o) {
                leaf_buf  = plexdb::move(o.leaf_buf); leaf = o.leaf;
                ref       = o.ref;                    idx  = o.idx;
                node_size = o.node_size; kp = o.kp;  vp   = o.vp;
                o.leaf    = nullptr;
            }
            return *this;
        }

        ~IteratorImpl() = default;

        IteratorImpl(const IteratorImpl&) = delete;
        IteratorImpl& operator=(const IteratorImpl&) = delete;

        bool operator==(const IteratorImpl& o) const noexcept {
            return ref == o.ref && idx == o.idx;
        }
        bool operator!=(const IteratorImpl& o) const noexcept { return !(*this == o); }
    };
}
