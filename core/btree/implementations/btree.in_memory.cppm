module;
#include <coroutine>

export module plexdb.btree.in_memory;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.pager.transaction;

import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.btree.node;

export namespace plexdb::btree {
    template<KeyPolicy KP = U64KeyPolicy, ValuePolicy VP = FixedValuePolicy>
    struct BTreeInMemory {
        Header header{};
        U32    node_bytes = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        // Primary constructor
        BTreeInMemory(KP kp, VP vp, U32 node_bytes);

        // Backward-compat constructor — only available for the default policy pair
        BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride)
            requires (SameAs<KP, U64KeyPolicy> && SameAs<VP, FixedValuePolicy>);

        ~BTreeInMemory();
    };
}
