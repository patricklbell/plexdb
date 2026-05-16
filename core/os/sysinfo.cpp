module;
#include <plexdb/macros/macros.h>

#if PLEXDB_OS_LINUX
    #include <unistd.h>
    #include <sys/utsname.h>
    #include <sys/resource.h>
    #include <sys/statvfs.h>
    #include <sys/mman.h>
    #include <sys/syscall.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <linux/aio_abi.h>
    #include <linux/io_uring.h>
    #include <stdlib.h>
    #include <stdio.h>
    #include <string.h>
    #include <limits.h>
#endif

module plexdb.os.sysinfo;

namespace plexdb::os {
    #if PLEXDB_OS_LINUX

        // ========================================================================
        // helpers
        // ========================================================================
        namespace {
            SystemInfo     g_system_info;
            ProcessInfo    g_process_info;
            KernelFeatures g_kernel_features;
            bool           g_initialized = false;

            U64 parse_u64(const char* str) {
                U64 result = 0;
                while (*str >= '0' && *str <= '9') {
                    result = result * 10 + (*str - '0');
                    str++;
                }
                return result;
            }

            bool starts_with(const char* str, const char* prefix) {
                while (*prefix) {
                    if (*str++ != *prefix++) return false;
                }
                return true;
            }

            const char* skip_whitespace(const char* str) {
                while (*str == ' ' || *str == '\t') str++;
                return str;
            }

            const char* skip_to_value(const char* str) {
                while (*str && *str != ':') str++;
                if (*str == ':') str++;
                return skip_whitespace(str);
            }

            U64 read_sysfs_u64(const char* path, U64 fallback) {
                int fd = open(path, O_RDONLY);
                if (fd == -1) return fallback;

                char buf[64];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);

                if (n <= 0) return fallback;
                buf[n] = '\0';
                return parse_u64(buf);
            }

            U64 read_meminfo_field(const char* field_name) {
                int fd = open("/proc/meminfo", O_RDONLY);
                if (fd == -1) return 0;

                char buf[4096];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n <= 0) return 0;
                buf[n] = '\0';

                U64 field_len = strlen(field_name);
                const char* line = buf;
                while (*line) {
                    if (starts_with(line, field_name) && line[field_len] == ':') {
                        const char* val = skip_to_value(line);
                        U64 kb = parse_u64(val);
                        return kb * 1024;
                    }
                    while (*line && *line != '\n') line++;
                    if (*line == '\n') line++;
                }
                return 0;
            }

            void parse_kernel_version(KernelFeatures* k) {
                struct utsname un;
                if (uname(&un) != 0) {
                    k->major = 0;
                    k->minor = 0;
                    k->patch = 0;
                    return;
                }

                const char* p = un.release;
                k->major = static_cast<U32>(parse_u64(p));
                while (*p && *p != '.') p++;
                if (*p == '.') p++;
                k->minor = static_cast<U32>(parse_u64(p));
                while (*p && *p != '.') p++;
                if (*p == '.') p++;
                k->patch = static_cast<U32>(parse_u64(p));
            }

            bool kernel_at_least(U32 major, U32 minor) {
                if (g_kernel_features.major > major) return true;
                if (g_kernel_features.major == major && g_kernel_features.minor >= minor) return true;
                return false;
            }

            U32 count_logical_processors() {
                long n = sysconf(_SC_NPROCESSORS_ONLN);
                return (n > 0) ? static_cast<U32>(n) : 1;
            }

            U32 count_physical_cores() {
                // @note counts unique physical core IDs from /proc/cpuinfo
                int fd = open("/proc/cpuinfo", O_RDONLY);
                if (fd == -1) return count_logical_processors();

                char buf[32768];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n <= 0) return count_logical_processors();
                buf[n] = '\0';

                U32 max_core_id = 0;
                U32 found_any = 0;

                const char* line = buf;
                while (*line) {
                    if (starts_with(line, "core id")) {
                        const char* val = skip_to_value(line);
                        U32 core_id = static_cast<U32>(parse_u64(val));
                        if (core_id > max_core_id) max_core_id = core_id;
                        found_any = 1;
                    }
                    while (*line && *line != '\n') line++;
                    if (*line == '\n') line++;
                }

                return found_any ? (max_core_id + 1) : count_logical_processors();
            }

            U64 get_cache_line_size() {
                U64 size = read_sysfs_u64("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size", 0);
                return (size > 0) ? size : 64;
            }

            U64 get_page_size() {
                long ps = sysconf(_SC_PAGESIZE);
                return (ps > 0) ? static_cast<U64>(ps) : 4096;
            }

            U64 get_large_page_size() {
                U64 size = read_meminfo_field("Hugepagesize");
                return (size > 0) ? size : (2 * 1024 * 1024);
            }

            U64 get_total_memory() {
                long pages = sysconf(_SC_PHYS_PAGES);
                long ps = sysconf(_SC_PAGESIZE);
                if (pages > 0 && ps > 0) {
                    return static_cast<U64>(pages) * static_cast<U64>(ps);
                }
                return read_meminfo_field("MemTotal");
            }

            U64 get_mlock_limit() {
                struct rlimit rl;
                if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
                    return (rl.rlim_cur == RLIM_INFINITY) ? MAX_U64 : static_cast<U64>(rl.rlim_cur);
                }
                return 0;
            }

            U32 get_vma_limit() {
                FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
                if (!f) {
                    long max_map_count;
                    if (fscanf(f, "%ld", &max_map_count) == 1) {
                        return static_cast<U32>(max_map_count);
                    }
                }
                return MAX_U32;
            }

            U32 get_disk_queue_depth() {
                constexpr U32 DEFAULT_QUEUE_DEPTH = 32;
                constexpr U32 ROTATIONAL_QUEUE_DEPTH = 8;

                DIR* sys_block = opendir("/sys/block");
                if (sys_block) {
                    struct dirent* entry;
                    while ((entry = readdir(sys_block)) != nullptr) {
                        if (entry->d_name[0] == '.') continue;

                        if (strncmp(entry->d_name, "loop", 4) == 0) continue;
                        if (strncmp(entry->d_name, "nvme", 4) == 0) continue;

                        char rotational_path[512];
                        snprintf(rotational_path, sizeof(rotational_path), "/sys/block/%s/queue/rotational", entry->d_name);
                        if (read_sysfs_u64(rotational_path, 1) == 1) {
                            closedir(sys_block);
                            return ROTATIONAL_QUEUE_DEPTH;
                        }
                    }
                    closedir(sys_block);
                }

                return DEFAULT_QUEUE_DEPTH;
            }

            void probe_io_uring(IoUringFeatures* f) {
                f->supported = 0;
                f->max_sq_entries = 0;
                f->max_cq_entries = 0;
                f->sqpoll = 0;
                f->iopoll = 0;
                f->registered_buffers = 0;
                f->registered_files = 0;
                f->max_iovecs = 0;

                if (!kernel_at_least(5, 1)) return;

                struct io_uring_params params = {};
                int fd = static_cast<int>(syscall(__NR_io_uring_setup, 32, &params));
                if (fd < 0) return;

                f->supported = 1;
                f->max_sq_entries = params.sq_entries;
                f->max_cq_entries = params.cq_entries;

                long max_iovecs = sysconf(_SC_IOV_MAX);
                f->max_iovecs = (max_iovecs > 0) ? static_cast<U32>(max_iovecs) : 0;

                if (params.features & IORING_FEAT_SQPOLL_NONFIXED) {
                    f->sqpoll = 1;
                }

                // @note iopoll requires kernel 5.1+, registered buffers 5.1+, registered files 5.1+
                f->iopoll = 1;
                f->registered_buffers = 1;
                f->registered_files = 1;

                close(fd);
            }

            void probe_aio(KernelFeatures* k) {
                k->aio_supported = 0;
                k->aio_poll_supported = 0;

                aio_context_t ctx = 0;
                int ret = static_cast<int>(syscall(__NR_io_setup, 1, &ctx));
                if (ret == 0) {
                    k->aio_supported = 1;
                    syscall(__NR_io_destroy, ctx);

                    // IOCB_CMD_POLL available in kernel 4.18+
                    k->aio_poll_supported = kernel_at_least(4, 18) ? 1 : 0;
                }
            }

            B32 probe_large_pages_allowed() {
                void* ptr = mmap(nullptr, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
                if (ptr == MAP_FAILED) return 0;
                munmap(ptr, 2 * 1024 * 1024);
                return 1;
            }

            B32 probe_transparent_huge_pages() {
                int fd = open("/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY);
                if (fd == -1) return 0;

                char buf[128];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n <= 0) return 0;
                buf[n] = '\0';

                // @note format is: "always [madvise] never" - bracketed is current
                const char* p = buf;
                while (*p) {
                    if (*p == '[') {
                        p++;
                        if (starts_with(p, "always") || starts_with(p, "madvise")) {
                            return 1;
                        }
                        return 0;
                    }
                    p++;
                }
                return 0;
            }

            void ensure_initialized() {
                if (g_initialized) return;

                // kernel features first (needed for version checks)
                parse_kernel_version(&g_kernel_features);
                probe_io_uring(&g_kernel_features.io_uring);
                probe_aio(&g_kernel_features);
                g_kernel_features.transparent_huge_pages = probe_transparent_huge_pages();

                // system info
                g_system_info.logical_processor_count = count_logical_processors();
                g_system_info.physical_processor_count = count_physical_cores();
                g_system_info.page_size = get_page_size();
                g_system_info.large_page_size = get_large_page_size();
                g_system_info.allocation_granularity = g_system_info.page_size;
                g_system_info.total_memory = get_total_memory();
                g_system_info.cache_line_size = get_cache_line_size();
                g_system_info.mlock_limit = get_mlock_limit();
                g_system_info.vma_limit = get_vma_limit();
                g_system_info.disk_queue_depth = get_disk_queue_depth();

                // process info
                g_process_info.pid = static_cast<U32>(getpid());
                g_process_info.large_pages_allowed = probe_large_pages_allowed();

                g_initialized = true;
            }
        }

        // ============================================================================
        // public interface
        // ============================================================================
        const SystemInfo* get_system_info() {
            ensure_initialized();
            return &g_system_info;
        }

        const ProcessInfo* get_process_info() {
            ensure_initialized();
            return &g_process_info;
        }

        const KernelFeatures* get_kernel_features() {
            ensure_initialized();
            return &g_kernel_features;
        }

        MemoryStatus query_memory_status() {
            MemoryStatus status;
            status.total_bytes = read_meminfo_field("MemTotal");
            status.available_bytes = read_meminfo_field("MemAvailable");
            status.free_bytes = read_meminfo_field("MemFree");
            return status;
        }

        DiskStatus query_disk_status(String8 path) {
            DiskStatus status = {};

            struct statvfs st;
            if (statvfs(path.c_str(), &st) == 0) {
                status.total_bytes = static_cast<U64>(st.f_blocks) * st.f_frsize;
                status.available_bytes = static_cast<U64>(st.f_bavail) * st.f_frsize;
                status.block_size = static_cast<U64>(st.f_bsize);
            }

            return status;
        }

    #else
        #error "sysinfo not implemented for this OS"
    #endif
}
