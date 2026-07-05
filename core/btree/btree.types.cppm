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
    static_assert(sizeof(Node) % alignof(NodeRef) == 0, "key/child regions immediately follow Node and must stay NodeRef-aligned");

    struct Header {
        U64     depth;
        U64     size;
        NodeRef root;
        NodeRef leaves;
        // @note high-water mark of every inserted key's stored size; bounds how
        // large a cascaded separator can be, since separators are always copies
        // of previously-inserted keys, never synthesized
        U16 max_key_bytes = 0;
    };

    enum class SearchStrategy {
        RequireEquality,
        FirstGreater,
        FirstGreaterEqual,
        LastLess,
        LastLessEqual,
    };
}
