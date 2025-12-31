module plexdb.btree.paged;

import plexdb.pager;

namespace plexdb::btree {
    BTreePaged::BTreePaged(Pager* pager, U64 header_page)
        : pager(pager), header_page(header_page) {}

    // @todo
    BTreePaged::Transaction::Transaction(BTreePaged* t): t(t) {}
    BTreePaged::Transaction::~Transaction() {}
    BTreePaged::Transaction scope(const BTreePaged::Transaction& t) {
        return BTreePaged::Transaction(t.t);
    }
}