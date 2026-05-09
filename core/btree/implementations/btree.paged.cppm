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

        // @todo proper transactions because this just relies on no collision occurring
        // while transaction is holding a page (which may overwrite page buffer)
        struct Transaction {
            Transaction();
            Transaction(BTreePaged* btree);
            Transaction(Transaction&& t);
            Transaction& operator=(Transaction&& other);
            ~Transaction();

            Transaction(const Transaction& other) = delete;
            Transaction& operator=(const Transaction& other) = delete;

            BTreePaged* t;
        };
    };
    coroutine::Task<U64> create_paged(Pager& pager, U64 value_stride);

    BTreePaged::Transaction scope(const BTreePaged::Transaction& t);
}