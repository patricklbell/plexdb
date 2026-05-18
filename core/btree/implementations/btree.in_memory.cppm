module;
#include <coroutine>

export module plexdb.btree.in_memory;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.btree.node;

export namespace plexdb::btree {
    template<KeyPolicy KP = U64KeyPolicy, ValuePolicy VP = FixedStrideValuePolicy>
    struct BTreeInMemory {
        Header header{};
        U32    node_bytes = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        // Primary constructor
        BTreeInMemory(KP kp, VP vp, U32 node_bytes);

        // Backward-compat constructor — only available for the default policy pair
        BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride)
            requires (SameAs<KP, U64KeyPolicy> && SameAs<VP, FixedStrideValuePolicy>);

        ~BTreeInMemory();

        friend KP key_policy  (const BTreeInMemory& b) noexcept { return b.kp; }
        friend VP value_policy(const BTreeInMemory& b) noexcept { return b.vp; }

        struct Transaction {
            Transaction();
            explicit Transaction(BTreeInMemory* t);
            Transaction(Transaction&& t);

            Transaction(const Transaction&) = delete;
            Transaction& operator=(const Transaction&) = delete;

            coroutine::Task<> begin();
            coroutine::Task<> commit();

            friend U32 node_size(const Transaction& t) noexcept { return t.t->node_bytes; }

            friend coroutine::Task<const Header*> read_header(Transaction& t) {
                co_return &t.t->header;
            }
            friend coroutine::Task<Header*> update_header(Transaction& t) {
                co_return &t.t->header;
            }
            friend coroutine::Task<const Node*> read_node(Transaction&, const NodeRef& ref) {
                co_return reinterpret_cast<const Node*>(ref);
            }
            friend coroutine::Task<Node*> update_node(Transaction&, const NodeRef& ref) {
                co_return reinterpret_cast<Node*>(ref);
            }
            friend coroutine::Task<NodeRef> create_internal(Transaction& t) {
                co_return reinterpret_cast<NodeRef>(alloc_internal(t.t->node_bytes));
            }
            friend coroutine::Task<NodeRef> create_leaf(Transaction& t) {
                co_return reinterpret_cast<NodeRef>(alloc_leaf(t.t->node_bytes));
            }
            friend coroutine::Task<> delete_node(Transaction&, const NodeRef& ref) {
                os::deallocate(reinterpret_cast<void*>(ref));
                co_return;
            }

            BTreeInMemory* t;
        };
    };
}
