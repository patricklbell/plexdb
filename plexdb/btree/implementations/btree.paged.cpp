module plexdb.btree.paged;

import plexdb.pager;
import plexdb.btree.node;

namespace plexdb::btree {
    BTreePaged::BTreePaged()
        : pager(nullptr), header_page(0) {}
    BTreePaged::BTreePaged(Pager* pager, U64 header_page)
        : pager(pager), header_page(header_page) {}

    U64 create_paged(Pager& pager, U64 value_stride) {
        Header header = Header{
            .value_stride = value_stride,
            .depth = 0,
            .size = 0,
            .max_keys_per_internal = get_max_internal_nodes_in_bytes(pager.header.page_size),
            .max_keys_per_leaf = get_max_leaf_nodes_in_bytes(pager.header.page_size, value_stride),
        };
        U64 header_page = pager::new_page(pager);
        header.root = reinterpret_cast<NodeRef>(pager::new_page(pager));
        header.leaves = header.root;
        os::memory_copy(pager::rwpage(pager, header_page), &header);

        return header_page;
    }

    // @todo
    BTreePaged::Transaction::Transaction(): t(nullptr) {}
    BTreePaged::Transaction::Transaction(BTreePaged* t): t(t) {
        assert_true(t->pager != nullptr, "cannot create a transaction for an uninitialized btree");
    }
    BTreePaged::Transaction::Transaction(Transaction&& other): t(other.t) {}
    BTreePaged::Transaction::~Transaction() {}
    BTreePaged::Transaction scope(const BTreePaged::Transaction& t) {
        return BTreePaged::Transaction(t.t);
    }
}