export module plexdb.btree.node;

import plexdb.base;
import plexdb.arena;
import plexdb.btree.types;

export namespace plexdb::btree {
    KeyType*       keys_raw(Node* node);
    NodeRef*       children_raw(Node* node, const Header& h);
    U8*            values_raw(Node* node, const Header& h);

    const KeyType* keys_raw(const Node* node);
    const NodeRef* children_raw(const Node* node, const Header& h);
    const U8*      values_raw(const Node* node, const Header& h);

    TArrayView<KeyType, CountType>       keys(Node* node);
    TArrayView<NodeRef, CountType>       children(Node* node, const Header& h);
    ArrayView<CountType, U64, U8>        values(Node* node, const Header& h);

    TArrayView<const KeyType, CountType> keys(const Node* node);
    TArrayView<const NodeRef, CountType> children(const Node* node, const Header& h);
    ArrayView<CountType, U64, const U8>  values(const Node* node, const Header& h);

    void init_node(Node* node);

    Node* push_internal_node(const Header& h);
    Node* push_leaf_node(const Header& h);
}