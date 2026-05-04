# PlexDB
**plexdb** is a high-performance persistent storage library written in C++. It has the following implementations:
 - **objstore**, a CQL object-store database.

## Download

Pre-built binaries and the core static library are attached to each [GitHub release](../../releases/latest):

| Asset | Description |
|---|---|
| `objstore` | Server executable (Linux x86-64) |
| `core-linux-x64.tar.gz` | Static library + C++23 module sources |
| `libobjstore_log_otel.so` | OTLP/gRPC metrics plugin (load via `LD_PRELOAD`) |

## Usage

```
objstore <db_path> [--port|-p <port>] [--repl|-r]
```

| Argument | Default | Description |
|---|---|---|
| `db_path` | *(required)* | Path to the database file (created if it does not exist) |
| `--port`, `-p` | `8080` | TCP port for the HTTP server |
| `--repl`, `-r` | — | Start an interactive REPL |

## Building from source

Requires: CMake ≥ 3.28, liburing-devel ≥ 2.0. Tested with: Ninja, Clang ≥ 19.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_C_COMPILER=clang-19
ninja -C build
```

## Plugins

### OTLP metrics plugin

Exports structured log stats to an OpenTelemetry collector via OTLP/gRPC. Requires gRPC and protobuf system libraries.

```sh
cd objstore/plugins/log_otel
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Load at server startup:

```sh
LD_PRELOAD=objstore/plugins/log_otel/build/libobjstore_log_otel.so \
  PLEXDB_OTLP_ENDPOINT=localhost:4317 \
  ./build/objstore/objstore_server <db_path>
```

| Env var | Default | Description |
|---|---|---|
| `PLEXDB_OTLP_ENDPOINT` | `localhost:4317` | OTLP/gRPC collector endpoint |
| `PLEXDB_OTLP_INTERVAL_MS` | `10000` | Export interval in milliseconds |
| `PLEXDB_OTLP_SERVICE` | `plexdb` | Service name reported to collector |
