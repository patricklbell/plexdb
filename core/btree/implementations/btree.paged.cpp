module;
#include <coroutine>

module plexdb.btree.paged;

import plexdb.coroutine;
import plexdb.pager;
import plexdb.btree.node;

namespace plexdb::btree {
    BTreePaged::BTreePaged()
        : pager(nullptr), header_page(0) {}
    BTreePaged::BTreePaged(Pager* pager, U64 header_page)
        : pager(pager), header_page(header_page) {}

    coroutine::Task<U64> create_paged(Pager& pager, U64 value_stride) {
        Header header = Header{
            .value_stride = value_stride,
            .depth = 0,
            .size = 0,
            .root = {},
            .leaves = {},
            .max_keys_per_internal = get_max_internal_nodes_in_bytes(pager.header.page_size),
            .max_keys_per_leaf = get_max_leaf_nodes_in_bytes(pager.header.page_size, value_stride),
        };
        U64 header_page = co_await pager::new_page(pager);
        header.root = reinterpret_cast<NodeRef>(co_await pager::new_page(pager));
        header.leaves = header.root;
        U8* page_data = co_await pager::rwpage(pager, header_page);
        os::memory_copy(page_data, &header);

        co_return header_page;
    }

    BTreePaged::Transaction::Transaction(): t(nullptr) {}
    BTreePaged::Transaction::Transaction(BTreePaged* t): t(t) {
        assert_true(t->pager != nullptr, "cannot create a transaction for an uninitialized btree");
    }
    BTreePaged::Transaction::Transaction(Transaction&& other): started_transaction(other.started_transaction), t(other.t) {
        other.started_transaction = false;
        other.t = nullptr;
    }
    BTreePaged::Transaction& BTreePaged::Transaction::operator=(Transaction&& other) {
        started_transaction = other.started_transaction;
        t = other.t;
        other.started_transaction = false;
        other.t = nullptr;
        return *this;
    }
    BTreePaged::Transaction::~Transaction() {
        if (started_transaction && t && t->pager && t->pager->transaction_active)
            pager::rollback_transaction(*t->pager);
    }

    coroutine::Task<> BTreePaged::Transaction::begin() {
        pager::begin_transaction(*t->pager);
        started_transaction = true;
        co_return;
    }

    coroutine::Task<> BTreePaged::Transaction::commit() {
        co_await pager::commit_transaction(*t->pager);
    }
}
