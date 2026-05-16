# PlexDB
**PlexDB** is an experimental high-performance database written in C++. It provides:
 - A cassandra like NoSQL server.
 - A redis persistent key-value store.

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
