module;
#include <coroutine>

export module plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.pager;

import plexdb.btree.types;
import plexdb.btree.policy;

export namespace plexdb::btree {
    template<KeyPolicy KP, ValuePolicy VP>
    struct BTreePaged {
        using kp_type                        = KP;
        using vp_type                        = VP;
        Pager*                   pager       = nullptr;
        U64                      header_page = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        BTreePaged() = default;

        BTreePaged(Pager* p, U64 hp, KP kp, VP vp)
            : pager(p)
            , header_page(hp)
            , kp(kp)
            , vp(vp) {
        }
    };

    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<U64> create_paged(Pager& pager, KP, VP) {
        Header header{
            .depth  = 0,
            .size   = 0,
            .root   = {},
            .leaves = {}
        };

        // @todo avoid extra page for header
        U64 header_page = co_await pager::new_page(pager);
        header.root     = co_await pager::new_page(pager);
        header.leaves   = header.root;
        U8* page_data   = co_await pager::rwpage(pager, header_page);
        os::memory_copy(page_data, &header, sizeof(Header));

        co_return header_page;
    }
}
