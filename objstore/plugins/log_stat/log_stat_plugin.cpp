// Stat-logging plugin: writes structured stat events to a line-oriented text file.
// Designed for consumption by extra/plot_stats.py.
//
// Output format (one line per event, tab-separated):
//   P <producer_id>\t<name>                   — producer registration
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

struct State {
    std::FILE* file = nullptr;
    std::mutex mtx;

    State() {
        const char* path_cstr = std::getenv("PLEXDB_STAT_FILE");
        std::string path = path_cstr ? std::string(path_cstr) : "plexdb.stats";
        file = std::fopen(path.c_str(), "w");
    }

    ~State() {
        if (file) std::fclose(file);
    }

    void on_producer_registered(uint32_t id, const char* name) {
        std::lock_guard<std::mutex> guard(mtx);
        if (!file) return;
        std::fprintf(file, "P %u\t%s\n", id, name);
        std::fflush(file);
    }

    void on_stat_meta(uint32_t producer_id, uint32_t stat_id, const char* name) {
        std::lock_guard<std::mutex> guard(mtx);
        if (!file) return;
        std::fprintf(file, "M %u\t%u\t%s\n", producer_id, stat_id, name);
        std::fflush(file);
    }

    void on_stat(uint32_t producer_id, uint32_t stat_id, int64_t value) {
        using namespace std::chrono;
        auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();

        std::lock_guard<std::mutex> guard(mtx);
        if (!file) return;
        std::fprintf(file, "S %u\t%u\t%lld\t%lld\n",
            producer_id, stat_id,
            static_cast<long long>(ns),
            static_cast<long long>(value));
        std::fflush(file);
    }
};

State* g_state = nullptr;

void on_event(const PlexdbLogEvent* event, void* ctx) {
    auto* state = static_cast<State*>(ctx);
    switch (event->type) {
        case PLEXDB_LOG_PRODUCER_REGISTERED:
            state->on_producer_registered(
                event->producer_registered.producer_id,
                event->producer_registered.name);
            break;
        case PLEXDB_LOG_STAT_META:
            state->on_stat_meta(
                event->stat_meta.producer_id,
                event->stat_meta.stat_id,
                event->stat_meta.name);
            break;
        case PLEXDB_LOG_STAT:
            state->on_stat(
                event->stat.producer_id,
                event->stat.stat_id,
                event->stat.value);
            break;
        default:
            break;
    }
}

} // namespace

__attribute__((constructor))
static void init() {
    g_state = new State();
    plexdb_log_register_consumer(on_event, g_state);
}

__attribute__((destructor))
static void fini() {
    if (!g_state) return;
    plexdb_log_unregister_consumer(on_event, g_state);
    delete g_state;
    g_state = nullptr;
}
