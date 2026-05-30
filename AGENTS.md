## Code style
- Prefer `plexdb` library over STL.
- Prefer structs of POD. Only use classes for lifetime/resource management. Only make members private if strictly necessary.
- Use functions instead of class methods except for library compatibility.
- Wrap OS-specific includes and calls in platform macros from `macros.h`.
- Prefer single-expression idioms over ternaries or if-else. E.g. saturating subtract: `max(x, y) - y`.

## Commenting style
- Minimal comments. Do not explain what is obvious from the code.
- Use `@todo` for future work, `@note` for important assumptions, `@profile` for performance notes, `@padding` for struct padding.
- Never reference specific tasks, fixes, callers, or changes in comments. No "added for X", "changed to Y", "round-trip:", "old/new path:", or any annotation that will rot as the code evolves.

## Dev environment
- C++20 modules with CMake. Static libraries: `plexdb` (core) and `cql` (CQL object store).
- Module interface units (`.cppm`): export types and declare functions only — no function bodies except for templates (which must be defined in the interface for instantiation). Remember to add corresponding `.cpp` implementation units to CMakeLists.txt.
- Module implementation units (`.cpp`): define all non-template function bodies. Helper functions not declared in the interface must be `static` to give them internal linkage.

## Schema (cql.engine.schema)
- `schema::Column` has `key_kind` (`KeyKind::None`, `PartitionKey`, `ClusteringKey`), `key_position` (0-based ordering within that key), and `is_static: bool`.
- `schema::Table` has `partition_key_col_indices`, `clustering_key_col_indices`, and `static_col_indices`, all `DynamicArray<U64>` sorted by `key_position`. `has_clustering_keys()` returns `clustering_key_col_indices.length > 0`.
- `schema::PartitionEntry` is a `#pragma pack(push,1)` struct `{ U64 data_page; U64 static_page; }` (16 bytes). `data_page` is the row blob page (non-clustering) or clustering BTree root page; `static_page` is 0 when no static row exists for that partition.
- `schema::PartitionBTree = BTreePaged<VarlenKeyPolicy<>, FixedValuePolicy<sizeof(PartitionEntry)>>`. The btree key is serialized partition key bytes; the value is a `PartitionEntry`.
- `schema::ClusteringBTree = BTreePaged<VarlenKeyPolicy<>, FixedValuePolicy<sizeof(U64)>>`. Used as the inner per-partition BTree for tables with clustering keys; value is the row blob page.
- `schema::Table.btree` is the per-table `PartitionBTree`.
- Static column constraints (enforced in `create_column`): table must have at least one clustering column; partition key columns cannot be static.

## Types (cql.engine.types)
- Collection type structs `List`, `Set`, `Map`, `Vector` each carry a `frozen` bool field. Frozen types are parsed and stored but currently treated permissively — they behave identically to their non-frozen counterparts.
- `operator==` on `Type` compares `frozen` flags for collection types.

## Key serialization (cql.engine.key)
- Partition and clustering key bytes must be lexicographically comparable byte strings.
- **Integers**: XOR the sign bit, then write big-endian. Maps `[INT_MIN, INT_MAX]` → `[0, UINT_MAX]` monotonically.
- **Floats**: for positive values flip the sign bit; for negative values flip all bits.
- **Text/blob** (single-column key): written as raw bytes with no length prefix. Lexicographic order matches string order.
- **Composite keys**: variable-length components use escaped+terminated encoding (0x00 bytes escaped as `0x00 0xFF`; component terminated with `0x00 0x00`). Fixed-width components are prefixed with a 2-byte big-endian length (constant, so ordering is unaffected).
- Functions: `serialize_partition_single` / `serialize_partition` (multiple columns) and `serialize_clustering_single` / `serialize_clustering`.

## Row/column iterator architecture (cql.engine.it)
- `PartitionBTree` and `ClusteringBTree` are re-exported from `cql.engine.schema` (see Schema section for definitions).
- `RowIterator` is a **two-level** iterator:
  - `partition_it: btree::Iterator<PartitionBTree, schema::PartitionEntry>` — outer level over serialized partition key bytes.
  - `clustering_btree: ClusteringBTree` — stored by value inside `RowIterator`. `clustering_it` and `clustering_end_it` point into `&clustering_btree`. Move/copy constructors call `fix_clustering_btree_ptr` to keep these pointers valid after object moves.
  - `clustering_it` / `clustering_end_it: btree::Iterator<ClusteringBTree, U64>` — inner level over serialized clustering key bytes within one partition. Only valid when `table->has_clustering_keys()`.
- `deref()` → `coroutine::Task<ColumnRange>`. Reads `row_page` from `clustering_it` (clustering tables) or `partition_entry.data_page` (non-clustering tables). Passes `partition_entry.static_page` to `load()` so the `ColumnIterator` can load the static blob.
- `advance()` → `coroutine::Task<void>`. For clustering tables: advances `clustering_it`; when exhausted advances `partition_it` and opens a new `clustering_btree` for the next partition.
- `RowRange` holds `RowIterator start` and `RowIterator stop`. Stop iterators produced by `_lt_it` / `_le_it` do not initialize clustering iterators (they are compared using only `partition_it`).
- `ColumnRange` holds two `ColumnIterator`s (`start`, `stop`). `ColumnIterator` reads column values in schema order, returning `Null` for inactive columns. It pre-fetches all bytes on construction so `operator*` and `operator++` are synchronous.
- **`ColumnIterator` two-buffer design**: `row_data` holds the regular blob (non-static columns); `static_row_data` holds the static blob (only allocated when `static_page_idx != 0`). `operator*` / `operator++` dispatch between the two buffers based on `col.is_static`. No third merge buffer is allocated.
- Row blob layout (both regular and static): `[column_count: U64][mask words: U64 × ⌈column_count/64⌉][column data...]`. In the regular blob, mask bits for static columns are always 0 (and vice versa for the static blob). Byte offset into data = `COLUMN_COUNT_BYTE_COUNT + ceil_div(row_column_count, MASK_BIT_COUNT) * MASK_BYTE_COUNT`.
- **Consumer pattern:**
  ```cpp
  RowIterator& row_it = result.rows->start;
  RowIterator& row_end = result.rows->stop;
  while (row_it != row_end) {
      ColumnRange col_range = co_await row_it.deref();
      for (U64 ci = 0; ci < tbl->cols.length && col_range.start != col_range.stop; ci++, ++col_range.start) {
          use(*col_range.start);
      }
      co_await row_it.advance();
  }
  ```
- `btree::SearchStrategy` values: `RequireEquality`, `FirstGreater`, `FirstGreaterEqual`, `LastLess`, `LastLessEqual`. The `*_gt_it` and `*_le_it` factory functions both map to `FirstGreater`; `*_ge_it` and `*_lt_it` both map to `FirstGreaterEqual` — they differ only in whether they are used as start or stop iterators.
- `ExecutionResult` carries `resolved_table` (schema pointer) and `select_col_indices` (empty = all columns).

## CQL native protocol (cql.native)
- Protocol v4 frame: 9-byte header `[version, flags, stream(2), opcode, body_len(4)]` + body. Response version byte is `0x84`.
- Null column values encode as `[bytes]` with length `-1` (big-endian S32).
- `append_result_rows` backpatches the row count: write placeholder `0` at a recorded position, iterate rows, then overwrite the 4 bytes with the actual count.
- Row count from `btree::size` is wrong for filtered/range queries; always use the actual iteration count.

## Testing
- Tests use [Catch2](https://github.com/catchorg/Catch2/blob/devel/docs/). Create tests for your changes.
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
