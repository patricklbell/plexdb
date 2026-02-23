export module plexdb.os.sysinfo;

import plexdb.base;

export namespace plexdb::os {
    // ========================================================================
    // system info
    // ========================================================================
    struct SystemInfo {
        U32 logical_processor_count;
        U32 physical_processor_count;
        U64 page_size;
        U64 large_page_size;
        U64 allocation_granularity;
        U64 total_memory;
        U64 cache_line_size;
        U64 mlock_limit;
    };

    // ========================================================================
    // process info
    // ========================================================================
    struct ProcessInfo {
        U32 pid;
        B32 large_pages_allowed;
    };

    // ========================================================================
    // kernel features
    // ========================================================================
    struct IoUringFeatures {
        B32 supported;
        U32 max_entries;
        B32 sqpoll;
        B32 iopoll;
        B32 registered_buffers;
        B32 registered_files;
    };

    struct KernelFeatures {
        U32 major;
        U32 minor;
        U32 patch;
        
        IoUringFeatures io_uring;
        
        B32 aio_supported;
        B32 aio_poll_supported;
        B32 transparent_huge_pages;
    };

    // ========================================================================
    // global accessors
    // ========================================================================
    const SystemInfo*     get_system_info();
    const ProcessInfo*    get_process_info();
    const KernelFeatures* get_kernel_features();

    // ========================================================================
    // dynamic queries
    // @note these may be expensive and should be used sparingly
    // ========================================================================
    struct MemoryStatus {
        U64 total_bytes;
        U64 available_bytes;
        U64 free_bytes;
    };
    MemoryStatus query_memory_status();

    struct DiskStatus {
        U64 total_bytes;
        U64 available_bytes;
        U64 block_size;
    };
    DiskStatus query_disk_status(String8 path);
}
