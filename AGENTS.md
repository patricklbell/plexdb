## Code style
- Prefer `plexdb` library over STL.
- Prefer structs of POD. Only use classes for lifetime/resource management. Only make members private if strictly necessary.
    - Prefer struct designated initialization or default constructors rather than setting members as this is more fragile if the struct changes.
- Use functions instead of class methods except for library compatibility.
- Wrap OS-specific includes and calls in platform macros from `macros.h`.
- Prefer single-expression idioms over ternaries or if-else. E.g. saturating subtract: `max(x, y) - y`.

## Commenting style
- Minimal comments. Do not explain what is obvious from the code.
- Use `@todo` for future work, `@note` for important assumptions, `@profile` for performance notes, `@padding` for struct padding.
- Never reference specific tasks, fixes, callers, or changes in comments. No "added for X", "changed to Y", "round-trip:", "old/new path:", or any annotation that will rot as the code evolves.

## Coroutines
- Coroutine parameters: the C++20 coroutine frame copies reference parameters as references (not their referents). A `const T&` parameter whose referent is a temporary will dangle after the first suspension point. Pass by value for any parameter whose address is captured or reinterpreted inside the coroutine body (e.g. as a byte span), or whenever the coroutine may outlive the call site.

## Dev environment
- C++20 modules with CMake. Static libraries: `plexdb` (core) and `cql` (CQL object store).
- Module interface units (`.cppm`): export types and declare functions only — no function bodies except for templates (which must be defined in the interface for instantiation). Remember to add corresponding `.cpp` implementation units to CMakeLists.txt.
- Module implementation units (`.cpp`): define all non-template function bodies. Helper functions not declared in the interface must be `static` to give them internal linkage.

## Architecture

Module layers: `cql.parsers` → `cql.engine` (types, schema, key, iterator, statements, engine) → `cql.native`.

Non-obvious invariants:
- Tables with clustering keys use a two-level BTree: the partition BTree value points to a per-partition clustering BTree root (plus an optional static row page). Tables without clustering keys store the row blob page directly in the partition BTree value.
- Partition and clustering key bytes must be lexicographically comparable — any change to key serialization must preserve this.
- Frozen collection types (LIST, SET, MAP) are parsed and stored but treated identically to non-frozen counterparts; this is intentional.
- Row count from `btree::size` is incorrect for filtered or range queries; always count from actual iteration.

## Testing
- Tests use [Catch2](https://github.com/catchorg/Catch2/blob/devel/docs/). Create tests for your changes.
- Additional test-file conventions (applied to `*.test.cpp`) are in `.github/instructions/tests.instructions.md`.
- Test executables: `build/core/core_tests`, `build/cql/cql_tests`, `build/resp/resp_tests`. Use `--skip-benchmarks` for normal runs.
- **Server/client thread roles in integration tests:** Always run the server on the main test thread and the client on a background thread (`threads::launch`). Catch2's `CHECK`/`FAIL_CHECK` macros only work correctly on the main thread — a common failure pattern is an assertion firing inside the server, which would be silently lost if the server were on a background thread. The main thread also already has the scratch arena context equipped by `main.test.cpp`. Pattern: launch client thread → client calls `os::signal_notify_safe(interrupt)` when done → call `server::run(...)` on main thread (blocks until interrupt). See `resp/server/server.test.cpp` and `cql/native/native.test.cpp`.

## Logging & debugging test failures
- Build with `-DPLEXDB_ENABLE_PLUGINS=ON` to enable the structured logging system.
- All log messages are routed to Catch2's `UNSCOPED_INFO` via the test log consumer (`test/log_consumer_helper.test.cpp`). They appear automatically in the output of any failing test case.
- The log system uses levels: `Trace`, `Debug`, `Info`, `Warn`, `Error` (see `plexdb::plugin::Level`).
- Registration is lazy: `Producer` and `Stat` objects register themselves on first use, so construction order does not matter.
- To add diagnostic logging, create a `plexdb::plugin::Producer` and call `plexdb::plugin::message(producer, Level::Debug, str8)` where `str8` is a `String8`.
- For structured numeric metrics, create a `plexdb::plugin::Stat` with a `StatType` (`Counter` or `Gauge`) and call `plexdb::plugin::stat(s, value)` — no string formatting overhead. The stat's name and type are registered automatically on first fire.
- Stat types: `StatType::Counter` for monotonically increasing cumulative values, `StatType::Gauge` for point-in-time measurements. Default is `Gauge`.
- When a consumer registers, all known producers and stat metadata (including stat types) are replayed (catch-up), so the consumer always has a complete view.
- The plugin system (`plexdb/plugin/include/plexb/plugin_abi/plugin_abi.h/abi.h`) allows custom consumers. Register via `plexdb_plugin_register_consumer` to filter or redirect logs. See `plugins/log/log_plugin.cpp` for a reference plugin.
- The OTLP plugin (`plugins/otel/otel_plugin.cpp`) exports metrics via OpenTelemetry (OTLP/gRPC). Uses plaintext gRPC (`use_ssl_credentials = false`).
