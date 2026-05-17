export module plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.btree.types;
import plexdb.pager;

export namespace plexdb::btree {
    struct BTreePaged {
        Pager* pager;
        U64 header_page;

        explicit BTreePaged();
        explicit BTreePaged(Pager* pager, U64 header_page);

        struct Transaction {
            Transaction();
            Transaction(BTreePaged* btree);
            Transaction(Transaction&& t);
            Transaction& operator=(Transaction&& other);
            ~Transaction();

            Transaction(const Transaction& other) = delete;
            Transaction& operator=(const Transaction& other) = delete;

            coroutine::Task<> begin();
            coroutine::Task<> commit();

            bool started_transaction = false;
            BTreePaged* t;
        };
    };
    coroutine::Task<U64> create_paged(Pager& pager, U64 value_stride);
}