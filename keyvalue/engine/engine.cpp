module keyvalue.engine;

import plexdb.dynamic.containers;
import plexdb.tagged_union;

using namespace plexdb;

namespace keyvalue::engine {
    ExecutionResult execute(store::Store& store, const Statement& statement) {
        return visit(statement.value, [&](const auto& v) -> ExecutionResult {
            using T = RemoveCVRef<decltype(v)>;

            if constexpr (SameAs<T, Ping>) {
                if (v.message) return ResultBulkStr{*v.message};
                else           return ResultSimpleStr{"PONG"};
            } else if constexpr (SameAs<T, Quit>) {
                return ResultClose{};
            } else if constexpr (SameAs<T, Get>) {
                auto val = store::get(store, v.key);
                if (val) return ResultBulkStr{AutoString8{*val}};
                else     return ResultNull{};
            } else if constexpr (SameAs<T, Set>) {
                bool exists = store::exists(store, v.key) > 0;
                if (v.nx && exists)  return ResultNull{};
                if (v.xx && !exists) return ResultNull{};
                store::set(store, v.key, v.value);
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Del>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++)
                    count += store::del_one(store, v.keys[i]);
                return ResultInt{S64(count)};
            } else if constexpr (SameAs<T, Exists>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++)
                    count += store::exists(store, v.keys[i]);
                return ResultInt{S64(count)};
            } else if constexpr (SameAs<T, Mget>) {
                DynamicArray<Optional<AutoString8>> vals;
                for (U64 i = 0; i < v.keys.length; i++) {
                    auto val = store::get(store, v.keys[i]);
                    if (val) push_back(vals, Optional<AutoString8>{AutoString8{*val}});
                    else     push_back(vals, Optional<AutoString8>{});
                }
                return ResultNullBulkArr{move(vals)};
            } else if constexpr (SameAs<T, Mset>) {
                for (U64 i = 0; i < v.pairs.length; i++)
                    store::set(store, v.pairs[i].first, v.pairs[i].second);
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Keys>) {
                auto result = store::keys(store, v.pattern);
                DynamicArray<AutoString8> keys;
                for (U64 i = 0; i < result.length; i++)
                    push_back(keys, AutoString8{result[i]});
                return ResultBulkArr{move(keys)};
            } else if constexpr (SameAs<T, Scan>) {
                String8 pattern = v.match ? String8{*v.match} : String8{"*"};
                U64 count = v.count ? *v.count : 10;
                auto [next_cursor, matched] = store::scan(store, v.cursor, pattern, count);
                DynamicArray<AutoString8> keys;
                for (U64 i = 0; i < matched.length; i++)
                    push_back(keys, AutoString8{matched[i]});
                return ResultScan{to_str(next_cursor), move(keys)};
            } else if constexpr (SameAs<T, FlushDb>) {
                store::flush(store); return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, FlushAll>) {
                store::flush(store); return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, DbSize>) {
                return ResultInt{S64(store::dbsize(store))};
            } else if constexpr (SameAs<T, TypeOf>) {
                return ResultSimpleStr{store::exists(store, v.key) > 0 ? "string" : "none"};
            } else if constexpr (SameAs<T, Cmd>) {
                return ResultEmptyArr{};
            } else if constexpr (SameAs<T, SelectDb>) {
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, ClientGetName>) {
                return ResultNull{};
            } else if constexpr (SameAs<T, Client>) {
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Info>) {
                return ResultBulkStr{
                    "# Server\r\nredis_version:7.0.0\r\narch_bits:64\r\n# Keyspace\r\ndb0:keys="_as
                    + to_str(store::dbsize(store))
                    + ",expires=0\r\n"_as
                };
            } else if constexpr (SameAs<T, Unknown>) {
                return ResultError{"unknown command '"_as + v.name + "'"_as};
            } else {
                static_assert(!SameAs<T,T>, "missing type case");
            }
        });
    }
}
