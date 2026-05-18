export module plexdb.btree.in_memory.detail;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.pager.transaction;

import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.btree.node;
import plexdb.btree.in_memory;

export namespace plexdb::btree {
    template<KeyPolicy KP, ValuePolicy VP>
    KP key_policy(const BTreeInMemory<KP,VP>& b) noexcept {
        return b.kp;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    VP value_policy(const BTreeInMemory<KP,VP>& b) noexcept {
        return b.vp;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    U32 node_size(const BTreeInMemory<KP,VP>& b) noexcept {
        return b.node_bytes;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Header*> read_header(BTreeInMemory<KP,VP>& b) {
        co_return &b.header;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Header*> update_header(BTreeInMemory<KP,VP>& b) {
        co_return &b.header;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Node*> read_node(BTreeInMemory<KP,VP>&, const NodeRef& ref) {
        co_return reinterpret_cast<const Node*>(ref);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Node*> update_node(BTreeInMemory<KP,VP>&, const NodeRef& ref) {
        co_return reinterpret_cast<Node*>(ref);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_internal(BTreeInMemory<KP,VP>& b) {
        co_return reinterpret_cast<NodeRef>(alloc_internal(b.node_bytes));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_leaf(BTreeInMemory<KP,VP>& b) {
        co_return reinterpret_cast<NodeRef>(alloc_leaf(b.node_bytes));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> delete_node(BTreeInMemory<KP,VP>&, const NodeRef& ref) {
        os::deallocate(reinterpret_cast<void*>(ref));
        co_return;
    }
}
