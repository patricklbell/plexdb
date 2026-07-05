# plexdb_cql_server runtime image for benchmarking, built and run the same
# way on every machine regardless of host OS/toolchain/glibc — this matters
# because a binary built natively on a laptop's rolling-release distro can
# be linked against a newer glibc than a runtime image provides. Building
# inside the same base image it later runs in (matching the toolchain
# tools/cassandra_stress and CI already pin: ubuntu-24.04 + clang-19) keeps
# the plexdb build itself a controlled, reproducible variable across
# machines, symmetric with how the Cassandra target is already just a
# pinned image.
#
# Build from the repo root:
#   docker build -f tools/bench/plexdb_container/Dockerfile -t plexdb-bench .

FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build liburing-dev clang-19 clang-tools-19 ca-certificates git pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++-19 \
      -DCMAKE_C_COMPILER=clang-19 \
      -DCMAKE_CXX_SCAN_FOR_MODULES=ON \
    && ninja -C build plexdb_cql_server

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends liburing2 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/cql/plexdb_cql_server /usr/local/bin/plexdb_cql_server

ENTRYPOINT ["/usr/local/bin/plexdb_cql_server"]
