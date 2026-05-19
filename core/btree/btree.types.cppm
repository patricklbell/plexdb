export module plexdb.btree.types;

import plexdb.base;
import plexdb.coroutine;

export namespace plexdb::btree {
    using CountType = U16;
    using NodeRef   = U64;
    using SizeType  = U16;

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
    };

    enum class SearchStrategy {
        RequireEquality,
        FirstGreater,
        FirstGreaterEqual,
        LastLess,
        LastLessEqual,
    };
}
