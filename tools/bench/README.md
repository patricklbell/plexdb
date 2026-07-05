# Reproducible benchmarks: plexdb vs. Cassandra

Runs `cassandra-stress` against `plexdb_cql_server` and a real Apache
Cassandra, pinned and mode-matched for a fair comparison, records a full
hardware/software fingerprint with every result, and can check a run
against this machine's own history to catch performance regressions.

Builds on `tools/cassandra_stress/`, which just fires `cassandra-stress` at
a single already-running server — this directory adds the orchestration,
tuning, dual-target comparison, and regression tracking around it.

## Quick start

```bash
# One-time: see what this machine's tuning looks like, and what invasive
# changes are recommended (not applied automatically).
tools/bench/tune.sh --check -d /tmp/plexdb-bench-data -C 2-9

# Seed this machine's baseline (first run, real op counts, takes a while)
tools/bench/run.sh --update-baseline

# Day to day: fail loudly if this machine's own history regressed
tools/bench/run.sh --baseline

# From another machine on the LAN, reachable over ssh with docker + the
# build toolchain already installed:
tools/bench/remote_run.sh dev@other-machine
```

## Options (`run.sh`)

| Flag | Description | Default |
|------|-------------|---------|
| `-n <ops>` | operations per workload | `100000` |
| `-t <threads>` | client threads | `16` |
| `-C <cpulist>` | cpu list to pin server + client to | `0-3` |
| `-r <reps>` | repetitions per target/workload (first discarded as warm-up) | `5` |
| `-d <dir>` | bench scratch/data dir | `/tmp/plexdb-bench-data` |
| `--mode <m>` | `container` or `native` | this machine's baseline mode, else `container` |
| `--targets <l>` | comma-separated: `plexdb,cassandra` | `plexdb,cassandra` |
| `--baseline` | compare against `baseline/<slug>.json`, exit non-zero on regression | off |
| `--update-baseline` | overwrite `baseline/<slug>.json` with this run's medians | off |

## Methodology

**Symmetry.** plexdb and Cassandra always run the same way as each other —
both containerized (default) or both native — never one of each, since
comparing a containerized server against a native one would confound the
result with container overhead instead of measuring the databases. The
container image for plexdb (`plexdb_container/Dockerfile`) builds *inside*
`ubuntu:24.04` with the same toolchain CI already pins, rather than
bind-mounting a host-built binary — a binary built natively on a
rolling-release host can be linked against a newer glibc than a slim
runtime image provides, so it wouldn't even start.

**Sequential, not concurrent.** Targets never run at the same time, so both
containers reuse Cassandra's default port 9042 — with `--network host`,
port mapping doesn't apply anyway, and there's nothing to collide with.

**Statistical noise reduction.** Each target/workload runs `-r` times
(default 5); the first repetition is discarded as warm-up and the rest are
combined with the per-metric *median* before anything is written to a
result file or compared against a baseline.

**Per-machine baselines.** `baseline/<slug>.json` is keyed by a hardware
slug (CPU model + thread count), so a MacBook's numbers never get compared
against a desktop's. `compare.py` refuses cross-slug comparisons outright
and warns (without failing) on a hostname mismatch within the same slug.

**System fingerprint.** Every result embeds `system_info.sh`'s output —
CPU, governor/boost/THP/swappiness/io_uring state, kernel cmdline,
filesystem + mount options for the bench dir, compiler, and the actual
plexdb git sha / Cassandra release version — so a result found months from
now is still self-describing.

**Container-vs-native sanity check.** `container` mode is the default, but
container overhead varies by machine. Before trusting it, run plexdb's own
benchmark once in each mode and diff them:

```bash
tools/bench/run.sh --mode container --targets plexdb -d /tmp/bench-c
tools/bench/run.sh --mode native    --targets plexdb -d /tmp/bench-n
python3 tools/bench/compare.py --self-check \
  tools/bench/results/<container-run>.json tools/bench/results/<native-run>.json
```

If they diverge by more than 5%, default that machine to `--mode native`
(pass it explicitly, or set `"mode": "native"` in that machine's baseline
file — `run.sh` reads the baseline's mode when `--mode` isn't given).

**Tuning.** `tune.sh` only ever makes reversible, session-scoped changes
(cpu governor, boost, swappiness, marking the bench dir `nodatacow` on
btrfs) behind `--apply`/`--reset`. `--check` additionally prints exact
commands for more invasive changes it won't make for you — kernel cmdline
CPU isolation (`isolcpus`/`nohz_full`/`rcu_nocbs`), disabling SMT, locking a
fixed clock frequency, moving the bench dir off a compressing filesystem,
and disabling swap outright.

**ASLR.** `run.sh` launches native-mode plexdb under `setarch -R` to
disable address-space-layout randomization for that process only — a
known source of run-to-run cache/branch-layout noise in short benchmarks,
with no system-wide or sudo cost.

## Regression tracking

Mirrors `tools/cql_tests/run.sh` + `mustpass.txt`: a baseline checked into
git, a script that re-derives current numbers and diffs them against it,
and a non-zero exit on regression — the same shape, just numeric-with-
tolerance (default ±10%, see `compare.py --tolerance`) instead of
exact-match pass/fail.

Results are **not** auto-committed. `run.sh` always writes to
`results/<timestamp>_<hostname>_<git-sha>.json` (gitignored — local history
only); `--update-baseline` writes/overwrites `baseline/<slug>.json`, which
you commit by hand when you want it tracked.

## Remote / multi-machine use

No daemon, no registration step. `remote_run.sh <user@host>` just needs the
target reachable over ssh with docker and the build toolchain already
installed, and a checkout of this repo (default `~/plexbd`, override with
`-p`). It syncs the local commit (or, if the working tree has uncommitted
changes, rsyncs it instead), runs `run.sh` remotely, and pulls
`results/*.json` back — already named with that machine's hostname, so
nothing collides with local results.

## Prior art

Not adopted as dependencies, but the reference points for the design:

- **`pyperf system tune`** — a general Linux benchmark-environment tuner
  (governor, ASLR, IRQ affinity). `tune.sh` reimplements a narrow slice of
  this in bash rather than pull in a Python-tooling dependency for
  something this small.
- **ScyllaDB `perftune.py`** and their published benchmarking methodology
  (pinned cores, disabled swap, IRQ/NIC/disk affinity for their sharded
  architecture).
- **ClickHouse's ClickBench** and **CockroachDB's `roachperf`** — both
  record a full hardware fingerprint per result and compare historically
  rather than against one fixed number, the same shape as `system_info.sh`
  + per-machine baselines here.
- **This repo's own `tools/cql_tests/run.sh` + `mustpass.txt`** — the
  direct template for `baseline/` + `compare.py`.
