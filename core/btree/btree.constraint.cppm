export module plexdb.btree.constraint;

import plexdb.base;
import plexdb.coroutine;

import plexdb.btree.policy;
import plexdb.btree.in_memory.detail;
import plexdb.btree.paged.detail;

export namespace plexdb::btree {
    template<typename B>
    concept BTree = requires(const B& b) {
        { key_policy(b)   } -> KeyPolicy;
        { value_policy(b) } -> ValuePolicy;
        { node_size(b)    } -> SameAs<U32>;
    };

    template<BTree BT>
    using BTreeKeyType = typename decltype(key_policy(declval<const BT&>()))::key_type;

    template<BTree BT>
    using BTreeKP = typename BT::kp_type;

    template<BTree BT>
    using BTreeVP = typename BT::vp_type;
}
