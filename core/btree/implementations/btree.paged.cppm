module;
#include <coroutine>

export module plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.pager;

export namespace plexdb::btree {
    template<KeyPolicy KP = U64KeyPolicy, ValuePolicy VP = FixedValuePolicy>
    struct BTreePaged {
        Pager* pager = nullptr;
        U64    header_page = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        BTreePaged() = default;
        BTreePaged(Pager* pager, U64 header_page, KP kp = {}, VP vp = {});

        friend KP key_policy  (const BTreePaged& b) noexcept { return b.kp; }
        friend VP value_policy(const BTreePaged& b) noexcept { return b.vp; }

        struct Transaction {
            Transaction();
            explicit Transaction(BTreePaged* btree);
            Transaction(Transaction&& t);
            Transaction& operator=(Transaction&& other);
            ~Transaction();

            Transaction(const Transaction&) = delete;
            Transaction& operator=(const Transaction&) = delete;

            coroutine::Task<> begin();
            coroutine::Task<> commit();

            friend U32 node_size(const Transaction& t) noexcept {
                return static_cast<U32>(t.t->pager->header.page_size);
            }

            friend coroutine::Task<const Header*> read_header(Transaction& t) {
                co_return reinterpret_cast<const Header*>(
                    co_await pager::rpage(*t.t->pager, t.t->header_page));
            }
            friend coroutine::Task<Header*> update_header(Transaction& t) {
                co_return reinterpret_cast<Header*>(
                    co_await pager::rwpage(*t.t->pager, t.t->header_page));
            }
            friend coroutine::Task<const Node*> read_node(Transaction& t, const NodeRef& ref) {
                co_return reinterpret_cast<const Node*>(
                    co_await pager::rpage(*t.t->pager, ref));
            }
            friend coroutine::Task<Node*> update_node(Transaction& t, const NodeRef& ref) {
                co_return reinterpret_cast<Node*>(
                    co_await pager::rwpage(*t.t->pager, ref));
            }
            friend coroutine::Task<NodeRef> create_internal(Transaction& t) {
                co_return co_await pager::new_page(*t.t->pager);
            }
            friend coroutine::Task<NodeRef> create_leaf(Transaction& t) {
                co_return co_await pager::new_page(*t.t->pager);
            }
            friend coroutine::Task<> delete_node(Transaction& t, const NodeRef& ref) {
                co_await pager::delete_page(*t.t->pager, ref);
            }

            bool started_transaction = false;
            BTreePaged* t = nullptr;
        };
    };

    // Create a new paged tree; returns the header page id
    template<KeyPolicy KP = U64KeyPolicy, ValuePolicy VP = FixedValuePolicy>
    coroutine::Task<U64> create_paged(Pager& pager, KP kp = {}, VP vp = {});

    // Backward-compat overload: create_paged(pager, value_stride)
    coroutine::Task<U64> create_paged(Pager& pager, U64 value_stride);
}
