# PlexDB Design

## Overview

Designed for **predictable low-latency and high throughput** on modern superscalar processors and NVMe SSDs. It leverages:

1. **Shard-local ownership:** cores own their data, cache, and I/O.
2. **Non-blocking concurrency:** fibers only yield on async events; cores never block.
3. **User-space caching:** explicit control over memory and eviction for predictable latency.
4. **Asynchronous SSD access:** io_uring submission and completion queues replace blocking reads/writes.
5. **Pipeline-friendly design:** memory layout and fiber scheduling optimized for modern superscalar processors.

---

## 1. Shard Architecture

```mermaid
flowchart LR
    Client["Client"]
    Network["Network Stack (TCP/HTTP/IPC)"]
    EventLoop["Shard Event Loop"]
    FiberScheduler["Fiber Scheduler"]
    UserCache["User-Space Cache"]
    ioUring["io_uring / SSD"]
    Response["Response to Client"]

    Client --> Network --> EventLoop --> FiberScheduler
    FiberScheduler --> UserCache
    UserCache -->|Cache Hit| Response
    UserCache -->|Cache Miss| ioUring
    ioUring --> FiberScheduler
    FiberScheduler --> Response
```

---

## 2. Request Data Flow

```mermaid
flowchart TB
    Client["Client Request"]
    Parse["Parse and Plan"]
    Fiber["Execute"]
    Cache["User-Space Cache"]
    SSD["SSD via io_uring"]
    Process["Finalize"]
    Send["Response"]

    Client --> Parse --> Fiber
    Fiber --> Cache
    Cache -->|Hit| Process
    Cache -->|Miss| SSD
    SSD --> Fiber
    Process --> Send --> Client
```

---

## 3. Per-Core Shard + Fiber Scheduler

```mermaid
flowchart LR
    EventLoop["Event Loop"]
    FiberQueue["Fiber Queue"]
    Fiber1["Fiber #1"]
    Fiber2["Fiber #2"]
    FiberN["Fiber #N"]
    Cache["User-Space Cache"]
    SSD["SSD Queue / io_uring"]

    EventLoop --> FiberQueue
    FiberQueue --> Fiber1
    FiberQueue --> Fiber2
    FiberQueue --> FiberN
    Fiber1 --> Cache
    Fiber2 --> Cache
    FiberN --> Cache
    Cache -->|Miss -> async I/O| SSD
    SSD --> Fiber1
    SSD --> Fiber2
    SSD --> FiberN
```

