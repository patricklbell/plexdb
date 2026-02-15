export module plexdb.btree.print;

import plexdb.base;
import plexdb.os;
import plexdb.btree;
import plexdb.btree.detail;

using namespace plexdb::btree;

namespace plexdb {
    template<typename T>
    AutoString8 node_to_str(const Node* node, const Header& h, bool is_leaf) {
        AutoString8 res = "["_as;
        for (int i = 0; i < node->key_count; i++) {
            res += to_str(keys(node)[i]);

            if (is_leaf) {
                T value = os::memory_cast<T>(values(node, h)[i]);
                res += ": " + to_str(value);
            }

            res += ((i == node->key_count-1) ? "" : ", ");
        }
        return res + "]";
    }

    template<typename T, typename Transaction>
    AutoString8 btree_to_str_recursive(Transaction& t, const Node* node, int depth, const String8& prepend, bool end) {
        const auto& h = *read_header(t);

        AutoString8 res = prepend + String8(end ? " └" : " ├");
        res += node_to_str<T>(node, h, depth == h.depth) + "\n";
        if (depth < h.depth) {
            auto cs = children(node, h);
            for (auto& child_ref : cs) {
                const Node* child = read_node(t, child_ref);
                res += btree_to_str_recursive<T>(t, child, depth+1, prepend + (end ? "   " : " | "), &child_ref == cs.end()-1);
            }
        }
        return res;
    }

    export template<typename T, typename BTree>
        requires Either<BTree, BTreeInMemory, BTreePaged>
    AutoString8 to_str(const Tag<T, BTree>& tag) {
        using Transaction = typename BTree::Transaction;
        Transaction t{tag.value};

        AutoString8 res = ""_as;
        res += "[tree, depth=" + to_str(read_header(t)->depth) + "]\n";
        res += btree_to_str_recursive<T>(t, read_node(t, read_header(t)->root), 0, "", true);
        return res;
    }
}