module;
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

export module keyvalue.engine;

export import keyvalue.engine.statements;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.btree;
import plexdb.btree.types;
import plexdb.blob;
import plexdb.pager;
import plexdb.coroutine;
import plexdb.tagged_union;

import keyvalue.glob;

using namespace plexdb;

// ============================================================================
// internal: blob layout [key_len:U64][key bytes][value_len:U64][value bytes]
// ============================================================================
namespace keyvalue::engine {
    template<typename Blob>
    coroutine::Task<void> write_blob(Blob& b, String8 key, String8 value) {
        U64 total = sizeof(U64) * 2 + key.length + value.length;
        co_await plexdb::blob::resize(b, total);
        U64 off = 0;
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(&key.length), sizeof(U64), off);
        off += sizeof(U64);
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(key.data), key.length, off);
        off += key.length;
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(&value.length), sizeof(U64), off);
        off += sizeof(U64);
        co_await plexdb::blob::update(b, reinterpret_cast<const U8*>(value.data), value.length, off);
    }

    template<typename Blob>
    coroutine::Task<AutoString8> blob_read_key(Blob& b) {
        U64 key_len = 0;
        co_await plexdb::blob::get(b, reinterpret_cast<U8*>(&key_len), sizeof(U64), 0);
        DynamicArray<U8> buf;
        resize(buf, key_len);
        co_await plexdb::blob::get(b, buf.ptr, key_len, sizeof(U64));
        co_return AutoString8{
            String8{reinterpret_cast<const char*>(buf.ptr), key_len}
        };
    }

    template<typename Blob>
    coroutine::Task<AutoString8> blob_read_value(Blob& b) {
        U64 key_len = 0, value_len = 0;
        U64 off = 0;
        co_await plexdb::blob::get(b, reinterpret_cast<U8*>(&key_len), sizeof(U64), off);
        off += sizeof(U64) + key_len;
        co_await plexdb::blob::get(b, reinterpret_cast<U8*>(&value_len), sizeof(U64), off);
        off += sizeof(U64);
        DynamicArray<U8> buf;
        resize(buf, value_len);
        co_await plexdb::blob::get(b, buf.ptr, value_len, off);
        co_return AutoString8{
            String8{reinterpret_cast<const char*>(buf.ptr), value_len}
        };
    }
}

// ============================================================================
// exported types and engine api
// ============================================================================
export namespace keyvalue::engine {
    enum class ExecutionStatus : U8 {
        Ok,
        NotFound,
        Error,
        Close
    };
    enum class ResultKind : U8 {
        None,
        SimpleStr,
        BulkStr,
        Int,
        Arr,
        NullArr,
        Scan
    };

    struct ExecutionResult {
        ExecutionStatus                     status = ExecutionStatus::Ok;
        ResultKind                          kind   = ResultKind::None;
        AutoString8                         str;
        S64                                 integer = 0;
        DynamicArray<AutoString8>           arr;
        DynamicArray<Optional<AutoString8>> null_arr;
        AutoString8                         cursor;
        AutoString8                         error_message;
    };

    struct InMemoryEngine {
        btree::BTreeInMemory<btree::FixedKeyPolicy<U64>, btree::FixedValuePolicy<sizeof(U64)>> index;
        DynamicMap<U64, blob::BlobInMemory>                                                    blobs;
        U64                                                                                    next_blob_id = 0;

        InMemoryEngine()
            : index{btree::FixedKeyPolicy<U64>{}, btree::FixedValuePolicy<sizeof(U64)>{}, 544u} {
        }
    };

    struct PagedEngine {
        btree::BTreePaged<btree::FixedKeyPolicy<U64>, btree::FixedValuePolicy<sizeof(U64)>> index;
        Pager*                                                                              pager = nullptr;
    };

    template<typename E>
    concept Engine = Either<E, InMemoryEngine, PagedEngine>;

    coroutine::Task<AutoString8> read_key(Engine auto& engine, U64 id);

    coroutine::Task<AutoString8> read_value(Engine auto& engine, U64 id);

    coroutine::Task<void> write_entry(Engine auto& engine, U64 id, String8 key, String8 value);

    coroutine::Task<U64> alloc_blob(Engine auto& engine);

    coroutine::Task<void> free_blob(Engine auto& engine, U64 id);

    coroutine::Task<ExecutionResult> execute(Engine auto& engine, const Statement& statement);
}

namespace keyvalue::engine {
    ExecutionResult create_not_found() {
        ExecutionResult r;
        r.status = ExecutionStatus::NotFound;
        return r;
    }
    ExecutionResult create_close() {
        ExecutionResult r;
        r.status = ExecutionStatus::Close;
        return r;
    }
    ExecutionResult create_error(AutoString8 msg) {
        ExecutionResult r;
        r.status        = ExecutionStatus::Error;
        r.error_message = move(msg);
        return r;
    }
    ExecutionResult create_simple(String8 s) {
        ExecutionResult r;
        r.kind = ResultKind::SimpleStr;
        r.str  = s;
        return r;
    }
    ExecutionResult create_bulk(AutoString8 s) {
        ExecutionResult r;
        r.kind = ResultKind::BulkStr;
        r.str  = move(s);
        return r;
    }
    ExecutionResult create_int(S64 n) {
        ExecutionResult r;
        r.kind    = ResultKind::Int;
        r.integer = n;
        return r;
    }
    ExecutionResult create_arr(DynamicArray<AutoString8> a) {
        ExecutionResult r;
        r.kind = ResultKind::Arr;
        r.arr  = move(a);
        return r;
    }
    ExecutionResult create_null_arr(DynamicArray<Optional<AutoString8>> a) {
        ExecutionResult r;
        r.kind     = ResultKind::NullArr;
        r.null_arr = move(a);
        return r;
    }
    ExecutionResult create_scan(AutoString8 cursor, DynamicArray<AutoString8> keys) {
        ExecutionResult r;
        r.kind   = ResultKind::Scan;
        r.cursor = move(cursor);
        r.arr    = move(keys);
        return r;
    }
}

// ============================================================================
// engine blob access helpers
// ============================================================================
export namespace keyvalue::engine {
    coroutine::Task<AutoString8> read_key(Engine auto& engine, U64 id) {
        if constexpr (SameAs<Decay<decltype(engine)>, InMemoryEngine>) {
            co_return co_await blob_read_key(*find(engine.blobs, id));
        } else {
            blob::BlobDynamicPaged b;
            co_await blob::load(b, engine.pager, id);
            co_return co_await blob_read_key(b);
        }
    }

    coroutine::Task<AutoString8> read_value(Engine auto& engine, U64 id) {
        if constexpr (SameAs<Decay<decltype(engine)>, InMemoryEngine>) {
            co_return co_await blob_read_value(*find(engine.blobs, id));
        } else {
            blob::BlobDynamicPaged b;
            co_await blob::load(b, engine.pager, id);
            co_return co_await blob_read_value(b);
        }
    }

    coroutine::Task<void> write_entry(Engine auto& engine, U64 id, String8 key, String8 value) {
        if constexpr (SameAs<Decay<decltype(engine)>, InMemoryEngine>) {
            co_await write_blob(*find(engine.blobs, id), key, value);
        } else {
            blob::BlobDynamicPaged b;
            co_await blob::load(b, engine.pager, id);
            co_await write_blob(b, key, value);
        }
    }

    coroutine::Task<U64> alloc_blob(Engine auto& engine) {
        if constexpr (SameAs<Decay<decltype(engine)>, InMemoryEngine>) {
            U64 id = engine.next_blob_id++;
            insert(engine.blobs, id, blob::BlobInMemory{});
            co_return id;
        } else {
            co_return co_await blob::create_paged_dynamic(*engine.pager);
        }
    }

    coroutine::Task<void> free_blob(Engine auto& engine, U64 id) {
        if constexpr (SameAs<Decay<decltype(engine)>, InMemoryEngine>) {
            remove(engine.blobs, id);
            co_return;
        } else {
            blob::BlobDynamicPaged b;
            co_await blob::load(b, engine.pager, id);
            co_await blob::remove(b);
        }
    }
}

// ============================================================================
// execute
// ============================================================================
namespace keyvalue::engine {
    coroutine::Task<ExecutionResult> execute_inside_transaction(Engine auto& engine, const Statement& statement) {
        co_return co_await visit(statement.value, [&](const auto& v) -> coroutine::Task<ExecutionResult> {
            using T = Decay<decltype(v)>;

            if constexpr (SameAs<T, Ping>) {
                if (v.message) {
                    co_return create_bulk(AutoString8{*v.message});
                }
                co_return create_simple("PONG");
            } else if constexpr (SameAs<T, Quit>) {
                co_return create_close();
            } else if constexpr (SameAs<T, Get>) {
                auto id_opt = co_await btree::tfind<U64>(engine.index, hash(String8{v.key}));
                if (!id_opt) {
                    co_return create_not_found();
                }
                co_return create_bulk(co_await read_value(engine, *id_opt));
            } else if constexpr (SameAs<T, Set>) {
                U64  key_hash = hash(String8{v.key});
                auto id_opt   = co_await btree::tfind<U64>(engine.index, key_hash);
                if (v.nx && id_opt) {
                    co_return create_not_found();
                }
                if (v.xx && !id_opt) {
                    co_return create_not_found();
                }
                if (id_opt) {
                    co_await write_entry(engine, *id_opt, String8{v.key}, String8{v.value});
                } else {
                    U64 id = co_await alloc_blob(engine);
                    co_await write_entry(engine, id, String8{v.key}, String8{v.value});
                    co_await btree::tinsert(engine.index, key_hash, id);
                }
                co_return create_simple("OK");
            } else if constexpr (SameAs<T, Del>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    U64  key_hash = hash(String8{v.keys[i]});
                    auto id_opt   = co_await btree::tfind<U64>(engine.index, key_hash);
                    if (!id_opt) {
                        continue;
                    }
                    co_await free_blob(engine, *id_opt);
                    co_await btree::remove(engine.index, key_hash);
                    count++;
                }
                co_return create_int(S64(count));
            } else if constexpr (SameAs<T, Exists>) {
                U64 count = 0;
                for (U64 i = 0; i < v.keys.length; i++) {
                    if (co_await btree::tfind<U64>(engine.index, hash(String8{v.keys[i]}))) {
                        count++;
                    }
                }
                co_return create_int(S64(count));
            } else if constexpr (SameAs<T, Mget>) {
                DynamicArray<Optional<AutoString8>> vals;
                for (U64 i = 0; i < v.keys.length; i++) {
                    auto id_opt = co_await btree::tfind<U64>(engine.index, hash(String8{v.keys[i]}));
                    if (id_opt) {
                        push_back(vals, Optional<AutoString8>{co_await read_value(engine, *id_opt)});
                    } else {
                        push_back(vals, Optional<AutoString8>{});
                    }
                }
                co_return create_null_arr(move(vals));
            } else if constexpr (SameAs<T, Mset>) {
                for (U64 i = 0; i < v.pairs.length; i++) {
                    U64  key_hash = hash(String8{v.pairs[i].first});
                    auto id_opt   = co_await btree::tfind<U64>(engine.index, key_hash);
                    if (id_opt) {
                        co_await write_entry(engine, *id_opt, String8{v.pairs[i].first}, String8{v.pairs[i].second});
                    } else {
                        U64 id = co_await alloc_blob(engine);
                        co_await write_entry(engine, id, String8{v.pairs[i].first}, String8{v.pairs[i].second});
                        co_await btree::tinsert(engine.index, key_hash, id);
                    }
                }
                co_return create_simple("OK");
            } else if constexpr (SameAs<T, Keys>) {
                String8                   pattern = v.pattern;
                DynamicArray<AutoString8> keys;
                auto                      it     = co_await btree::begin<U64>(engine.index);
                auto                      end_it = btree::end<U64>(engine.index);
                while (it != end_it) {
                    AutoString8 key = co_await read_key(engine, *it);
                    if (glob::match(String8{key}, pattern)) {
                        push_back(keys, move(key));
                    }
                    co_await it.advance();
                }
                co_return create_arr(move(keys));
            } else if constexpr (SameAs<T, Scan>) {
                String8                   pattern = v.match ? String8{*v.match} : String8{"*"};
                U64                       count   = v.count ? *v.count : 10;
                DynamicArray<AutoString8> result;

                auto it     = v.cursor == 0
                                ? co_await btree::begin<U64>(engine.index)
                                : co_await btree::find_it<U64, btree::SearchStrategy::FirstGreaterEqual>(engine.index, v.cursor);
                auto end_it = btree::end<U64>(engine.index);

                U64 scanned = 0;
                while (it != end_it && scanned < count) {
                    AutoString8 key = co_await read_key(engine, *it);
                    if (glob::match(String8{key}, pattern)) {
                        push_back(result, move(key));
                    }
                    scanned++;
                    co_await it.advance();
                }

                U64 next_cursor = 0;
                if (it != end_it) {
                    AutoString8 next_key = co_await read_key(engine, *it);
                    next_cursor          = hash(String8{next_key});
                }
                co_return create_scan(to_str(next_cursor), move(result));
            } else if constexpr (SameAs<T, FlushDb> || SameAs<T, FlushAll>) {
                if constexpr (SameAs<Decay<decltype(engine)>, InMemoryEngine>) {
                    clear(engine.blobs);
                } else {
                    auto it     = co_await btree::begin<U64>(engine.index);
                    auto end_it = btree::end<U64>(engine.index);
                    while (it != end_it) {
                        U64 id = *it;
                        co_await it.advance();
                        co_await free_blob(engine, id);
                    }
                }
                co_await btree::truncate(engine.index);
                co_return create_simple("OK");
            } else if constexpr (SameAs<T, DbSize>) {
                co_return create_int(S64(co_await btree::size(engine.index)));
            } else if constexpr (SameAs<T, TypeOf>) {
                bool found = static_cast<bool>(co_await btree::tfind<U64>(engine.index, hash(String8{v.key})));
                co_return create_simple(found ? "string" : "none");
            } else if constexpr (SameAs<T, Cmd>) {
                co_return create_arr({});
            } else if constexpr (SameAs<T, SelectDb>) {
                if (v.index != 0) {
                    assert_not_implemented("multiple databases");
                }
                co_return create_simple("OK");
            } else if constexpr (SameAs<T, ClientGetName>) {
                co_return create_not_found();
            } else if constexpr (SameAs<T, Client>) {
                co_return create_simple("OK");
            } else if constexpr (SameAs<T, Info>) {
                U64 db_size = co_await btree::size(engine.index);
                co_return create_bulk(
                    "# Server\r\nredis_version:7.0.0\r\narch_bits:64\r\n# Keyspace\r\ndb0:keys="_as + to_str(db_size) + ",expires=0\r\n"_as
                );
            } else if constexpr (SameAs<T, Unknown>) {
                co_return create_error("unknown command '"_as + v.name + "'"_as);
            } else {
                assert_true(false, "missing statement type in execute");
                co_return create_error("internal error"_as);
            }
        });
    }

    export template<Engine E>
    coroutine::Task<ExecutionResult> execute(E& engine, const Statement& statement) {
        ZoneScopedN("engine::execute");
        if constexpr (SameAs<E, PagedEngine>) {
            pager::Transaction tx{&engine.pager};

            co_await tx.begin();
            auto result = co_await execute_inside_transaction(engine, statement);
            co_await tx.commit();

            co_return result;
        } else {
            auto result = co_await execute_inside_transaction(engine, statement);
            co_return result;
        }
    }
}

export namespace keyvalue {
    template<typename E>
    concept Engine = engine::Engine<E>;
}
