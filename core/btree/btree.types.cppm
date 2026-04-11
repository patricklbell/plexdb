export module plexdb.btree.types;

import plexdb.base;

export namespace plexdb::btree {
    using KeyType = U64;
    using CountType = U16;
    using NodeRef = U64;

    // @padding
    struct Node {
        CountType key_count;
        NodeRef prev, next;
    };

    // @padding
    struct Header {
        U64 value_stride;
        U64 depth;
        U64 size;
        NodeRef root;
        NodeRef leaves;
        CountType max_keys_per_internal;
        CountType max_keys_per_leaf;
    };

    template<typename T>
    concept HasTransaction = requires {
        typename T::Transaction;
    };

    template <typename B>
    concept TransactionConstructible = requires(B* b_ptr) {
        typename B::Transaction(b_ptr);
    };

    template<typename T>
    concept Transaction = requires(T& t, NodeRef ref) {
        { read_header(t) } -> SameAs<const Header*>;
        { update_header(t) } -> SameAs<Header*>;
        
        { create_internal(t) } -> SameAs<NodeRef>;
        { create_leaf(t) } -> SameAs<NodeRef>;
        { read_node(t, ref) } -> SameAs<const Node*>;
        { update_node(t, ref) } -> SameAs<Node*>;
        { delete_node(t, ref) } -> SameAs<void>;
    };

    template<typename B>
    concept BTree = HasTransaction<B> && TransactionConstructible<B> && Transaction<typename B::Transaction>;
}