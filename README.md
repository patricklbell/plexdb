# PlexDB

**PlexDB** is an experimental high-performance persistent and in-memory database.

A common storage layer is provided which supports:
 - A NoSQL database. Applications connect through the [Cassandra](https://cassandra.apache.org/) drivers. 
 - A key-value database. Applications connect through the [Redis](https://redis.io/) drivers.

Application developers may reference the examples in `docs`.

## Download

Pre-built server binaries are attached to each [release](../../releases/latest).

## Building from source

### Servers

Requires: CMake ≥ 3.28, liburing-devel ≥ 2.0. Tested with: Ninja, Clang ≥ 19.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

*Note that the configured compiler needs to support c++23 and modules*

To enable io_uring as the I/O backend requires the `CAP_IPC_LOCK` capability or a `RLIMIT_MEMLOCK` large enough to cover the ring's buffers. RPM and DEB release packages set this capability automatically.

```sh
sudo setcap cap_ipc_lock+ep build/cql/plexdb_cql_server
```

### Plugins

See the README.md files under `plugins`.

## Development setup

After cloning, activate the pre-commit formatting hook:

```sh
git config core.hooksPath .hooks
```

The hook runs `clang-format-19 --dry-run` on staged files.
