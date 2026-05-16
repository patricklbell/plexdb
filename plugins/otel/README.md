Exports plexdb metrics and log events via OpenTelemetry (OTLP/gRPC).

- `Counter` stats → OTLP Counter instruments (cumulative deltas)
- `Gauge` stats → OTLP Gauge instruments (point-in-time)
- Log messages with a `message_id` → OTLP log records; the attribute name is formed as `<producer>.<message_id>` with the `otlp.` producer prefix stripped

## Usage

```sh
LD_PRELOAD=libplexdb_otel_plugin.so plexdb_cql_server /path/to/db
```

## Configuration

| Variable | Default | Description |
|---|---|---|
| `PLEXDB_OTLP_ENDPOINT` | `localhost:4317` | OTLP/gRPC collector endpoint |
| `PLEXDB_OTLP_INTERVAL_MS` | `10000` | Export interval in milliseconds |
| `PLEXDB_OTLP_SERVICE` | `plexdb` | `service.name` resource attribute |

## Building

Requires `libgrpc++-dev` and `libprotobuf-dev`. See `INSTALL.md` for details.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```
