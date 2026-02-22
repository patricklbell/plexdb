export module objstore.engine;

import objstore.engine.dtype;
import objstore.engine.schema;
import objstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.pager;
import plexdb.blob;
import plexdb.btree;

using namespace plexdb;

export namespace objstore::engine {
    template<typename F>
    concept Flush = requires(F f, U64 length) {
        f(length);
    };
        
    struct Engine {
        Pager* pager;
        schema::Schema schema;

        Engine(Pager* in_pager);
    };

    void create_database(Pager& pager);

    struct ExecutionContext {
        const TArrayView<U8> write_buffer;
    };

    enum ExecutionResult {
        Success = 0,
        BadRequest,
        NotImplemented,
    };
    
    ExecutionResult execute(Engine& engine, const Statement& statement, const Flush auto& flush, const ExecutionContext& ctx) {
        auto write_str8_and_flush = [&flush,&ctx](String8 str) {
            assert_true(str.length < ctx.write_buffer.length, "string too large to fit in write buffer");
            os::memory_copy(ctx.write_buffer, TArrayView(reinterpret_cast<const U8*>(str.data), str.length));
        };
        U64 write_offset = 0;

        auto add_str8_and_flush_if_needed = [&flush,&ctx,&write_offset](const String8& str) {
            for (U64 str_offset = 0; str_offset < str.length;) {
                U64 length = min(ctx.write_buffer.length - write_offset, str.length - str_offset);

                os::memory_copy(
                    ctx.write_buffer.ptr + write_offset,
                    reinterpret_cast<const U8*>(str.data + str_offset),
                    length
                );
                write_offset += length;

                if (write_offset == ctx.write_buffer.length) {
                    flush(length);
                    write_offset = 0;
                }

                str_offset += length;
            }
        };

        auto finalize_flush = [&flush,&ctx,&write_offset]() {
            if (write_offset > 0) {
                flush(write_offset);
            }
        };

        // @todo switch on type index
        return visit(statement.value, [&](const auto& stmt) -> ExecutionResult {
            using T = RemoveCVRef<decltype(stmt)>;

            if constexpr (SameAs<T, CreateKeyspace>) {
                auto ks = schema::create_keyspace(engine.schema, stmt);
                if (ks == nullptr) {
                    return ExecutionResult::BadRequest;
                }
    
                return ExecutionResult::Success;
            } else if constexpr (SameAs<T, CreateTable>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return ExecutionResult::BadRequest;
                }
    
                auto tbl = schema::create_table(engine.schema, *ks, stmt);
                if (tbl == nullptr) {
                    return ExecutionResult::BadRequest;
                }
                
                return ExecutionResult::Success;
            } else if constexpr (SameAs<T, InsertInto>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return ExecutionResult::BadRequest;
                }
    
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return ExecutionResult::BadRequest;
                }
    
                if (tbl->cols.length != stmt.values.length) {
                    return ExecutionResult::BadRequest;
                }
    
                // @todo static blobs/in-tree for fixed column set
                U64 row_page = blob::create_paged_dynamic(*engine.pager);
                blob::BlobDynamicPaged row_blob(engine.pager, row_page);
    
                U64 row_offset_bytes = 0;
                auto write = [&row_offset_bytes,&row_blob](const U8* in_value, U64 size) {
                    blob::insert(row_blob, in_value, size, row_offset_bytes);
                    row_offset_bytes += size;
                };

                for (U64 idx = 0; idx < tbl->cols.length; idx++) {
                    const auto& col = tbl->cols[idx];
                    const auto& value = stmt.values[idx];
    
                    dtype::write(write, value, col.dtype);
                }
                
                // @todo uniqueness check
                tinsert(tbl->btree, dtype::hash(stmt.values[tbl->primary_col_idx]), row_page);
                
                return ExecutionResult::Success;
            } else if constexpr (SameAs<T, SelectFrom>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return ExecutionResult::BadRequest;
                }
    
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return ExecutionResult::BadRequest;
                }

                for (auto it = btree::tbegin<U64>(tbl->btree); it != btree::tend<U64>(tbl->btree); ++it) {
                    const U64& row_page = (*it);
                    blob::BlobDynamicPaged row_blob(engine.pager, row_page);

                    U64 row_offset_bytes = 0;
                    auto read = [&row_offset_bytes,&row_blob](U8* out_value, U64 size) {
                        blob::get(row_blob, out_value, size, row_offset_bytes);
                        row_offset_bytes += size;
                    };

                    add_str8_and_flush_if_needed("{");
                    for (U64 col_idx = 0; col_idx < tbl->cols.length; col_idx++) {
                        const auto& col = tbl->cols[col_idx];

                        auto value = dtype::read(read, col.dtype);
                        AutoString8 str = dtype::to_str(value, col.dtype);

                        add_str8_and_flush_if_needed(str);
                        if (col_idx != tbl->cols.length - 1) {
                            add_str8_and_flush_if_needed(",");
                        }
                    }
                    add_str8_and_flush_if_needed("}\n");
                }

                finalize_flush();
            } else {
                static_assert(false);
            }

            return ExecutionResult::Success;
        });
    }
}
