## Code style
- Do not use the standard template library (stl/std) unless strictly necessary. instead use the `plexdb` library. This library is similar to the stl but is much simpler.
- Prefer structs of POD. Only use classes for lifetime and resource management. For example, the `os::File` which wraps the file `os::Handle` and releases the handle. Only make members private if strictly necessary.
- Do not define class methods except for compatibility with libaries. Use functions instead.
- The `os` library should wrap os calls and should be very simple.
    - OS specific includes and calls should be wrapped in platform macros from `macros.h`
- Prefer single-expression idioms over ternaries or if-else. E.g. saturating subtract: `max(x, y) - y` instead of `(x > y) ? (x - y) : 0`.

## Commenting style
- Minimal comments. Do not explain what is obvious from the code.
- Use `@todo` for future work
- Use `@note` for important assumptions or unexpected behaviours
- Use `@profile` for performance-related notes
- Use `@padding` for struct padding notes
- No inline comments explaining standard operations (e.g. "// Free memory", "// Check if null")

## Dev environment
- C++ 20 modules using cmake as a build system
- The project is split into static libraries which can be built separately
    - The core library `plexdb`
    - The CQL object storage library `objstore`

## Testing instructions
- Create tests for your changes. Make sure the tests pass.
- Tests are written in the Catch2 testing framework.
- Test can use the STL but should try to use the `plexdb` library if possible.
- Cmake compiles the tests for each project into cli executables
    - `plexdb` -> `build/plexdb/plexdb_tests`
    - `objstore` -> `build/objstore/objstore_tests`
    - etc.
- Catch2 supports benchmarks. Use the `--skip-benchmarks` command line options to skip benchmarks during normal testing. Carefully consider before adding benchmarks.
- Here is a link to the Catch2 documentation on github, https://github.com/catchorg/Catch2/blob/devel/docs/.