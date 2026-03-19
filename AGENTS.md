## Code style
- Prefer `plexdb` library over STL.
- Prefer structs of POD. Only use classes for lifetime/resource management. Only make members private if strictly necessary.
- Use functions instead of class methods except for library compatibility.
- Wrap OS-specific includes and calls in platform macros from `macros.h`.
- Prefer single-expression idioms over ternaries or if-else. E.g. saturating subtract: `max(x, y) - y`.

## Commenting style
- Minimal comments. Do not explain what is obvious from the code.
- Use `@todo` for future work, `@note` for important assumptions, `@profile` for performance notes, `@padding` for struct padding.

## Dev environment
- C++20 modules with CMake. Static libraries: `plexdb` (core) and `objstore` (CQL object store).

## Testing
- Tests use [Catch2](https://github.com/catchorg/Catch2/blob/devel/docs/). Create tests for your changes.
- Test executables: `build/plexdb/plexdb_tests`, `build/objstore/objstore_tests`. Use `--skip-benchmarks` for normal runs.

## Logging & debugging test failures
- Build with `-DPLEXDB_LOG_ENABLED=ON` to enable the structured logging system.
- All log messages are routed to Catch2's `UNSCOPED_INFO` via the test log consumer (`test/log_consumer_helper.test.cpp`). They appear automatically in the output of any failing test case.
- The log system uses levels: `Trace`, `Debug`, `Info`, `Warn`, `Error` (see `plexdb::log::Level`).
- To add diagnostic logging, create a `plexdb::log::Producer` and call `plexdb::log::fire_message(producer.id, Level::Debug, text, len)`.
- For structured numeric metrics use `plexdb::log::fire_stat(producer.id, stat_id, value)` — no string formatting overhead.
- Register stat metadata with `plexdb::log::fire_stat_meta(producer.id, stat_id, "name")` so consumers can map stat IDs to human-readable names.
- The plugin system (`plexdb/log/log_abi.h`) allows custom consumers. Register via `plexdb_log_register_consumer` to filter or redirect logs. See `objstore/plugins/log_file/log_file_plugin.cpp` for a reference plugin.
- The stat plugin (`objstore/plugins/log_stat/log_stat_plugin.cpp`) writes stat events to a file. Graph them with `python extra/plot_stats.py plexdb.stats`.