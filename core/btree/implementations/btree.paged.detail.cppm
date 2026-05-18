module;
#include <coroutine>

export module plexdb.btree.paged.detail;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.pager.transaction;
import plexdb.pager;

import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.btree.paged;

export namespace plexdb::btree {
    template<KeyPolicy KP, ValuePolicy VP>
    KP key_policy(const BTreePaged<KP,VP>& b) noexcept {
        return b.kp;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    VP value_policy(const BTreePaged<KP,VP>& b) noexcept {
        return b.vp;
    }
    template<KeyPolicy KP, ValuePolicy VP>
    U32 node_size(const BTreePaged<KP,VP>& b) noexcept {
        return static_cast<U32>(b.pager->header.page_size);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Header*> read_header(BTreePaged<KP,VP>& b) {
        co_return reinterpret_cast<const Header*>(
            co_await pager::rpage(*b.pager, b.header_page));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Header*> update_header(BTreePaged<KP,VP>& b) {
        co_return reinterpret_cast<Header*>(
            co_await pager::rwpage(*b.pager, b.header_page));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Node*> read_node(BTreePaged<KP,VP>& b, const NodeRef& ref) {
        co_return reinterpret_cast<const Node*>(
            co_await pager::rpage(*b.pager, ref));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Node*> update_node(BTreePaged<KP,VP>& b, const NodeRef& ref) {
        co_return reinterpret_cast<Node*>(
            co_await pager::rwpage(*b.pager, ref));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_internal(BTreePaged<KP,VP>& b) {
        co_return co_await pager::new_page(*b.pager);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_leaf(BTreePaged<KP,VP>& b) {
        co_return co_await pager::new_page(*b.pager);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> delete_node(BTreePaged<KP,VP>& b, const NodeRef& ref) {
        co_await pager::delete_page(*b.pager, ref);
    }
}
