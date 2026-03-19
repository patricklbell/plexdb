// Real-time terminal dashboard plugin: renders live database stats using ANSI
// escape codes. Loaded via LD_PRELOAD alongside the server binary.
//
// Usage:
//   LD_PRELOAD=./build/objstore/libobjstore_log_dashboard.so \
//       ./build/objstore/objstore_server ...
//
// Configuration (environment variables):
//   PLEXDB_DASHBOARD_INTERVAL_MS  – minimum refresh interval (default: 500)
//   PLEXDB_DASHBOARD_FD           – file descriptor to write to (default: 2, stderr)

#include "log_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ============================================================================
// state
// ============================================================================
struct DashboardState {
    std::unordered_map<uint32_t, std::string>                              producer_names;
    std::map<std::pair<uint32_t,std::string>, std::string>                 producer_meta;
    std::map<std::pair<uint32_t,uint32_t>, std::string>                    stat_names;
    std::map<std::pair<uint32_t,uint32_t>, int64_t>                        stat_values;
    std::vector<std::pair<std::string, std::string>>                       recent_logs;
    std::mutex                                                             mtx;
    std::chrono::steady_clock::time_point                                  last_render;
    std::chrono::milliseconds                                              interval;
    std::FILE*                                                             out;
    unsigned                                                               max_recent_logs;
};

DashboardState* g_state = nullptr;

// ============================================================================
// rendering
// ============================================================================
void render_dashboard(DashboardState* s) {
    auto now = std::chrono::steady_clock::now();
    if (now - s->last_render < s->interval) return;
    s->last_render = now;

    std::FILE* f = s->out;

    // ANSI: move cursor home + clear screen
    std::fprintf(f, "\033[H\033[2J");

    std::fprintf(f, "\033[1;36m╔══════════════════════════════════════════════════════════════╗\033[0m\n");
    std::fprintf(f, "\033[1;36m║\033[0m  \033[1;37mplexdb real-time dashboard\033[0m                                  \033[1;36m║\033[0m\n");
    std::fprintf(f, "\033[1;36m╠══════════════════════════════════════════════════════════════╣\033[0m\n");

    // group stats by producer
    std::map<uint32_t, std::vector<std::pair<uint32_t,uint32_t>>> by_producer;
    for (const auto& [key, val] : s->stat_values) {
        by_producer[key.first].push_back(key);
    }

    for (const auto& [pid, keys] : by_producer) {
        auto name_it = s->producer_names.find(pid);
        const char* pname = (name_it != s->producer_names.end()) ? name_it->second.c_str() : "???";
        std::fprintf(f, "\033[1;36m║\033[0m  \033[1;33m▸ %s\033[0m (id=%u)", pname, pid);

        // show producer metadata inline
        for (const auto& [mk, mv] : s->producer_meta) {
            if (mk.first == pid) {
                std::fprintf(f, " \033[2m%s=%s\033[0m", mk.second.c_str(), mv.c_str());
            }
        }

        // pad to box width
        std::fprintf(f, "\n");

        for (const auto& key : keys) {
            auto sname_it = s->stat_names.find(key);
            int64_t value = s->stat_values[key];
            if (sname_it != s->stat_names.end()) {
                std::fprintf(f, "\033[1;36m║\033[0m    \033[37m%-30s\033[0m \033[1;32m%lld\033[0m\n",
                    sname_it->second.c_str(), static_cast<long long>(value));
            } else {
                std::fprintf(f, "\033[1;36m║\033[0m    \033[37mstat:%u\033[0m                        \033[1;32m%lld\033[0m\n",
                    key.second, static_cast<long long>(value));
            }
        }
    }

    if (by_producer.empty()) {
        std::fprintf(f, "\033[1;36m║\033[0m  \033[2m(no stats yet)\033[0m\n");
    }

    // recent log messages
    if (!s->recent_logs.empty()) {
        std::fprintf(f, "\033[1;36m╠══════════════════════════════════════════════════════════════╣\033[0m\n");
        std::fprintf(f, "\033[1;36m║\033[0m  \033[1;37mrecent log messages\033[0m\n");
        for (const auto& [level, text] : s->recent_logs) {
            const char* color = "\033[37m";
            if (level == "ERROR") color = "\033[1;31m";
            else if (level == "WARN") color = "\033[1;33m";
            else if (level == "DEBUG") color = "\033[2m";
            else if (level == "TRACE") color = "\033[2m";
            std::fprintf(f, "\033[1;36m║\033[0m    %s[%s]\033[0m %.50s\n",
                color, level.c_str(), text.c_str());
        }
    }

    std::fprintf(f, "\033[1;36m╚══════════════════════════════════════════════════════════════╝\033[0m\n");
    std::fflush(f);
}

// ============================================================================
// event handlers
// ============================================================================
void handle_producer_registered(DashboardState* s, uint32_t id, const char* name) {
    std::lock_guard<std::mutex> guard(s->mtx);
    s->producer_names[id] = name;
    render_dashboard(s);
}

void handle_producer_meta(DashboardState* s, uint32_t producer_id, const char* key, const char* value) {
    std::lock_guard<std::mutex> guard(s->mtx);
    s->producer_meta[{producer_id, std::string(key)}] = value;
    render_dashboard(s);
}

void handle_message(DashboardState* s, uint32_t level, const char* text, size_t len) {
    static constexpr const char* level_tags[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    constexpr size_t n_levels = sizeof(level_tags) / sizeof(level_tags[0]);
    const char* tag = (level < n_levels) ? level_tags[level] : "???";

    std::lock_guard<std::mutex> guard(s->mtx);
    s->recent_logs.push_back({tag, std::string(text, len)});
    if (s->recent_logs.size() > s->max_recent_logs) {
        s->recent_logs.erase(s->recent_logs.begin());
    }
    render_dashboard(s);
}

void handle_stat(DashboardState* s, uint32_t producer_id, uint32_t stat_id, int64_t value) {
    std::lock_guard<std::mutex> guard(s->mtx);
    s->stat_values[{producer_id, stat_id}] = value;
    render_dashboard(s);
}

void handle_stat_meta(DashboardState* s, uint32_t producer_id, uint32_t stat_id, const char* name) {
    std::lock_guard<std::mutex> guard(s->mtx);
    s->stat_names[{producer_id, stat_id}] = name;
    render_dashboard(s);
}

// ============================================================================
// consumer callback
// ============================================================================
void on_event(const PlexdbLogEvent* event, void* ctx) {
    auto* s = static_cast<DashboardState*>(ctx);
    switch (event->type) {
        case PLEXDB_LOG_PRODUCER_REGISTERED:
            handle_producer_registered(s,
                event->producer_registered.producer_id,
                event->producer_registered.name);
            break;
        case PLEXDB_LOG_PRODUCER_META:
            handle_producer_meta(s,
                event->producer_meta.producer_id,
                event->producer_meta.key,
                event->producer_meta.value);
            break;
        case PLEXDB_LOG_MESSAGE:
            handle_message(s,
                event->message.level,
                event->message.text,
                event->message.text_len);
            break;
        case PLEXDB_LOG_STAT:
            handle_stat(s,
                event->stat.producer_id,
                event->stat.stat_id,
                event->stat.value);
            break;
        case PLEXDB_LOG_STAT_META:
            handle_stat_meta(s,
                event->stat_meta.producer_id,
                event->stat_meta.stat_id,
                event->stat_meta.name);
            break;
        default:
            break;
    }
}

// ============================================================================
// lifecycle
// ============================================================================
DashboardState* dashboard_init() {
    auto* s = new DashboardState{};

    const char* interval_env = std::getenv("PLEXDB_DASHBOARD_INTERVAL_MS");
    unsigned ms = interval_env ? static_cast<unsigned>(std::atoi(interval_env)) : 500;
    if (ms == 0) ms = 500;
    s->interval = std::chrono::milliseconds(ms);

    const char* fd_env = std::getenv("PLEXDB_DASHBOARD_FD");
    int fd = fd_env ? std::atoi(fd_env) : 2;
    s->out = (fd == 1) ? stdout : stderr;

    s->max_recent_logs = 8;
    s->last_render = std::chrono::steady_clock::now() - s->interval;
    return s;
}

void dashboard_fini(DashboardState* s) {
    if (!s) return;
    delete s;
}

} // namespace

__attribute__((constructor))
static void init() {
    g_state = dashboard_init();
    plexdb_log_register_consumer(on_event, g_state);
}

__attribute__((destructor))
static void fini() {
    if (!g_state) return;
    plexdb_log_unregister_consumer(on_event, g_state);
    dashboard_fini(g_state);
    g_state = nullptr;
}
