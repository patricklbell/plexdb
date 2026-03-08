// Log-file plugin: writes structured plexdb log events to a text file.
// Handles PLEXDB_LOG_PRODUCER_REGISTERED (records producer names) and
// PLEXDB_LOG_MESSAGE (appends "[producer] text" lines).  Uses STL since
// plugins are not subject to the no-STL rule.
//
// Configuration (environment variables):
//   PLEXDB_LOG_FILE   – destination path  (default: plexdb.log)
//   PLEXDB_LOG_BATCH  – lines per flush   (default: 64)

#include "log_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace {

std::string current_local_datetime() {
    using namespace std::chrono;

    auto tz = current_zone();  
    zoned_time zt{tz, system_clock::now()};
    return std::format("{:%Y-%m-%d %H:%M:%S}", zt.get_local_time());
}

constexpr std::string DEFAULT_PLEXDB_LOG_FILE = "plexdb.log";
constexpr size_t DEFAULT_PLEXDB_LOG_BATCH = 1;

struct State {
    std::unordered_map<uint32_t, std::string> producers;
    std::vector<std::string>                  buffer;
    std::FILE*                                file  = nullptr;
    std::mutex                                mtx;
    size_t                                    batch = DEFAULT_PLEXDB_LOG_BATCH;

    State() {
        const char* path_cstr = std::getenv("PLEXDB_LOG_FILE");
        std::string path = (!path_cstr) ? DEFAULT_PLEXDB_LOG_FILE : std::string(path_cstr);

        const char* batch_env = std::getenv("PLEXDB_LOG_BATCH");
        if (batch_env) {
            size_t v = static_cast<size_t>(std::atol(batch_env));
            if (v > 0) {
                batch = v;
            }
        }

        file = std::fopen(path.c_str(), "a");
    }

    ~State() {
        flush();
        if (file) std::fclose(file);
    }

    void on_producer_registered(uint32_t id, const char* name) {
        std::lock_guard<std::mutex> guard(mtx);
        producers[id] = name;
    }

    void on_message(uint32_t producer_id, const char* text, size_t len) {
        std::time_t t = std::time(nullptr);

        std::lock_guard<std::mutex> guard(mtx);
        std::string line = "";
        auto it = producers.find(producer_id);
        if (it != producers.end()) {
            line += '[' + it->second + "] ";
        }
        line += '[' + current_local_datetime()  + "] ";

        line.append(text, len);
        buffer.push_back(std::move(line));
        if (buffer.size() >= batch) {
            flush_locked();
        }
    }

    void flush() {
        std::lock_guard<std::mutex> guard(mtx);
        flush_locked();
    }

private:
    void flush_locked() {
        if (!file || buffer.empty()) {
            return;
        }

        for (const auto& entry : buffer) {
            std::fwrite(entry.data(), 1, entry.size(), file);
            std::fputc('\n', file);
        }
        std::fflush(file);
        buffer.clear();
    }
};

State* g_state = nullptr;

void on_event(const PlexdbLogEvent* event, void* ctx) {
    auto* state = static_cast<State*>(ctx);
    switch (event->type) {
        case PLEXDB_LOG_PRODUCER_REGISTERED:{
            state->on_producer_registered(
                event->producer_registered.producer_id,
                event->producer_registered.name);
        }break;
        case PLEXDB_LOG_MESSAGE:{
            state->on_message(
                event->message.producer_id,
                event->message.text,
                event->message.text_len);
        }break;
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
    if (!g_state) {
        return;
    }

    plexdb_log_unregister_consumer(on_event, g_state);
    delete g_state;
    g_state = nullptr;
}
