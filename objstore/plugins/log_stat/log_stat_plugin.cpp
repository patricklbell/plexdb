// Stat-logging plugin: writes structured stat events to a line-oriented text file.
// Designed for consumption by extra/plot_stats.py or tail -f for live monitoring.
//
// Output format (one line per event, tab-separated):
//   P <producer_id>\t<name>                   — producer registration
//   D <producer_id>\t<key>\t<value>           — producer metadata
//   M <producer_id>\t<stat_id>\t<name>        — stat metadata
//   S <producer_id>\t<stat_id>\t<timestamp_ns>\t<value>  — stat sample
//
// Configuration (environment variables):
//   PLEXDB_STAT_FILE  – destination path  (default: plexdb.stats)

#include "log_abi.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <string>

namespace {

struct StatPluginState {
    std::FILE* file;
    std::mutex mtx;
};

StatPluginState* g_state = nullptr;

void write_producer_registered(StatPluginState* s, uint32_t id, const char* name) {
    std::lock_guard<std::mutex> guard(s->mtx);
    if (!s->file) return;
    std::fprintf(s->file, "P %u\t%s\n", id, name);
    std::fflush(s->file);
}

void write_producer_meta(StatPluginState* s, uint32_t producer_id, const char* key, const char* value) {
    std::lock_guard<std::mutex> guard(s->mtx);
    if (!s->file) return;
    std::fprintf(s->file, "D %u\t%s\t%s\n", producer_id, key, value);
    std::fflush(s->file);
}

void write_stat_meta(StatPluginState* s, uint32_t producer_id, uint32_t stat_id, const char* name) {
    std::lock_guard<std::mutex> guard(s->mtx);
    if (!s->file) return;
    std::fprintf(s->file, "M %u\t%u\t%s\n", producer_id, stat_id, name);
    std::fflush(s->file);
}

void write_stat(StatPluginState* s, uint32_t producer_id, uint32_t stat_id, int64_t value) {
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> guard(s->mtx);
    if (!s->file) return;
    std::fprintf(s->file, "S %u\t%u\t%lld\t%lld\n",
        producer_id, stat_id,
        static_cast<long long>(ns),
        static_cast<long long>(value));
    std::fflush(s->file);
}

void on_event(const PlexdbLogEvent* event, void* ctx) {
    auto* s = static_cast<StatPluginState*>(ctx);
    switch (event->type) {
        case PLEXDB_LOG_PRODUCER_REGISTERED:
            write_producer_registered(s,
                event->producer_registered.producer_id,
                event->producer_registered.name);
            break;
        case PLEXDB_LOG_PRODUCER_META:
            write_producer_meta(s,
                event->producer_meta.producer_id,
                event->producer_meta.key,
                event->producer_meta.value);
            break;
        case PLEXDB_LOG_STAT_META:
            write_stat_meta(s,
                event->stat_meta.producer_id,
                event->stat_meta.stat_id,
                event->stat_meta.name);
            break;
        case PLEXDB_LOG_STAT:
            write_stat(s,
                event->stat.producer_id,
                event->stat.stat_id,
                event->stat.value);
            break;
        default:
            break;
    }
}

StatPluginState* stat_plugin_init() {
    const char* path_cstr = std::getenv("PLEXDB_STAT_FILE");
    std::string path = path_cstr ? std::string(path_cstr) : "plexdb.stats";

    auto* s = new StatPluginState{};
    s->file = std::fopen(path.c_str(), "w");
    return s;
}

void stat_plugin_fini(StatPluginState* s) {
    if (!s) return;
    if (s->file) std::fclose(s->file);
    delete s;
}

} // namespace

__attribute__((constructor))
static void init() {
    g_state = stat_plugin_init();
    plexdb_log_register_consumer(on_event, g_state);
}

__attribute__((destructor))
static void fini() {
    if (!g_state) return;
    plexdb_log_unregister_consumer(on_event, g_state);
    stat_plugin_fini(g_state);
    g_state = nullptr;
}
