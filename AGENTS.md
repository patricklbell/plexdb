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
- Build with `-DPLEXDB_ENABLE_LOGGING=ON` to enable the structured logging system.
- All log messages are routed to Catch2's `UNSCOPED_INFO` via the test log consumer (`test/log_consumer_helper.test.cpp`). They appear automatically in the output of any failing test case.
- The log system uses levels: `Trace`, `Debug`, `Info`, `Warn`, `Error` (see `plexdb::log::Level`).
- Registration is lazy: `Producer` and `Stat` objects register themselves on first use, so construction order does not matter.
- To add diagnostic logging, create a `plexdb::log::Producer` and call `plexdb::log::message(producer, Level::Debug, str8)` where `str8` is a `String8`.
- For structured numeric metrics, create a `plexdb::log::Stat` with a `StatType` (`Counter` or `Gauge`) and call `plexdb::log::stat(s, value)` — no string formatting overhead. The stat's name and type are registered automatically on first fire.
- Stat types: `StatType::Counter` for monotonically increasing cumulative values, `StatType::Gauge` for point-in-time measurements. Default is `Gauge`.
- When a consumer registers, all known producers and stat metadata (including stat types) are replayed (catch-up), so the consumer always has a complete view.
- The plugin system (`plexdb/log/log_abi.h`) allows custom consumers. Register via `plexdb_log_register_consumer` to filter or redirect logs. See `objstore/plugins/log_file/log_file_plugin.cpp` for a reference plugin.
- The OTLP plugin (`objstore/plugins/log_otel/log_otel_plugin.cpp`) exports metrics via OpenTelemetry (OTLP/gRPC). Built as a standalone project in `objstore/plugins/log_otel/`; load via `LD_PRELOAD`. Uses plaintext gRPC (`use_ssl_credentials = false`).