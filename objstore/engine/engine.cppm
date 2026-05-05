export module objstore.engine;

export import objstore.engine.types;
export import objstore.engine.schema;
export import objstore.engine.system_schema;
export import objstore.engine.virtual_table;
export import objstore.engine.it;

import objstore.engine.statements;
import objstore.engine.io;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.pager;
import plexdb.blob;
import plexdb.btree;

using namespace plexdb;

export namespace objstore::engine {
    // ========================================================================
    // prepared statement cache
    // ========================================================================
    struct BindVariableSpec {
        AutoString8 name;
        Type type;
    };

    struct PreparedEntry {
        AutoString8 query_string;
        AutoString8 keyspace;
        AutoString8 table;
        DynamicArray<BindVariableSpec> bind_variables;
        S32 pk_index = -1;
    };

    // @todo determine what the correct strategy is here
    constexpr U64 MAX_PREPARED_STATEMENTS = 1024;

    // ========================================================================
    // engine
    // ========================================================================
    struct Engine {
        Pager* pager;
        schema::Schema schema;
        AutoString8 current_keyspace{""};
        MapFixedSentinel<U64, PreparedEntry, MAX_PREPARED_STATEMENTS> prepared_cache;

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
        }
        return "UNKNOWN";
    }


    enum class ResultKind : U8 {
        Void = 0,       // No result (INSERT, UPDATE, DELETE)
        Rows,           // SELECT result
        VirtualRows,    // Virtual/system table result (not backed by storage)
        SchemaChange,   // CREATE/DROP/ALTER result
        UseKeyspace,    // USE keyspace
    };

    // ========================================================================
    // execution result
    // ========================================================================
    struct ExecutionResult {
        ExecutionStatus status = ExecutionStatus::Success;
        ResultKind kind = ResultKind::Void;
        String8 message = "";
        String8 keyspace = "";
        String8 table = "";

        U64 row_limit_count = MAX_U64;
        Optional<RowRange> rows = {};
        Optional<VirtualRows> virtual_rows = {};

        const schema::Table* resolved_table = nullptr;
        DynamicArray<U64> select_col_indices = {};
    };

    ExecutionResult execute(Engine& engine, const Statement& statement);
    ExecutionResult execute(Engine& engine, Statement& statement, DynamicArray<Constant>&& bound_values);
    ExecutionResult execute(Engine& engine, U64 prepared_id, DynamicArray<Constant>&& bound_values);

    // ========================================================================
    // bind variables
    // ========================================================================
    DynamicArray<BindVariableSpec> collect_bind_variables(Engine& engine, const Statement& statement);

    // ========================================================================
    // prepared statements
    // ========================================================================
    struct PrepareResult {
        ExecutionStatus status = ExecutionStatus::Success;
        String8 message = "";
        U64 id = 0;
        PreparedEntry* entry = nullptr;
    };

    PrepareResult prepare(Engine& engine, String8 query);
    PreparedEntry* try_get_prepared(Engine& engine, U64 prepared_id);
}
