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

    template<KeyPolicy KP, ValuePolicy VP>
    BTreePaged<KP,VP>::Transaction::Transaction(): t(nullptr) {}
    template<KeyPolicy KP, ValuePolicy VP>
    BTreePaged<KP,VP>::Transaction::Transaction(BTreePaged* t_): t(t_) {
        assert_true(t_->pager != nullptr, "cannot create a transaction for an uninitialized btree");
    }
    template<KeyPolicy KP, ValuePolicy VP>
    BTreePaged<KP,VP>::Transaction::Transaction(Transaction&& other)
        : started_transaction(other.started_transaction), t(other.t) {
        other.started_transaction = false;
        other.t = nullptr;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    typename BTreePaged<KP,VP>::Transaction&
    BTreePaged<KP,VP>::Transaction::operator=(Transaction&& other) {
        started_transaction = other.started_transaction;
        t = other.t;
        other.started_transaction = false;
        other.t = nullptr;
        return *this;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    BTreePaged<KP,VP>::Transaction::~Transaction() {
        if (started_transaction && t && t->pager && t->pager->transaction_active)
            pager::rollback_transaction(*t->pager);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> BTreePaged<KP,VP>::Transaction::begin() {
        pager::begin_transaction(*t->pager);
        started_transaction = true;
        co_return;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> BTreePaged<KP,VP>::Transaction::commit() {
        co_await pager::commit_transaction(*t->pager);
    }

    // Explicit instantiations
    template struct BTreePaged<U64KeyPolicy, FixedValuePolicy>;
    template coroutine::Task<U64> create_paged(Pager&, U64KeyPolicy, FixedValuePolicy);
}
