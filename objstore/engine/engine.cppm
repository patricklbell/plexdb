export module objstore.engine;

export import objstore.engine.dtype;
export import objstore.engine.schema;
import objstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.pager;
import plexdb.blob;
import plexdb.btree;

using namespace plexdb;

export namespace objstore::engine {
    struct Engine {
        Pager* pager;
        schema::Schema schema;

        Engine(Pager* in_pager);
    };

    void create_database(Pager& pager);

    enum class ExecutionStatus : U16 {
        Success         = 0x0000,
        ServerError     = 0x0001,  // Unexpected server-side error
        SyntaxError     = 0x2000,  // Query has syntax error
        Unauthorized    = 0x2100,  // No permission
        Invalid         = 0x2200,  // Syntactically correct but invalid
        ConfigError     = 0x2300,  // Configuration issue
        AlreadyExists   = 0x2400,  // Keyspace/table already exists
        NotImplemented  = 0x2500,  // Feature not implemented
    };

    constexpr String8 to_str(ExecutionStatus status) {
        switch (status) {
            case ExecutionStatus::Success:        return "SUCCESS";
            case ExecutionStatus::ServerError:    return "SERVER_ERROR";
            case ExecutionStatus::SyntaxError:    return "SYNTAX_ERROR";
            case ExecutionStatus::Unauthorized:   return "UNAUTHORIZED";
            case ExecutionStatus::Invalid:        return "INVALID";
            case ExecutionStatus::ConfigError:    return "CONFIG_ERROR";
            case ExecutionStatus::AlreadyExists:  return "ALREADY_EXISTS";
            case ExecutionStatus::NotImplemented: return "NOT_IMPLEMENTED";
        }
        return "UNKNOWN";
    }

    
    enum class ResultKind : U8 {
        Void = 0,       // No result (INSERT, UPDATE, DELETE)
        Rows,           // SELECT result
        SchemaChange,   // CREATE/DROP/ALTER result
    };

    struct ExecutionResult {
        ExecutionStatus status = ExecutionStatus::Success;
        ResultKind kind = ResultKind::Void;
        String8 message = "";
        String8 keyspace = "";
        String8 table = "";
    };

    template<typename F>
    concept OnValue = requires(F f, const schema::Table& tbl, const schema::Column& col, U64 col_count, U64 row_idx, U64 col_idx, const dtype::ReadValue& value) {
        {f(tbl, col, col_count, row_idx, col_idx, value)} -> SameAs<void>;
    };
    
    ExecutionResult execute(Engine& engine, const Statement& statement, const OnValue auto& on_value) {
        return visit(statement.value, [&](const auto& stmt) -> ExecutionResult {
            using T = RemoveCVRef<decltype(stmt)>;

            if constexpr (SameAs<T, CreateKeyspace>) {
                if (!stmt.if_not_exists) {
                    auto existing = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                    if (existing != nullptr) {
                        return {
                            .status = ExecutionStatus::AlreadyExists,
                            .message = "Keyspace already exists",
                            .keyspace = stmt.keyspace_name,
                        };
                    }
                }

                auto ks = schema::create_keyspace(engine.schema, stmt);
                if (ks == nullptr) {
                    return {.status = ExecutionStatus::ServerError, .message = "Failed to create keyspace"};
                }
    
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .message = "CREATED",
                    .keyspace = stmt.keyspace_name,
                };
            } else if constexpr (SameAs<T, CreateTable>) {
                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }

                if (!stmt.if_not_exists) {
                    auto existing = schema::read_table(engine.schema, *ks, stmt.table_name);
                    if (existing != nullptr) {
                        return {
                            .status = ExecutionStatus::AlreadyExists,
                            .message = "Table already exists",
                            .keyspace = stmt.keyspace_name,
                            .table = stmt.table_name,
                        };
                    }
                }
    
                auto tbl = schema::create_table(engine.schema, *ks, stmt);
                if (tbl == nullptr) {
                    return {.status = ExecutionStatus::ServerError, .message = "Failed to create table"};
                }
                
                return {
                    .status = ExecutionStatus::Success,
                    .kind = ResultKind::SchemaChange,
                    .message = "CREATED",
                    .keyspace = stmt.keyspace_name,
                    .table = stmt.table_name,
                };
            } else if constexpr (SameAs<T, InsertInto>) {
                // @todo implement these features
                assert_true(stmt.column_names.length == 0, "INSERT with column names not implemented");
                assert_true(stmt.timestamp == -1, "INSERT with USING TIMESTAMP not implemented");
                assert_true(stmt.ttl == -1, "INSERT with USING TTL not implemented");
                assert_true(!stmt.if_not_exists, "INSERT IF NOT EXISTS not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
    
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }
    
                if (tbl->cols.length != stmt.values.length) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Column count mismatch",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }

                for (U64 idx = 0; idx < tbl->cols.length; idx++) {
                    const auto& col = tbl->cols[idx];
                    const auto& value = stmt.values[idx];

                    if (!dtype::can_write(value, col.dtype)) {
                        return {
                            .status = ExecutionStatus::Invalid,
                            .message = "Value does not match its column's type",
                            .keyspace = stmt.keyspace_name,
                            .table = stmt.table_name,
                        };
                    }
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
                
                return {.status = ExecutionStatus::Success, .kind = ResultKind::Void};
            } else if constexpr (SameAs<T, SelectFrom>) {
                // @todo implement these features
                assert_true(stmt.column_names.cap == 0, "SELECT with column names not implemented");
                assert_true(stmt.where.cap == 0, "SELECT with WHERE clause not implemented");
                assert_true(stmt.limit == -1, "SELECT with LIMIT not implemented");

                auto ks = schema::read_keyspace(engine.schema, stmt.keyspace_name);
                if (ks == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Keyspace does not exist",
                        .keyspace = stmt.keyspace_name,
                    };
                }
    
                auto tbl = schema::read_table(engine.schema, *ks, stmt.table_name);
                if (tbl == nullptr) {
                    return {
                        .status = ExecutionStatus::Invalid,
                        .message = "Table does not exist",
                        .keyspace = stmt.keyspace_name,
                        .table = stmt.table_name,
                    };
                }


                U64 row_idx = 0;
                for (auto it = btree::tbegin<U64>(tbl->btree); it != btree::tend<U64>(tbl->btree); ++it) {
                    const U64& row_page = (*it);
                    blob::BlobDynamicPaged row_blob(engine.pager, row_page);

                    U64 row_offset_bytes = 0;
                    auto read = [&row_offset_bytes,&row_blob](U8* out_value, U64 size) {
                        blob::get(row_blob, out_value, size, row_offset_bytes);
                        row_offset_bytes += size;
                    };

                    for (U64 col_idx = 0; col_idx < tbl->cols.length; col_idx++) {
                        const auto& col = tbl->cols[col_idx];
                        const auto& value = dtype::read(read, col.dtype);

                        on_value(*tbl, col, tbl->cols.length, row_idx, col_idx, value);
                    }

                    row_idx++;
                }

                return {.status = ExecutionStatus::Success, .kind = ResultKind::Rows};
            } else if constexpr (SameAs<T, UseKeyspace>) {
                // @todo implement USE keyspace (set current keyspace context)
                return {.status = ExecutionStatus::NotImplemented, .message = "USE not implemented"};
            } else if constexpr (SameAs<T, AlterKeyspace>) {
                // @todo implement ALTER KEYSPACE
                return {.status = ExecutionStatus::NotImplemented, .message = "ALTER KEYSPACE not implemented"};
            } else if constexpr (SameAs<T, DropKeyspace>) {
                // @todo implement DROP KEYSPACE
                return {.status = ExecutionStatus::NotImplemented, .message = "DROP KEYSPACE not implemented"};
            } else if constexpr (SameAs<T, AlterTable>) {
                // @todo implement ALTER TABLE
                return {.status = ExecutionStatus::NotImplemented, .message = "ALTER TABLE not implemented"};
            } else if constexpr (SameAs<T, DropTable>) {
                // @todo implement DROP TABLE
                return {.status = ExecutionStatus::NotImplemented, .message = "DROP TABLE not implemented"};
            } else if constexpr (SameAs<T, TruncateTable>) {
                // @todo implement TRUNCATE TABLE
                return {.status = ExecutionStatus::NotImplemented, .message = "TRUNCATE not implemented"};
            } else if constexpr (SameAs<T, Update>) {
                // @todo implement UPDATE
                return {.status = ExecutionStatus::NotImplemented, .message = "UPDATE not implemented"};
            } else if constexpr (SameAs<T, Delete>) {
                // @todo implement DELETE
                return {.status = ExecutionStatus::NotImplemented, .message = "DELETE not implemented"};
            } else if constexpr (SameAs<T, Batch>) {
                // @todo implement BATCH
                return {.status = ExecutionStatus::NotImplemented, .message = "BATCH not implemented"};
            } else {
                static_assert(false, "Unhandled statement type in engine::execute");
            }
        });
    }
}
