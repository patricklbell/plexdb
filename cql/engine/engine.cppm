module;
#include <coroutine>

export module cql.engine;

export import cql.engine.types;
export import cql.engine.schema;
export import cql.engine.schema.types;
export import cql.engine.system_schema;
export import cql.engine.virtual_table;
export import cql.engine.it;
import plexdb.pager;

import cql.engine.statements;
import cql.engine.io;
import cql.engine.evaluator;

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.blob;
import plexdb.btree;
import plexdb.aio;

using namespace plexdb;

export namespace cql::engine {
    // ========================================================================
    // prepared statement cache
    // ========================================================================
    struct BindVariableSpec {
        AutoString8 name;
        type::Type  type;
    };

    struct PreparedEntry {
        AutoString8                    query_string;
        AutoString8                    keyspace;
        AutoString8                    table;
        DynamicArray<BindVariableSpec> bind_variables;
        S32                            pk_index       = -1;
        U64                            schema_version = 0;
    };

    // @todo determine what the correct strategy is here
    constexpr U64 MAX_PREPARED_STATEMENTS = 1024;

    // ========================================================================
    // virtual (system/system_schema) table cache — rebuilt only when schema.version changes
    // ========================================================================
    struct VirtualTableCacheEntry {
        U64         schema_version = MAX_U64; // MAX_U64 means never populated
        VirtualRows rows;
    };
    constexpr U64 VIRTUAL_TABLE_CACHE_SIZE = 13;

    // ========================================================================
    // engine
    // ========================================================================
    struct Engine {
        Pager*         pager = nullptr;
        schema::Schema schema;
        AutoString8    current_keyspace{""};
        bool           single_node = true;
        U16            port        = 9042;

        MapFixedSentinel<U64, PreparedEntry, MAX_PREPARED_STATEMENTS> prepared_cache;
        VirtualTableCacheEntry                                        virtual_table_cache[VIRTUAL_TABLE_CACHE_SIZE];
    };

    coroutine::Task<>     init(Engine& engine, Pager* in_pager);
    coroutine::Task<void> create_database(Pager& pager);

    enum class ExecutionStatus : U16 {
        Success       = 0x0000,
        ServerError   = 0x0001, // Unexpected server-side error
        SyntaxError   = 0x2000, // Query has syntax error
        Unauthorized  = 0x2100, // No permission
        Invalid       = 0x2200, // Syntactically correct but invalid
        ConfigError   = 0x2300, // Configuration issue
        AlreadyExists = 0x2400, // Keyspace/table already exists
    };

    constexpr String8 to_str(ExecutionStatus status) {
        switch (status) {
            case ExecutionStatus::Success:
                return "SUCCESS";
            case ExecutionStatus::ServerError:
                return "SERVER_ERROR";
            case ExecutionStatus::SyntaxError:
                return "SYNTAX_ERROR";
            case ExecutionStatus::Unauthorized:
                return "UNAUTHORIZED";
            case ExecutionStatus::Invalid:
                return "INVALID";
            case ExecutionStatus::ConfigError:
                return "CONFIG_ERROR";
            case ExecutionStatus::AlreadyExists:
                return "ALREADY_EXISTS";
        }
        return "UNKNOWN";
    }

    enum class ResultKind : U8 {
        Void = 0,     // No result (INSERT, UPDATE, DELETE)
        Rows,         // SELECT result
        VirtualRows,  // Virtual/system table result (not backed by storage)
        SchemaChange, // CREATE/DROP/ALTER result
        UseKeyspace,  // USE keyspace
    };

    // ========================================================================
    // execution result
    // ========================================================================
    struct ExecutionResult {
        ExecutionStatus status          = ExecutionStatus::Success;
        ResultKind      kind            = ResultKind::Void;
        String8         message         = "";
        AutoString8     message_storage = {}; // @note owns message string when dynamic (message.data points here)
        AutoString8     keyspace        = {};
        AutoString8     table           = {};

        U64                   row_limit_count = MAX_U64;
        bool                  is_distinct     = false;
        Optional<RowRange>    rows            = {};
        Optional<VirtualRows> virtual_rows    = {};

        const schema::Table* resolved_table     = nullptr;
        DynamicArray<U64>    select_col_indices = {};
        // Parallel to select_col_indices when populated; an alias overrides the
        // schema column name in the result frame's column spec. Empty Optional
        // means "no alias — use the underlying column name".
        DynamicArray<Optional<AutoString8>> select_col_aliases = {};

        DynamicArray<cql::WhereClause::Relation> filter_predicates = {};
        cql::EvalContext                         filter_ctx        = {};

        // For SELECT results: holds the open execute-transaction so that row iteration
        // (which uses the own_tx borrowing pattern) always sees a stable active transaction
        // that cannot be committed by another connection mid-iteration.
        // The caller must co_await deferred_tx.commit() after consuming all rows.
        // If not explicitly committed, the destructor rolls back (borrowing is safe for reads).
        pager::Transaction deferred_tx = {};
    };

    coroutine::Task<ExecutionResult> execute(Engine& engine, const Statement& statement);
    coroutine::Task<ExecutionResult> execute(Engine& engine, Statement& statement, DynamicArray<Term>&& bound_values);
    coroutine::Task<ExecutionResult> execute(Engine& engine, U64 prepared_id, DynamicArray<Term>&& bound_values);

    // Substitute positional bind markers in `statement` with values from `bound_values`.
    void bind_values_to_statement(Statement& statement, DynamicArray<Term>& bound_values);

    // Per-connection-keyspace overloads: use when multiple connections share an engine
    coroutine::Task<ExecutionResult> execute(Engine& engine, const Statement& statement, AutoString8& current_keyspace);
    coroutine::Task<ExecutionResult> execute(Engine& engine, Statement& statement, DynamicArray<Term>&& bound_values, AutoString8& current_keyspace);
    coroutine::Task<ExecutionResult> execute(Engine& engine, U64 prepared_id, DynamicArray<Term>&& bound_values, AutoString8& current_keyspace);

    // ========================================================================
    // bind variables
    // ========================================================================
    DynamicArray<BindVariableSpec> collect_bind_variables(Engine& engine, const Statement& statement);
    DynamicArray<BindVariableSpec> collect_bind_variables(Engine& engine, const Statement& statement, String8 current_keyspace);

    // ========================================================================
    // prepared statements
    // ========================================================================
    struct PrepareResult {
        ExecutionStatus status          = ExecutionStatus::Success;
        String8         message         = "";
        AutoString8     message_storage = {}; // @note owns message string when dynamic (message.data points here)
        U64             id              = 0;
        PreparedEntry*  entry           = nullptr;
    };

    PrepareResult  prepare(Engine& engine, String8 query, String8 current_keyspace);
    PreparedEntry* try_get_prepared(Engine& engine, U64 prepared_id);
}

export namespace cql {
    using Engine = engine::Engine;
}
