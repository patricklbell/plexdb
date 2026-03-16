module docstore.engine;

namespace docstore::engine {
    Engine::Engine(Pager* in_pager) : pager(in_pager) {}

    void create_database(Pager& pager) {
        pager::set_root(pager, 0);
    }

    ExecutionResult execute(Engine& engine, const Statement& statement) {
        return ExecutionResult{
            .status = ExecutionStatus::NotImplemented,
            .kind = ResultKind::Void,
            .message = "not implemented",
        };
    }
}
