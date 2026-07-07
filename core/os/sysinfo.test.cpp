#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.os;

using namespace plexdb;
using namespace plexdb::os;

TEST_CASE("SystemInfo basic sanity", "[plexdb.os.sysinfo]") {
    const SystemInfo* info = get_system_info();
    REQUIRE(info != nullptr);

    SECTION("processor counts") {
        REQUIRE(info->logical_processor_count >= 1);
        REQUIRE(info->physical_processor_count >= 1);
        REQUIRE(info->logical_processor_count >= info->physical_processor_count);
    }

    SECTION("memory sizes") {
        REQUIRE(info->page_size >= 4096);
        REQUIRE(info->page_size % 4096 == 0);
        REQUIRE(info->large_page_size >= info->page_size);
        REQUIRE(info->allocation_granularity >= info->page_size);
        REQUIRE(info->total_memory > 0);
    }

    SECTION("cache line") {
        REQUIRE(info->cache_line_size >= 32);
        REQUIRE(info->cache_line_size <= 256);
    }
}

TEST_CASE("ProcessInfo basic sanity", "[plexdb.os.sysinfo]") {
    const ProcessInfo* info = get_process_info();
    REQUIRE(info != nullptr);

    REQUIRE(info->pid > 0);
}

TEST_CASE("KernelFeatures basic sanity", "[plexdb.os.sysinfo]") {
    const KernelFeatures* k = get_kernel_features();
    REQUIRE(k != nullptr);

    SECTION("kernel version") {
        REQUIRE(k->major >= 2);
        REQUIRE(k->major < 100);
    }

    SECTION("aio support") {
        REQUIRE((k->aio_supported == 0 || k->aio_supported == 1));
    }

    SECTION("io_uring") {
        if (k->io_uring.supported) {
            REQUIRE(k->io_uring.max_sq_entries >= 1);
            REQUIRE(k->io_uring.max_cq_entries >= 1);
        }
    }
}

TEST_CASE("query_memory_status returns valid data", "[plexdb.os.sysinfo]") {
    MemoryStatus status = query_memory_status();

    REQUIRE(status.total_bytes > 0);
    REQUIRE(status.free_bytes > 0);
    REQUIRE(status.available_bytes > 0);
    REQUIRE(status.available_bytes <= status.total_bytes);
}

TEST_CASE("query_disk_status returns valid data", "[plexdb.os.sysinfo]") {
    DiskStatus status = query_disk_status("/");

    REQUIRE(status.total_bytes > 0);
    REQUIRE(status.available_bytes > 0);
    REQUIRE(status.block_size > 0);
    REQUIRE(status.available_bytes <= status.total_bytes);
}

TEST_CASE("sysinfo singleton returns same pointer", "[plexdb.os.sysinfo]") {
    const SystemInfo* a = get_system_info();
    const SystemInfo* b = get_system_info();
    REQUIRE(a == b);

    const ProcessInfo* pa = get_process_info();
    const ProcessInfo* pb = get_process_info();
    REQUIRE(pa == pb);

    const KernelFeatures* ka = get_kernel_features();
    const KernelFeatures* kb = get_kernel_features();
    REQUIRE(ka == kb);
}

TEST_CASE("get_node_id is stable and non-zero", "[plexdb.os.sysinfo]") {
    Array<U8, 6> a = get_node_id();
    Array<U8, 6> b = get_node_id();

    SECTION("stable across calls") {
        for (U64 i = 0; i < 6; i++) {
            REQUIRE(a.values[i] == b.values[i]);
        }
    }

    SECTION("not the all-zero node") {
        bool any = false;
        for (U64 i = 0; i < 6; i++) {
            any |= (a.values[i] != 0);
        }
        REQUIRE(any);
    }
}
