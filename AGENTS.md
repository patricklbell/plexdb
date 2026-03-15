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