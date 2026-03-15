# PlexDB
**plexdb** is a high-performance persistent storage library written in C++. It has the following implementations:
 - **objstore**, a CQL object-store database.

## Download

Pre-built binaries and the plexdb static library are attached to each [GitHub release](../../releases/latest):

| Asset | Description |
|---|---|
| `objstore` | Server executable (Linux x86-64) |
| `plexdb-linux-x64.tar.gz` | Static library + C++20 module sources |

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

Requires: CMake ≥ 3.28, liburing ≥ 2.0. Tested with: Ninja, Clang ≥ 19.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_C_COMPILER=clang-19
ninja -C build
```
