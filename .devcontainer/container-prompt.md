You are running inside a secured Docker DevContainer for the **plexdb** project.

## Network access – domain whitelist

Only the following domains are required and expected to be reachable:

| Domain | Purpose |
|---|---|
| `archive.ubuntu.com`, `security.ubuntu.com` | Ubuntu apt packages |
| `apt.llvm.org` | Clang-19 apt repository |
| `github.com`, `objects.githubusercontent.com`, `api.github.com` | CMake FetchContent (Catch2, lexy), git operations |
| `registry.npmjs.org` | npm packages (`extra/cql_node_example`) |
| `pypi.org`, `files.pythonhosted.org` | Python packages (`extra/cql_py_example`) |
| `astral.sh` | uv installer for Python toolchain |

Outbound traffic to any other domain is unexpected and likely not needed.

## Build

```sh
# Configure (Debug)
cmake -B build -G Ninja -DBUILD_TESTS=ON -DPLEXDB_LOG_ENABLED=ON \
  -DCMAKE_BUILD_TYPE=Debug

# Build everything
ninja -C build

# Run tests
build/plexdb/plexdb_tests --skip-benchmarks
build/objstore/objstore_tests --skip-benchmarks
```

## Docker access (read-only)

Docker access is via a socket proxy that only allows read operations:
- `docker ps` – list running containers
- `docker logs <container>` – view container logs
- `docker inspect <container>` – inspect container details

**Blocked** (prevents container escape):
- `docker run`, `docker exec`, `docker build`

## Git access (local only)

- `git log`, `git status`, `git diff` – full read access
- `git commit`, `git branch`, `git stash` – local operations only
- `git push` – **blocked** (no SSH keys are mounted)

## Privilege restrictions

- No `sudo` – cannot escalate privileges
- Non-root user (`vscode`) with all Linux capabilities dropped
- `no-new-privileges` enforced