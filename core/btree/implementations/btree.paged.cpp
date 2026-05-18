module;
#include <coroutine>

module plexdb.btree.paged;

import plexdb.coroutine;
import plexdb.pager;
import plexdb.btree.node;
import plexdb.os;

namespace plexdb::btree {
    template<KeyPolicy KP, ValuePolicy VP>
    BTreePaged<KP,VP>::BTreePaged(Pager* p, U64 hp, KP kp_, VP vp_)
        : pager(p), header_page(hp), kp(kp_), vp(vp_) {}

    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<U64> create_paged(Pager& pager, KP, VP vp_) {
        Header header{.depth = 0, .size = 0, .root = {}, .leaves = {},
                      .value_stride = vp_.stride};
        U64 header_page = co_await pager::new_page(pager);
        header.root = co_await pager::new_page(pager);
        header.leaves = header.root;
        U8* page_data = co_await pager::rwpage(pager, header_page);
        os::memory_copy(page_data, &header, sizeof(Header));
        co_return header_page;
    }

    // Backward-compat: create_paged(pager, value_stride)
    coroutine::Task<U64> create_paged(Pager& pager, U64 value_stride) {
        co_return co_await create_paged(pager,
            U64KeyPolicy{},
            FixedValuePolicy{.stride = static_cast<U16>(value_stride)});
    }

    // Explicit instantiations
    template struct BTreePaged<U64KeyPolicy, FixedValuePolicy>;
    template coroutine::Task<U64> create_paged(Pager&, U64KeyPolicy, FixedValuePolicy);
}
