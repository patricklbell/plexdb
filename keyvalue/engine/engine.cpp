module keyvalue.engine;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.os;
import plexdb.tagged_union;
import keyvalue.protocol;
import keyvalue.store;

using namespace plexdb;
using namespace keyvalue::protocol;

namespace keyvalue::engine {
    static bool ci_eq(String8 a, const char* b) {
        String8 bs{b};
        if (a.length != bs.length) return false;
        for (U64 i = 0; i < a.length; i++) {
            char ca = a.data[i], cb = b[i];
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) return false;
        }
        return true;
    }

    bool execute(store::Store& store, const Statement& statement, DynamicArray<U8>& out) {
        return visit(statement.value, [&](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;

            if constexpr (SameAs<T, Ping>) {
                if (v.message) append_bulk_string(out, *v.message);
                else           append_simple_string(out, "PONG");
                return true;
            } else if constexpr (SameAs<T, Quit>) {
                append_simple_string(out, "OK");
                return false;
            } else if constexpr (SameAs<T, Get>) {
                auto val = store::get(store, v.key);
                if (val) append_bulk_string(out, *val);
                else     append_null_bulk_string(out);
                return true;
            } else if constexpr (SameAs<T, Set>) {
                bool exists = store::exists(store, v.key) > 0;
                if (v.nx && exists)  { append_null_bulk_string(out); return true; }
                if (v.xx && !exists) { append_null_bulk_string(out); return true; }
                store::set(store, v.key, v.value);
                append_simple_string(out, "OK");
                return true;
            } else if constexpr (SameAs<T, Del>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++)
                    count += store::del_one(store, v.keys[i]);
                append_integer(out, S64(count));
                return true;
            } else if constexpr (SameAs<T, Exists>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++)
                    count += store::exists(store, v.keys[i]);
                append_integer(out, S64(count));
                return true;
            } else if constexpr (SameAs<T, Mget>) {
                append_array_header(out, S64(v.keys.length));
                for (U64 i = 0; i < v.keys.length; i++) {
                    auto val = store::get(store, v.keys[i]);
                    if (val) append_bulk_string(out, *val);
                    else     append_null_bulk_string(out);
                }
                return true;
            } else if constexpr (SameAs<T, Mset>) {
                for (U64 i = 0; i < v.pairs.length; i++)
                    store::set(store, v.pairs[i].first, v.pairs[i].second);
                append_simple_string(out, "OK");
                return true;
            } else if constexpr (SameAs<T, Keys>) {
                auto result = store::keys(store, v.pattern);
                append_array_header(out, S64(result.length));
                for (U64 i = 0; i < result.length; i++)
                    append_bulk_string(out, result[i]);
                return true;
            } else if constexpr (SameAs<T, Scan>) {
                String8 pattern = v.match ? String8{*v.match} : String8{"*"};
                U64 count = v.count ? *v.count : 10;
                auto [next_cursor, matched] = store::scan(store, v.cursor, pattern, count);
                append_array_header(out, 2);
                AutoString8 nc = to_str(next_cursor);
                append_bulk_string(out, nc);
                append_array_header(out, S64(matched.length));
                for (U64 i = 0; i < matched.length; i++)
                    append_bulk_string(out, matched[i]);
                return true;
            } else if constexpr (SameAs<T, FlushDb>)  {
                store::flush(store); append_simple_string(out, "OK"); return true;
            } else if constexpr (SameAs<T, FlushAll>) {
                store::flush(store); append_simple_string(out, "OK"); return true;
            } else if constexpr (SameAs<T, DbSize>) {
                append_integer(out, S64(store::dbsize(store)));
                return true;
            } else if constexpr (SameAs<T, TypeOf>) {
                append_simple_string(out, store::exists(store, v.key) > 0 ? "string" : "none");
                return true;
            } else if constexpr (SameAs<T, SelectDb>) {
                append_simple_string(out, "OK"); return true;
            } else if constexpr (SameAs<T, Client>) {
                if (v.args.length >= 1 && ci_eq(String8{v.args[0].c_str, v.args[0].length}, "GETNAME"))
                    append_null_bulk_string(out);
                else
                    append_simple_string(out, "OK");
                return true;
            } else if constexpr (SameAs<T, Info>) {
                append_bulk_string(
                    out,
                    "# Server\r\n"
                    "redis_version:7.0.0\r\n"
                    "arch_bits:64\r\n"
                    "# Keyspace\r\n"
                    "db0:keys="_as + to_str(store::dbsize(store)) + ",expires=0\r\n"_as
                );
                return true;
            } else if constexpr (SameAs<T, Unknown>) {
                append_error(out, "ERR", "unknown command '"_as + v.name + "'"_as);
                return true;
            } else {
                static_assert(!SameAs<T,T>, "missing type case");
            }
        });
    }
}
