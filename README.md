# PlexDB
**PlexDB** is an experimental high-performance persistent and in-memory database. It provides:
 - A NoSQL database, supports Cassandra's native protocol.
 - A key-value database, supports the Redis serialization protocol.

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

### Plugins

See the README.md files under `plugins`.

## Development setup

After cloning, activate the pre-commit formatting hook:

```sh
git config core.hooksPath .hooks
```

The hook runs `clang-format --dry-run --Werror` on staged `.cpp`/`.cppm`/`.h` files and rejects the commit if any file needs reformatting. To fix, run:

```sh
clang-format -i <file>
```
