module plexdb.btree.node;

import plexdb.os;

namespace plexdb::btree {
    // ========================================================================
    // capacity helpers
    // ========================================================================
    CountType max_keys_internal(U32 node_size) noexcept {
        assert_true(node_size >= sizeof(Node) + sizeof(U64) + 2*sizeof(NodeRef),
                    "enough bytes for at least one key in internal node");
        U64 count = min(
            static_cast<U64>((node_size - sizeof(Node) - sizeof(NodeRef)) / (sizeof(U64) + sizeof(NodeRef))),
            static_cast<U64>(NumericLimits<CountType>::max())
        );
        return static_cast<CountType>(count);
    }

    CountType max_keys_leaf(U32 node_size, U16 val_stride) noexcept {
        assert_true(node_size >= sizeof(Node) + sizeof(U64) + val_stride,
                    "enough bytes for at least one key in leaf node");
        U64 count = min(
            static_cast<U64>((node_size - sizeof(Node)) / (sizeof(U64) + val_stride)),
            static_cast<U64>(NumericLimits<CountType>::max())
        );
        return static_cast<CountType>(count);
    }

    // ========================================================================
    // raw pointer accessors
    // ========================================================================
    KeyType*       keys_raw(Node* node) {
        return reinterpret_cast<KeyType*>(reinterpret_cast<U8*>(node) + sizeof(Node));
    }
    const KeyType* keys_raw(const Node* node) {
        return reinterpret_cast<const KeyType*>(reinterpret_cast<const U8*>(node) + sizeof(Node));
    }

    NodeRef* children_raw(Node* node, U32 node_size) {
        CountType mk = max_keys_internal(node_size);
        return reinterpret_cast<NodeRef*>(reinterpret_cast<U8*>(node) + sizeof(Node) + mk * sizeof(KeyType));
    }
    const NodeRef* children_raw(const Node* node, U32 node_size) {
        CountType mk = max_keys_internal(node_size);
        return reinterpret_cast<const NodeRef*>(reinterpret_cast<const U8*>(node) + sizeof(Node) + mk * sizeof(KeyType));
    }

    U8* values_raw(Node* node, U32 node_size, U16 val_stride) {
        CountType mk = max_keys_leaf(node_size, val_stride);
        return reinterpret_cast<U8*>(node) + sizeof(Node) + mk * sizeof(KeyType);
    }
    const U8* values_raw(const Node* node, U32 node_size, U16 val_stride) {
        CountType mk = max_keys_leaf(node_size, val_stride);
        return reinterpret_cast<const U8*>(node) + sizeof(Node) + mk * sizeof(KeyType);
    }

    // ========================================================================
    // view accessors
    // ========================================================================
    TArrayView<KeyType, CountType> keys(Node* node) {
        return TArrayView{keys_raw(node), node->key_count};
    }
    TArrayView<const KeyType, CountType> keys(const Node* node) {
        return TArrayView{keys_raw(node), node->key_count};
    }

    TArrayView<NodeRef, CountType> children(Node* node, U32 node_size) {
        return TArrayView{children_raw(node, node_size), static_cast<CountType>(node->key_count + 1)};
    }
    TArrayView<const NodeRef, CountType> children(const Node* node, U32 node_size) {
        return TArrayView{children_raw(node, node_size), static_cast<CountType>(node->key_count + 1)};
    }

    ArrayView<CountType, U16, U8> values(Node* node, U32 node_size, U16 val_stride) {
        return ArrayView{values_raw(node, node_size, val_stride), val_stride, node->key_count};
    }
    ArrayView<CountType, U16, const U8> values(const Node* node, U32 node_size, U16 val_stride) {
        return ArrayView{values_raw(node, node_size, val_stride), val_stride, node->key_count};
    }

    // ========================================================================
    // node init and allocation
    // ========================================================================
    void init_node(Node* node) {
        node->key_count = 0;
        node->prev = 0;
        node->next = 0;
    }

    Node* alloc_internal(U32 node_size) {
        Node* node = reinterpret_cast<Node*>(os::allocate(node_size));
        init_node(node);
        return node;
    }

    Node* alloc_leaf(U32 node_size) {
        Node* node = reinterpret_cast<Node*>(os::allocate(node_size));
        init_node(node);
        return node;
    }
}
