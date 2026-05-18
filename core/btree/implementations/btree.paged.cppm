module;
#include <coroutine>

export module plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.pager;
import plexdb.pager.transaction;

import plexdb.btree.types;
import plexdb.btree.policy;

export namespace plexdb::btree {
    template<KeyPolicy KP = U64KeyPolicy, ValuePolicy VP = FixedValuePolicy>
    struct BTreePaged {
        Pager* pager = nullptr;
        U64    header_page = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        BTreePaged() = default;
        BTreePaged(Pager* pager, U64 header_page, KP kp = {}, VP vp = {});
    };

    // Create a new paged tree; returns the header page id
    template<KeyPolicy KP = U64KeyPolicy, ValuePolicy VP = FixedValuePolicy>
    coroutine::Task<U64> create_paged(Pager& pager, KP kp = {}, VP vp = {});

    // Backward-compat overload: create_paged(pager, value_stride)
    coroutine::Task<U64> create_paged(Pager& pager, U64 value_stride);
}
