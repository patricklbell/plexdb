export module plexdb.pager.transaction;

import plexdb.base;
import plexdb.coroutine;
import plexdb.pager;

export namespace plexdb::pager {
    // @note currently only one transaction can exist at a time for a pager
    // @todo assert
    struct Transaction {
        bool started_transaction = false;
        Pager* p = nullptr;

        Transaction();
        explicit Transaction(Pager* in_pager);
        Transaction(Transaction&& t);
        Transaction& operator=(Transaction&& other);
        ~Transaction();

        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        coroutine::Task<> begin();
        coroutine::Task<> commit();
    };
}
