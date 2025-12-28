module plexdb.btree.paged.detail;

namespace plexdb::btree {
    Settings& get_settings(BTreePaged& btree) {
        assert_not_implemented();
    }
    NodeRef   get_root(BTreePaged& btree) {
        assert_not_implemented();
    }
    void      set_root(BTreePaged& btree, const NodeRef& new_root) {
        assert_not_implemented();
    }
    NodeRef   create_internal(BTreePaged& btree) {
        assert_not_implemented();
    }
    NodeRef   create_leaf(BTreePaged& btree) {
        assert_not_implemented();
    }
    void      delete_node(BTreePaged& btree, const NodeRef& ref) {
        assert_not_implemented();
    }
    Node*     rnode(BTreePaged& btree, const NodeRef& ref) {
        assert_not_implemented();
    }
    Node*     rwnode(BTreePaged& btree, const NodeRef& ref) {
        assert_not_implemented();
    }
}