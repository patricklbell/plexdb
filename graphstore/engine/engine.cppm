export module graphstore.engine;

import graphstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.pager;

using namespace plexdb;

export namespace graphstore::engine {
    struct Engine {
        Pager* pager;
        String8 current_graph = "";

        Engine(Pager* in_pager);
    };

    void create_database(Pager& pager);

    enum class ExecutionStatus : U16 {
        Success        = 0x0000,
        ServerError    = 0x0001,
        SyntaxError    = 0x2000,
        Invalid        = 0x2200,
        AlreadyExists  = 0x2400,
        NotImplemented = 0x2500,
    };

    constexpr String8 to_str(ExecutionStatus status) {
        switch (status) {
            case ExecutionStatus::Success:        return "SUCCESS";
            case ExecutionStatus::ServerError:    return "SERVER_ERROR";
            case ExecutionStatus::SyntaxError:    return "SYNTAX_ERROR";
            case ExecutionStatus::Invalid:        return "INVALID";
            case ExecutionStatus::AlreadyExists:  return "ALREADY_EXISTS";
            case ExecutionStatus::NotImplemented: return "NOT_IMPLEMENTED";
        }
        return "UNKNOWN";
    }

    enum class ResultKind : U8 {
        Void = 0,
        Bindings,
        Graph,
        Boolean,
        SchemaChange,
    };

    struct ExecutionResult {
        ExecutionStatus status = ExecutionStatus::Success;
        ResultKind kind = ResultKind::Void;
        String8 message = "";
    };

    ExecutionResult execute(Engine& engine, const Statement& statement);
}
