export module plexdb.btree.types;

import plexdb.base;
import plexdb.coroutine;
import plexdb.btree.policy;

export namespace plexdb::btree {
    using CountType = U16;
    using NodeRef   = U64;

    // @padding
    struct Node {
        CountType key_count;
        NodeRef   prev, next;
    };

    struct Header {
        U64     depth;
        U64     size;
        NodeRef root;
        NodeRef leaves;
        U16     value_stride; // persisted so paged trees survive reopen without re-supplying the policy
    };

    template<typename T>
    concept HasTransaction = requires {
        typename T::Transaction;
    };

    template<typename B>
    concept TransactionConstructible = requires(B* b_ptr) {
        typename B::Transaction(b_ptr);
    };

    template<typename T>
    concept Transaction = requires(T& t) {
        { node_size(t) } -> SameAs<U32>;
        { t.begin()  }   -> SameAs<coroutine::Task<>>;
        { t.commit() }   -> SameAs<coroutine::Task<>>;
    };

    template<typename B>
    concept BTree =
        HasTransaction<B> &&
        TransactionConstructible<B> &&
        Transaction<typename B::Transaction> &&
        requires(const B& b) {
            { key_policy(b) }   -> KeyPolicy;
            { value_policy(b) } -> ValuePolicy;
        };

    enum class SearchStrategy {
        RequireEquality,
        FirstGreater,
        FirstGreaterEqual,
        LastLess,
        LastLessEqual,
    };

    // @note templated on policies so operator* can reconstruct the right accessor
    template<KeyPolicy KP, ValuePolicy VP>
    struct BasicIteratorImpl {
        const Node* leaf      = nullptr;
        NodeRef     ref       = 0;
        CountType   idx       = 0;
        U32         node_size = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        bool operator==(const BasicIteratorImpl& o) const noexcept {
            return ref == o.ref && idx == o.idx;
        }
        bool operator!=(const BasicIteratorImpl& o) const noexcept { return !(*this == o); }
    };

    // Backward-compat alias used in existing Iterator<BT,T> for U64+fixed-stride
    using IteratorImpl = BasicIteratorImpl<U64KeyPolicy, FixedValuePolicy>;
}
