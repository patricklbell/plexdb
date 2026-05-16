Writes structured plexdb log events to a text file. Each line is formatted as:

```
[producer] [YYYY-MM-DD HH:MM:SS] [LEVEL:message_id] text
```

## Usage

```sh
LD_PRELOAD=libplexdb_plugin_plugin.so plexdb_cql_server /path/to/db
```

## Configuration

| Variable | Default | Description |
|---|---|---|
| `PLEXDB_LOG_FILE` | `plexdb.log` | Output file path (opened in append mode) |
| `PLEXDB_LOG_BATCH` | `1` | Lines to buffer before flushing |
| `PLEXDB_LOG_STDERR` | unset | Mirror output to stderr if set |
| `PLEXDB_LOG_STDOUT` | unset | Mirror output to stdout if set (ignored when `PLEXDB_LOG_STDERR` is set) |

## Building

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```
