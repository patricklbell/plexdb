module plexdb.btree.node;

import plexdb.os;

namespace plexdb::btree {
    KeyType*       keys_raw(Node* node)                            { return reinterpret_cast<KeyType*>(reinterpret_cast<U8*>(node)+sizeof(*node)); }
    NodeRef*       children_raw(Node* node, const Header& h)       { return reinterpret_cast<NodeRef*>(reinterpret_cast<U8*>(node)+sizeof(*node)+h.max_keys_per_internal*sizeof(KeyType)); }
    U8*            values_raw(Node* node, const Header& h)         { return reinterpret_cast<U8*>(node)+sizeof(*node)+h.max_keys_per_leaf*sizeof(KeyType); }

    const KeyType* keys_raw(const Node* node)                      { return reinterpret_cast<const KeyType*>(reinterpret_cast<const U8*>(node)+sizeof(*node)); }
    const NodeRef* children_raw(const Node* node, const Header& h) { return reinterpret_cast<const NodeRef*>(reinterpret_cast<const U8*>(node)+sizeof(*node)+h.max_keys_per_internal*sizeof(KeyType)); }
    const U8*      values_raw(const Node* node, const Header& h)   { return reinterpret_cast<const U8*>(node)+sizeof(*node)+h.max_keys_per_leaf*sizeof(KeyType); }

    TArrayView<KeyType, CountType>       keys(Node* node)                            { return TArrayView{keys_raw(node), node->key_count}; }
    TArrayView<NodeRef, CountType>       children(Node* node, const Header& h)       { return TArrayView{children_raw(node, h), static_cast<CountType>(node->key_count+1)}; }
    ArrayView<CountType, U64, U8>        values(Node* node, const Header& h)         { return ArrayView{values_raw(node, h), h.value_stride, node->key_count}; }

    TArrayView<const KeyType, CountType> keys(const Node* node)                      { return TArrayView{keys_raw(node), node->key_count}; }
    TArrayView<const NodeRef, CountType> children(const Node* node, const Header& h) { return TArrayView{children_raw(node, h), static_cast<CountType>(node->key_count+1)}; }
    ArrayView<CountType, U64, const U8>  values(const Node* node, const Header& h)   { return ArrayView{values_raw(node, h), h.value_stride, node->key_count}; }

    void init_node(Node* node) {
        node->key_count = 0;
        node->prev = ~0u;
        node->next = ~0u;
    }

    Node* push_internal_node(const Header& h) {
        U64 node_size = sizeof(Node) + h.max_keys_per_internal*sizeof(KeyType) + (h.max_keys_per_internal+1)*sizeof(Node*);
        Node* node = reinterpret_cast<Node*>(os::allocate(node_size));
        init_node(node);
        return node;
    }

    Node* push_leaf_node(const Header& h) {
        U64 node_size = sizeof(Node) + h.max_keys_per_leaf*sizeof(KeyType) + h.max_keys_per_leaf*h.value_stride;
        Node* node = reinterpret_cast<Node*>(os::allocate(node_size));
        init_node(node);
        return node;
    }
}