export module plexdb.btree.constraint;

import plexdb.base;
import plexdb.coroutine;

import plexdb.btree.policy;
import plexdb.btree.in_memory.detail;
import plexdb.btree.paged.detail;

export namespace plexdb::btree {
    template<typename B>
    concept BTree = requires(const B& b) {
        { key_policy(b)   } -> KeyPolicy;
        { value_policy(b) } -> ValuePolicy;
        { node_size(b)    } -> SameAs<U32>;
        // @todo fill in rest
    };
}
