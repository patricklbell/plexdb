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

    template<typename BlobT>
    void free_blob(U64 raw) {
        if constexpr (SameAs<BlobT, plexdb::blob::BlobInMemory>)
            delete reinterpret_cast<BlobT*>(raw);
        // For paged blobs: pager manages lifetime; free via blob::remove in a coroutine context
    }

    template<typename BlobT>
    BlobT* alloc_blob() {
        if constexpr (SameAs<BlobT, plexdb::blob::BlobInMemory>)
            return new BlobT{};
        else {
            assert_not_implemented("paged blob creation");
            return nullptr;
        }
    }
}

export namespace keyvalue::engine {
    struct ResultNull {};
    struct ResultEmptyArr {};
    struct ResultClose {};
    struct ResultSimpleStr { String8 value; };
    struct ResultBulkStr   { AutoString8 value; };
    struct ResultInt       { S64 value; };
    struct ResultBulkArr   { DynamicArray<AutoString8> values; };
    struct ResultNullBulkArr { DynamicArray<Optional<AutoString8>> values; };
    struct ResultScan      { AutoString8 cursor; DynamicArray<AutoString8> keys; };
    struct ResultError     { AutoString8 message; };

    using ExecutionResult = TaggedUnion<
        ResultSimpleStr, ResultNull, ResultEmptyArr,
        ResultBulkStr, ResultInt,
        ResultBulkArr, ResultNullBulkArr,
        ResultScan, ResultError, ResultClose
    >;

    template<btree::BTree BT, plexdb::blob::Blob BlobT>
    struct Engine {
        BT index;

        Engine() requires SameAs<BT, btree::BTreeInMemory>
            : index{32, 32, sizeof(U64)} {}
    };

    using InMemoryEngine = Engine<btree::BTreeInMemory, plexdb::blob::BlobInMemory>;

    template<btree::BTree BT, plexdb::blob::Blob BlobT>
    coroutine::Task<ExecutionResult> execute(Engine<BT, BlobT>& engine, const Statement& statement) {
        return visit(statement, [&](const auto& v) {
            using T = Decay<decltype(v)>;

            if (SameAs<T, Ping>) {
                if (v.message) co_return ResultBulkStr{AutoString8{*v.message}};
                co_return ResultSimpleStr{"PONG"};
            } else if (SameAs<T, Quit>) {
                co_return ResultClose{};
            } else if (SameAs<T, Get>) {
                U64 raw = 0;
                if (!co_await btree::find(engine.index, hash(String8{v.key}), reinterpret_cast<U8*>(&raw), sizeof(U64)))
                    co_return ResultNull{};
                co_return ResultBulkStr{co_await read_value(*reinterpret_cast<BlobT*>(raw))};
            } else if (SameAs<T, Set>) {
                U64 key_hash = hash(String8{v.key});
                U64 raw = 0;
                bool exists = co_await btree::find(engine.index, key_hash, reinterpret_cast<U8*>(&raw), sizeof(U64));
                if (v.nx && exists)  co_return ResultNull{};
                if (v.xx && !exists) co_return ResultNull{};
                if (exists) {
                    co_await write_entry(*reinterpret_cast<BlobT*>(raw), String8{v.key}, String8{v.value});
                } else {
                    BlobT* b = alloc_blob<BlobT>();
                    co_await write_entry(*b, String8{v.key}, String8{v.value});
                    U64 ptr = reinterpret_cast<U64>(b);
                    co_await btree::insert(engine.index, key_hash, reinterpret_cast<const U8*>(&ptr), sizeof(U64));
                }
                co_return ResultSimpleStr{"OK"};
            } else if (SameAs<T, Del>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    U64 key_hash = hash(String8{v.keys[i]});
                    U64 raw = 0;
                    if (!co_await btree::find(engine.index, key_hash, reinterpret_cast<U8*>(&raw), sizeof(U64))) continue;
                    free_blob<BlobT>(raw);
                    co_await btree::remove(engine.index, key_hash);
                    count++;
                }
                co_return ResultInt{S64(count)};
            } else if (SameAs<T, Exists>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    U64 raw = 0;
                    if (co_await btree::find(engine.index, hash(String8{v.keys[i]}), reinterpret_cast<U8*>(&raw), sizeof(U64)))
                        count++;
                }
                co_return ResultInt{S64(count)};
            } else if (SameAs<T, Mget>) {
                DynamicArray<Optional<AutoString8>> vals;
                for (U64 i = 0; i < v.keys.length; i++) {
                    U64 raw = 0;
                    if (co_await btree::find(engine.index, hash(String8{v.keys[i]}), reinterpret_cast<U8*>(&raw), sizeof(U64)))
                        push_back(vals, Optional<AutoString8>{co_await read_value(*reinterpret_cast<BlobT*>(raw))});
                    else
                        push_back(vals, Optional<AutoString8>{});
                }
                co_return ResultNullBulkArr{move(vals)};
            } else if (SameAs<T, Mset>) {
                for (U64 i = 0; i < v.pairs.length; i++) {
                    U64 key_hash = hash(String8{v.pairs[i].first});
                    U64 raw = 0;
                    bool exists = co_await btree::find(engine.index, key_hash, reinterpret_cast<U8*>(&raw), sizeof(U64));
                    if (exists) {
                        co_await write_entry(*reinterpret_cast<BlobT*>(raw),
                            String8{v.pairs[i].first}, String8{v.pairs[i].second});
                    } else {
                        BlobT* b = alloc_blob<BlobT>();
                        co_await write_entry(*b, String8{v.pairs[i].first}, String8{v.pairs[i].second});
                        U64 ptr = reinterpret_cast<U64>(b);
                        co_await btree::insert(engine.index, key_hash, reinterpret_cast<const U8*>(&ptr), sizeof(U64));
                    }
                }
                co_return ResultSimpleStr{"OK"};
            } else if (SameAs<T, Keys>) {
                String8 pattern = v.pattern;
                DynamicArray<AutoString8> keys;
                auto it = co_await btree::tbegin<U64>(engine.index);
                auto end_it = btree::tend<U64>(engine.index);
                while (it != end_it) {
                    AutoString8 key = co_await read_key(*reinterpret_cast<BlobT*>(*it));
                    if (glob::match(String8{key}, pattern))
                        push_back(keys, move(key));
                    co_await it.advance();
                }
                co_return ResultBulkArr{move(keys)};
            } else if (SameAs<T, Scan>) {
                String8 pattern = v.match ? String8{*v.match} : String8{"*"};
                U64 count = v.count ? *v.count : 10;
                DynamicArray<AutoString8> result;

                // Cursor is the btree key (hash) of the first entry to process next
                auto it = v.cursor == 0
                    ? co_await btree::tbegin<U64>(engine.index)
                    : co_await btree::tfind_it<U64, btree::SearchStrategy::FirstGreaterEqual>(engine.index, v.cursor);
                auto end_it = btree::tend<U64>(engine.index);

                U64 scanned = 0;
                while (it != end_it && scanned < count) {
                    AutoString8 key = co_await read_key(*reinterpret_cast<BlobT*>(*it));
                    if (glob::match(String8{key}, pattern))
                        push_back(result, move(key));
                    scanned++;
                    co_await it.advance();
                }

                U64 next_cursor = 0;
                if (it != end_it) {
                    AutoString8 next_key = co_await read_key(*reinterpret_cast<BlobT*>(*it));
                    next_cursor = hash(String8{next_key});
                }
                co_return ResultScan{to_str(next_cursor), move(result)};
            } else if (SameAs<T, FlushDb> || SameAs<T, FlushAll>) {
                auto it = co_await btree::tbegin<U64>(engine.index);
                auto end_it = btree::tend<U64>(engine.index);
                while (it != end_it) {
                    free_blob<BlobT>(*it);
                    co_await it.advance();
                }
                co_await btree::truncate(engine.index);
                co_return ResultSimpleStr{"OK"};
            } else if (SameAs<T, DbSize>) {
                co_return ResultInt{S64(co_await btree::size(engine.index))};
            } else if (SameAs<T, TypeOf>) {
                U64 raw = 0;
                bool found = co_await btree::find(engine.index, hash(String8{v.key}), reinterpret_cast<U8*>(&raw), sizeof(U64));
                co_return ResultSimpleStr{found ? "string" : "none"};
            } else if (SameAs<T, Cmd>) {
                co_return ResultEmptyArr{};
            } else if (SameAs<T, SelectDb>) {
                if (v.index != 0) assert_not_implemented("multiple databases");
                co_return ResultSimpleStr{"OK"};
            } else if (SameAs<T, ClientGetName>) {
                co_return ResultNull{};
            } else if (SameAs<T, Client>) {
                co_return ResultSimpleStr{"OK"};
            } else if (SameAs<T, Info>) {
                U64 db_size = co_await btree::size(engine.index);
                co_return ResultBulkStr{
                    "# Server\r\nredis_version:7.0.0\r\narch_bits:64\r\n# Keyspace\r\ndb0:keys="_as
                    + to_str(db_size)
                    + ",expires=0\r\n"_as
                };
            } else if (SameAs<T, Unknown>) {
                co_return ResultError{"unknown command '"_as + v.name + "'"_as};
            } else {
                assert_true(false, "missing statement type in execute");
                co_return ResultError{"internal error"_as};
            }
        });
    }
}
