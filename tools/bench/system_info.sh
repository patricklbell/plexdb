#!/usr/bin/env bash
# system_info.sh — print a JSON hardware/kernel/software fingerprint for a benchmark run.
#
# Every benchmark result embeds this so it stays self-describing even if the
# machine or software stack drifts later.
#
# Usage:
#   ./tools/bench/system_info.sh [options]
#     -d <dir>     bench scratch/data dir (for fs+mount info)   (default: /tmp)
#     -m <mode>    deployment mode (container|native), recorded verbatim
#     -M <port>    plexdb native protocol port, to query its version (default: none)
#     -C <port>    Cassandra native protocol port, to query its version (default: none)
#     -h           show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BENCH_DIR="/tmp"
MODE=""
PLEXDB_PORT=""
CASSANDRA_PORT=""

while getopts "d:m:M:C:h" opt; do
    case "$opt" in
        d) BENCH_DIR="$OPTARG" ;;
        m) MODE="$OPTARG" ;;
        M) PLEXDB_PORT="$OPTARG" ;;
        C) CASSANDRA_PORT="$OPTARG" ;;
        h)
            sed -n '2,11p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

json_str() {
    python3 -c 'import json, sys; print(json.dumps(sys.argv[1]))' "$1"
}

cpu_model="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
cpu_count="$(nproc)"
threads_per_core="$(lscpu 2>/dev/null | awk -F: '/Thread\(s\) per core/{gsub(/ /,"",$2); print $2}')"
numa_nodes="$(lscpu 2>/dev/null | awk -F: '/NUMA node\(s\)/{gsub(/ /,"",$2); print $2}')"

governor="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
boost="$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo unknown)"
thp="$(grep -o '\[.*\]' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null | tr -d '[]' || echo unknown)"
swappiness="$(cat /proc/sys/vm/swappiness 2>/dev/null || echo unknown)"
io_uring_disabled="$(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo unknown)"
aslr="$(cat /proc/sys/kernel/randomize_va_space 2>/dev/null || echo unknown)"
smt_control="$(cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo unknown)"

kernel_release="$(uname -r)"
kernel_cmdline="$(cat /proc/cmdline 2>/dev/null || echo unknown)"

mnt_line="$(findmnt -no SOURCE,FSTYPE,OPTIONS --target "$BENCH_DIR" 2>/dev/null || echo "unknown unknown unknown")"
fs_source="$(echo "$mnt_line" | awk '{print $1}')"
fs_type="$(echo "$mnt_line" | awk '{print $2}')"
fs_options="$(echo "$mnt_line" | cut -d' ' -f3-)"
# strip btrfs "[/subvol]" suffix findmnt appends to SOURCE
fs_device="${fs_source%%\[*}"

disk_dev="$(lsblk -no PKNAME "$fs_device" 2>/dev/null | head -1 || true)"
disk_model="unknown"
disk_rota="unknown"
if [ -n "$disk_dev" ]; then
    disk_model="$(lsblk -dno MODEL "/dev/$disk_dev" 2>/dev/null || echo unknown)"
    disk_rota="$(lsblk -dno ROTA "/dev/$disk_dev" 2>/dev/null || echo unknown)"
fi

docker_version="$(docker --version 2>/dev/null || echo "docker not found")"

# The compiler actually used for the build, from CMake's own record if a
# build dir exists; falls back to whatever $CXX/clang++/g++ resolves to.
CXX_FROM_CACHE=""
if [ -f "$ROOT_DIR/build/CMakeCache.txt" ]; then
    CXX_FROM_CACHE="$(awk -F= '/^CMAKE_CXX_COMPILER:/{print $2}' "$ROOT_DIR/build/CMakeCache.txt")"
fi
CXX_BIN="${CXX_FROM_CACHE:-${CXX:-clang++}}"
compiler_version="$("$CXX_BIN" --version 2>/dev/null | head -1 || echo unknown)"
build_type="unknown"
if [ -f "$ROOT_DIR/build/CMakeCache.txt" ]; then
    build_type="$(awk -F= '/^CMAKE_BUILD_TYPE:/{print $2}' "$ROOT_DIR/build/CMakeCache.txt")"
fi

git_sha="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
git_dirty=false
if ! git -C "$ROOT_DIR" diff --quiet 2>/dev/null || ! git -C "$ROOT_DIR" diff --cached --quiet 2>/dev/null; then
    git_dirty=true
fi

plexdb_version="unknown"
if [ -n "$PLEXDB_PORT" ] && command -v cqlsh >/dev/null 2>&1; then
    plexdb_version="$(cqlsh 127.0.0.1 "$PLEXDB_PORT" -e "SELECT release_version FROM system.local;" 2>/dev/null \
        | awk 'NR==4{print $1}' || echo unknown)"
fi

cassandra_version="unknown"
if [ -n "$CASSANDRA_PORT" ]; then
    cassandra_version="$(docker run --rm --network host cassandra:5.0 cqlsh 127.0.0.1 "$CASSANDRA_PORT" \
        -e "SELECT release_version FROM system.local;" 2>/dev/null \
        | awk 'NR==4{print $1}' || echo unknown)"
fi

cat <<EOF
{
  "hostname": $(json_str "$(hostname)"),
  "mode": $(json_str "$MODE"),
  "cpu": {
    "model": $(json_str "$cpu_model"),
    "logical_cpus": $cpu_count,
    "threads_per_core": $(json_str "$threads_per_core"),
    "numa_nodes": $(json_str "$numa_nodes")
  },
  "tuning": {
    "governor": $(json_str "$governor"),
    "boost": $(json_str "$boost"),
    "transparent_hugepage": $(json_str "$thp"),
    "swappiness": $(json_str "$swappiness"),
    "io_uring_disabled": $(json_str "$io_uring_disabled"),
    "aslr": $(json_str "$aslr"),
    "smt_control": $(json_str "$smt_control")
  },
  "kernel": {
    "release": $(json_str "$kernel_release"),
    "cmdline": $(json_str "$kernel_cmdline")
  },
  "storage": {
    "bench_dir": $(json_str "$BENCH_DIR"),
    "fs_source": $(json_str "$fs_source"),
    "fs_type": $(json_str "$fs_type"),
    "fs_options": $(json_str "$fs_options"),
    "disk_model": $(json_str "$disk_model"),
    "disk_rotational": $(json_str "$disk_rota")
  },
  "software": {
    "docker_version": $(json_str "$docker_version"),
    "compiler_version": $(json_str "$compiler_version"),
    "cmake_build_type": $(json_str "$build_type"),
    "git_sha": $(json_str "$git_sha"),
    "git_dirty": $git_dirty,
    "plexdb_release_version": $(json_str "$plexdb_version"),
    "cassandra_release_version": $(json_str "$cassandra_version")
  }
}
EOF
