module;
#include <coroutine>

export module keyvalue.engine;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
export import plexdb.btree;
import plexdb.btree.types;
export import plexdb.blob;
import plexdb.coroutine;

import keyvalue.glob;

export import keyvalue.engine.statements;

using namespace plexdb;

namespace keyvalue::engine {
    // Blob layout: [key_len: U64][key bytes][value_len: U64][value bytes]
    template<typename BlobT>
    coroutine::Task<void> write_entry(BlobT& b, String8 key, String8 value) {
        U64 total = sizeof(U64) * 2 + key.length + value.length;
        co_await plexdb::blob::resize(b, total);
        U64 off = 0;
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(&key.length),   sizeof(U64),  off); off += sizeof(U64);
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(key.data),      key.length,   off); off += key.length;
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(&value.length), sizeof(U64),  off); off += sizeof(U64);
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(value.data),    value.length, off);
    }

    template<typename BlobT>
    coroutine::Task<AutoString8> read_key(BlobT& b) {
        U64 key_len = 0;
        co_await plexdb::blob::get(b, reinterpret_cast<U8*>(&key_len), sizeof(U64), 0);
        DynamicArray<U8> buf;
        resize(buf, key_len);
        co_await plexdb::blob::get(b, buf.ptr, key_len, sizeof(U64));
        co_return AutoString8{String8{reinterpret_cast<const char*>(buf.ptr), key_len}};
    }

    template<typename BlobT>
    coroutine::Task<AutoString8> read_value(BlobT& b) {
        U64 key_len = 0, value_len = 0;
        U64 off = 0;
        co_await plexdb::blob::get(b, reinterpret_cast<U8*>(&key_len),   sizeof(U64), off); off += sizeof(U64) + key_len;
        co_await plexdb::blob::get(b, reinterpret_cast<U8*>(&value_len), sizeof(U64), off); off += sizeof(U64);
        DynamicArray<U8> buf;
        resize(buf, value_len);
        co_await plexdb::blob::get(b, buf.ptr, value_len, off);
        co_return AutoString8{String8{reinterpret_cast<const char*>(buf.ptr), value_len}};
    }
}

export namespace keyvalue::engine {
    struct ResultNull {};
    struct ResultEmptyArr {};
    struct ResultClose {};
    struct ResultSimpleStr   { String8 value; };
    struct ResultBulkStr     { AutoString8 value; };
    struct ResultInt         { S64 value; };
    struct ResultBulkArr     { DynamicArray<AutoString8> values; };
    struct ResultNullBulkArr { DynamicArray<Optional<AutoString8>> values; };
    struct ResultScan        { AutoString8 cursor; DynamicArray<AutoString8> keys; };
    struct ResultError       { AutoString8 message; };

    using ExecutionResult = TaggedUnion<
        ResultSimpleStr, ResultNull, ResultEmptyArr,
        ResultBulkStr, ResultInt,
        ResultBulkArr, ResultNullBulkArr,
        ResultScan, ResultError, ResultClose
    >;

    template<btree::BTree BT, plexdb::blob::Blob BlobT>
    struct Engine {
        BT index;
        DynamicMap<U64, BlobT> blobs;
        U64 next_blob_id = 0;

        Engine() requires SameAs<BT, btree::BTreeInMemory>
            : index{32, 32, sizeof(U64)} {}
    };

    using InMemoryEngine = Engine<btree::BTreeInMemory, plexdb::blob::BlobInMemory>;

    template<btree::BTree BT, plexdb::blob::Blob BlobT>
    coroutine::Task<ExecutionResult> execute(Engine<BT, BlobT>& engine, const Statement& statement) {
        return visit(statement.value, [&](const auto& v) -> coroutine::Task<ExecutionResult> {
            using T = Decay<decltype(v)>;

            if constexpr (SameAs<T, Ping>) {
                if (v.message) co_return ResultBulkStr{AutoString8{*v.message}};
                co_return ResultSimpleStr{"PONG"};
            } else if constexpr (SameAs<T, Quit>) {
                co_return ResultClose{};
            } else if constexpr (SameAs<T, Get>) {
                U64 id = 0;
                if (!co_await btree::find(engine.index, hash(String8{v.key}), reinterpret_cast<U8*>(&id), sizeof(U64)))
                    co_return ResultNull{};
                co_return ResultBulkStr{co_await read_value(*find(engine.blobs, id))};
            } else if constexpr (SameAs<T, Set>) {
                U64 key_hash = hash(String8{v.key});
                U64 id = 0;
                bool exists = co_await btree::find(engine.index, key_hash, reinterpret_cast<U8*>(&id), sizeof(U64));
                if (v.nx && exists)  co_return ResultNull{};
                if (v.xx && !exists) co_return ResultNull{};
                if (exists) {
                    co_await write_entry(*find(engine.blobs, id), String8{v.key}, String8{v.value});
                } else {
                    id = engine.next_blob_id++;
                    insert(engine.blobs, id, BlobT{});
                    co_await write_entry(*find(engine.blobs, id), String8{v.key}, String8{v.value});
                    co_await btree::insert(engine.index, key_hash, reinterpret_cast<const U8*>(&id), sizeof(U64));
                }
                co_return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Del>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    U64 key_hash = hash(String8{v.keys[i]});
                    U64 id = 0;
                    if (!co_await btree::find(engine.index, key_hash, reinterpret_cast<U8*>(&id), sizeof(U64))) continue;
                    remove(engine.blobs, id);
                    co_await btree::remove(engine.index, key_hash);
                    count++;
                }
                co_return ResultInt{S64(count)};
            } else if constexpr (SameAs<T, Exists>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    U64 id = 0;
                    if (co_await btree::find(engine.index, hash(String8{v.keys[i]}), reinterpret_cast<U8*>(&id), sizeof(U64)))
                        count++;
                }
                co_return ResultInt{S64(count)};
            } else if constexpr (SameAs<T, Mget>) {
                DynamicArray<Optional<AutoString8>> vals;
                for (U64 i = 0; i < v.keys.length; i++) {
                    U64 id = 0;
                    if (co_await btree::find(engine.index, hash(String8{v.keys[i]}), reinterpret_cast<U8*>(&id), sizeof(U64)))
                        push_back(vals, Optional<AutoString8>{co_await read_value(*find(engine.blobs, id))});
                    else
                        push_back(vals, Optional<AutoString8>{});
                }
                co_return ResultNullBulkArr{move(vals)};
            } else if constexpr (SameAs<T, Mset>) {
                for (U64 i = 0; i < v.pairs.length; i++) {
                    U64 key_hash = hash(String8{v.pairs[i].first});
                    U64 id = 0;
                    bool exists = co_await btree::find(engine.index, key_hash, reinterpret_cast<U8*>(&id), sizeof(U64));
                    if (exists) {
                        co_await write_entry(*find(engine.blobs, id),
                            String8{v.pairs[i].first}, String8{v.pairs[i].second});
                    } else {
                        id = engine.next_blob_id++;
                        insert(engine.blobs, id, BlobT{});
                        co_await write_entry(*find(engine.blobs, id),
                            String8{v.pairs[i].first}, String8{v.pairs[i].second});
                        co_await btree::insert(engine.index, key_hash, reinterpret_cast<const U8*>(&id), sizeof(U64));
                    }
                }
                co_return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Keys>) {
                String8 pattern = v.pattern;
                DynamicArray<AutoString8> keys;
                auto it = co_await btree::tbegin<U64>(engine.index);
                auto end_it = btree::tend<U64>(engine.index);
                while (it != end_it) {
                    AutoString8 key = co_await read_key(*find(engine.blobs, *it));
                    if (glob::match(String8{key}, pattern))
                        push_back(keys, move(key));
                    co_await it.advance();
                }
                co_return ResultBulkArr{move(keys)};
            } else if constexpr (SameAs<T, Scan>) {
                String8 pattern = v.match ? String8{*v.match} : String8{"*"};
                U64 count = v.count ? *v.count : 10;
                DynamicArray<AutoString8> result;

                auto it = v.cursor == 0
                    ? co_await btree::tbegin<U64>(engine.index)
                    : co_await btree::tfind_it<U64, btree::SearchStrategy::FirstGreaterEqual>(engine.index, v.cursor);
                auto end_it = btree::tend<U64>(engine.index);

                U64 scanned = 0;
                while (it != end_it && scanned < count) {
                    AutoString8 key = co_await read_key(*find(engine.blobs, *it));
                    if (glob::match(String8{key}, pattern))
                        push_back(result, move(key));
                    scanned++;
                    co_await it.advance();
                }

                U64 next_cursor = 0;
                if (it != end_it) {
                    AutoString8 next_key = co_await read_key(*find(engine.blobs, *it));
                    next_cursor = hash(String8{next_key});
                }
                co_return ResultScan{to_str(next_cursor), move(result)};
            } else if constexpr (SameAs<T, FlushDb> || SameAs<T, FlushAll>) {
                clear(engine.blobs);
                co_await btree::truncate(engine.index);
                co_return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, DbSize>) {
                co_return ResultInt{S64(co_await btree::size(engine.index))};
            } else if constexpr (SameAs<T, TypeOf>) {
                U64 id = 0;
                bool found = co_await btree::find(engine.index, hash(String8{v.key}), reinterpret_cast<U8*>(&id), sizeof(U64));
                co_return ResultSimpleStr{found ? "string" : "none"};
            } else if constexpr (SameAs<T, Cmd>) {
                co_return ResultEmptyArr{};
            } else if constexpr (SameAs<T, SelectDb>) {
                if (v.index != 0) assert_not_implemented("multiple databases");
                co_return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, ClientGetName>) {
                co_return ResultNull{};
            } else if constexpr (SameAs<T, Client>) {
                co_return ResultSimpleStr{"OK"};
            } else if constexpr (SameAs<T, Info>) {
                U64 db_size = co_await btree::size(engine.index);
                co_return ResultBulkStr{
                    "# Server\r\nredis_version:7.0.0\r\narch_bits:64\r\n# Keyspace\r\ndb0:keys="_as
                    + to_str(db_size)
                    + ",expires=0\r\n"_as
                };
            } else if constexpr (SameAs<T, Unknown>) {
                co_return ResultError{"unknown command '"_as + v.name + "'"_as};
            } else {
                assert_true(false, "missing statement type in execute");
                co_return ResultError{"internal error"_as};
            }
        });
    }
}
