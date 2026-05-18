module;
#include <coroutine>

module plexdb.pager.transaction;

namespace plexdb::pager {
    Transaction::Transaction(): p(nullptr) {}
    Transaction::Transaction(Pager* in_pager): p(in_pager) {
        assert_true(this->p != nullptr, "cannot create a Transaction from a nullptr");
    }
    Transaction::Transaction(Transaction&& other)
        : started_transaction(other.started_transaction), p(other.p) {
            other.started_transaction = false;
        other.p = nullptr;
    }
    Transaction& Transaction::operator=(Transaction&& other) {
        this->started_transaction = other.started_transaction;
        this->p = other.p;
        other.started_transaction = false;
        other.p = nullptr;
        return *this;
    }
    Transaction::~Transaction() {
        if (started_transaction && p && p->transaction_active)
            pager::rollback_transaction(*p);
    }
    coroutine::Task<> Transaction::begin() {
        pager::begin_transaction(*p);
        started_transaction = true;
        co_return;
    }
    coroutine::Task<> Transaction::commit() {
        co_await pager::commit_transaction(*p);
    }
}
