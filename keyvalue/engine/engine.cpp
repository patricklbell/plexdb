module keyvalue.engine;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.tagged_union;

using namespace plexdb;

namespace keyvalue::engine {
    static bool glob_match(const char* pattern, U64 plen, const char* text, U64 tlen) {
        if (plen == 1 && pattern[0] == '*') return true;
        U64 pi = 0, ti = 0;
        U64 star_pi = U64(-1), star_ti = 0;
        while (ti < tlen) {
            if (pi < plen && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
                pi++; ti++;
            } else if (pi < plen && pattern[pi] == '*') {
                star_pi = pi++;
                star_ti = ti;
            } else if (star_pi != U64(-1)) {
                pi = star_pi + 1;
                ti = ++star_ti;
            } else {
                return false;
            }
        }
        while (pi < plen && pattern[pi] == '*') pi++;
        return pi == plen;
    }

    static bool key_matches(const AutoString8& key, String8 pattern) {
        return glob_match(pattern.data, pattern.length, key.c_str, key.length);
    }

    ExecutionResult execute(Engine& engine, const Statement& statement) {
        return visit(statement.value, [&](const auto& v) -> ExecutionResult {
            using T = RemoveCVRef<decltype(v)>;

            if constexpr (SameAs<T, Ping>) {
                if (v.message) return ResultBulkStr{AutoString8{*v.message}};
                else           return ResultSimpleStr{"PONG"};
            } else if constexpr (SameAs<T, Quit>) {
                return ResultClose{};
            } else if constexpr (SameAs<T, Get>) {
                AutoString8 k{v.key};
                auto it = find_it(engine.data, k);
                if (it != engine.data.end()) return ResultBulkStr{AutoString8{(*it).second}};
                return ResultNull{};
            } else if constexpr (SameAs<T, Set>) {
                AutoString8 k{v.key};
                auto it = find_it(engine.data, k);
                bool exists = (it != engine.data.end());
                if (v.nx && exists)  return ResultNull{};
                if (v.xx && !exists) return ResultNull{};
                insert(engine.data, AutoString8{v.key}, AutoString8{v.value});
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Del>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    AutoString8 k{v.keys[i]};
                    U64 slot_idx = 0, pair_idx = 0;
                    if (!find_slot_and_pair(engine.data, k, slot_idx, pair_idx)) continue;
                    auto& bucket = engine.data.slots[slot_idx];
                    if (pair_idx + 1 < bucket.length)
                        bucket[pair_idx] = plexdb::move(bucket[bucket.length - 1]);
                    pop_back(bucket);
                    count++;
                }
                return ResultInt{S64(count)};
            } else if constexpr (SameAs<T, Exists>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    AutoString8 k{v.keys[i]};
                    U64 si = 0, pi = 0;
                    if (find_slot_and_pair(engine.data, k, si, pi)) count++;
                }
                return ResultInt{S64(count)};
            } else if constexpr (SameAs<T, Mget>) {
                DynamicArray<Optional<AutoString8>> vals;
                for (U64 i = 0; i < v.keys.length; i++) {
                    AutoString8 k{v.keys[i]};
                    auto it = find_it(engine.data, k);
                    if (it != engine.data.end()) push_back(vals, Optional<AutoString8>{AutoString8{(*it).second}});
                    else                     push_back(vals, Optional<AutoString8>{});
                }
                return ResultNullBulkArr{move(vals)};
            } else if constexpr (SameAs<T, Mset>) {
                for (U64 i = 0; i < v.pairs.length; i++)
                    insert(engine.data, AutoString8{v.pairs[i].first}, AutoString8{v.pairs[i].second});
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Keys>) {
                String8 pattern = v.pattern;
                DynamicArray<AutoString8> keys;
                for (U64 si = 0; si < engine.data.slots.length; si++) {
                    for (U64 pi = 0; pi < engine.data.slots[si].length; pi++) {
                        if (key_matches(engine.data.slots[si][pi].first, pattern))
                            push_back(keys, AutoString8{engine.data.slots[si][pi].first});
                    }
                }
                return ResultBulkArr{move(keys)};
            } else if constexpr (SameAs<T, Scan>) {
                String8 pattern = v.match ? String8{*v.match} : String8{"*"};
                U64 count = v.count ? *v.count : 10;
                DynamicArray<AutoString8> result;
                U64 pos = 0, scanned = 0, next_cursor = 0;
                for (U64 si = 0; si < engine.data.slots.length; si++) {
                    const auto& slot = engine.data.slots[si];
                    for (U64 pi = 0; pi < slot.length; pi++) {
                        if (pos < v.cursor) { pos++; continue; }
                        if (key_matches(slot[pi].first, pattern))
                            push_back(result, AutoString8{slot[pi].first});
                        pos++;
                        scanned++;
                        if (scanned >= count) {
                            bool more = (si < engine.data.slots.length - 1 || pi + 1 < slot.length);
                            next_cursor = more ? pos : 0;
                            return ResultScan{to_str(next_cursor), move(result)};
                        }
                    }
                }
                return ResultScan{to_str(U64(0)), move(result)};
            } else if constexpr (SameAs<T, FlushDb>) {
                clear(engine.data); return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, FlushAll>) {
                clear(engine.data); return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, DbSize>) {
                return ResultInt{S64(length(engine.data))};
            } else if constexpr (SameAs<T, TypeOf>) {
                AutoString8 k{v.key};
                auto it = find_it(engine.data, k);
                return ResultSimpleStr{it != engine.data.end() ? "string" : "none"};
            } else if constexpr (SameAs<T, Cmd>) {
                return ResultEmptyArr{};
            } else if constexpr (SameAs<T, SelectDb>) {
                if (v.index != 0) assert_not_implemented("multiple databases");
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, ClientGetName>) {
                return ResultNull{};
            } else if constexpr (SameAs<T, Client>) {
                return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Info>) {
                return ResultBulkStr{
                    "# Server\r\nredis_version:7.0.0\r\narch_bits:64\r\n# Keyspace\r\ndb0:keys="_as
                    + to_str(length(engine.data))
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
