export module plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.btree.types;
import plexdb.pager;

export namespace plexdb::btree {
    struct BTreePaged {
        Pager* pager;
        U64 header_page;

        explicit BTreePaged(Pager* pager, U64 header_page);

        // @todo proper transactions because this just relies on no collision occurring
        // while transaction is holding a page (which may overwrite page buffer)
        struct Transaction {
            explicit Transaction(BTreePaged* btree);
            ~Transaction();
            Transaction(const Transaction& other) = delete;
            Transaction& operator=(const Transaction& other) = delete;

            BTreePaged* t;
        };
    };

    BTreePaged::Transaction scope(const BTreePaged::Transaction& t);

    namespace paged {
        U64 create(Pager* pager, U64 value_stride);
    }
}