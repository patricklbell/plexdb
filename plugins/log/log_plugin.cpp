// Log-file plugin: writes structured plexdb log events to a text file.
// Handles all event types with human-readable formatting.
//
// Configuration (environment variables):
//   PLEXDB_LOG_FILE    – destination path  (default: plexdb.log)
//   PLEXDB_LOG_BATCH   – lines per flush   (default: 1)
//   PLEXDB_LOG_STDERR  – also mirror output to stderr if set
//   PLEXDB_LOG_STDOUT  – also mirror output to stdout if set (ignored if PLEXDB_LOG_STDERR is set)

#include <plexb/plugin_abi/plugin_abi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace {

    std::string current_local_datetime() {
        using namespace std::chrono;

        auto       tz = current_zone();
        zoned_time zt{tz, system_clock::now()};
        return std::format("{:%Y-%m-%d %H:%M:%S}", zt.get_local_time());
    }

    constexpr const char* DEFAULT_PLEXDB_LOG_FILE  = "plexdb.log";
    constexpr size_t      DEFAULT_PLEXDB_LOG_BATCH = 1;

    struct FilePluginState {
        std::unordered_map<uint32_t, std::string>            producers;
        std::map<std::pair<uint32_t, uint32_t>, std::string> stat_names;
        std::vector<std::string>                             buffer;
        std::FILE*                                           file;
        std::FILE*                                           mirror; // stderr or stdout, nullptr if not set
        std::mutex                                           mtx;
        size_t                                               batch;
    };

    FilePluginState* g_state = nullptr;

    // ============================================================================
    // buffer management
    // ============================================================================
    void flush_buffer(FilePluginState* s) {
        if (s->buffer.empty()) {
            return;
        }
        for (const auto& entry : s->buffer) {
            if (s->file) {
                std::fwrite(entry.data(), 1, entry.size(), s->file);
                std::fputc('\n', s->file);
            }
            if (s->mirror) {
                std::fwrite(entry.data(), 1, entry.size(), s->mirror);
                std::fputc('\n', s->mirror);
            }
        }
        if (s->file) {
            std::fflush(s->file);
        }
        if (s->mirror) {
            std::fflush(s->mirror);
        }
        s->buffer.clear();
    }

    void append_line(FilePluginState* s, std::string line) {
        s->buffer.push_back(std::move(line));
        if (s->buffer.size() >= s->batch) {
            flush_buffer(s);
        }
    }

    std::string producer_prefix(FilePluginState* s, uint32_t producer_id) {
        std::string prefix;
        auto        it = s->producers.find(producer_id);
        if (it != s->producers.end()) {
            prefix += '[' + it->second + "] ";
        }
        prefix += '[' + current_local_datetime() + "] ";
        return prefix;
    }

    // ============================================================================
    // event handlers
    // ============================================================================
    void handle_producer_registered(FilePluginState* s, uint32_t id, const char* name) {
        std::lock_guard<std::mutex> guard(s->mtx);
        s->producers[id] = name;
    }

    void handle_producer_meta(FilePluginState* s, uint32_t producer_id, const char* key, const char* value) {
        std::lock_guard<std::mutex> guard(s->mtx);
        std::string                 line = producer_prefix(s, producer_id);
        line += "[META] ";
        line += key;
        line += "=";
        line += value;
        append_line(s, std::move(line));
    }

    void handle_message(FilePluginState* s, uint32_t producer_id, uint32_t level, const char* text, size_t text_len, const char* id, size_t id_len) {
        static constexpr const char* level_tags[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
        constexpr size_t             n_levels     = sizeof(level_tags) / sizeof(level_tags[0]);
        const char*                  tag          = (level < n_levels) ? level_tags[level] : "???";

        std::lock_guard<std::mutex> guard(s->mtx);
        std::string                 line = producer_prefix(s, producer_id);

        line += "[";
        line += tag;

        if (id != nullptr && id_len != 0) {
            line += ":";
            line.append(id, id_len);
            line += +"] ";
        } else {
            line += "] ";
        }

        line.append(text, text_len);
        append_line(s, std::move(line));
    }

    void handle_stat(FilePluginState* s, uint32_t producer_id, uint32_t stat_id, int64_t value) {
        std::lock_guard<std::mutex> guard(s->mtx);
        std::string                 line = producer_prefix(s, producer_id);

        auto sit = s->stat_names.find({producer_id, stat_id});
        if (sit != s->stat_names.end()) {
            line += "[STAT:" + sit->second + "] " + std::to_string(value);
        } else {
            line += "[STAT|" + std::to_string(stat_id) + "] " + std::to_string(value);
        }
        append_line(s, std::move(line));
    }

    void handle_stat_meta(FilePluginState* s, uint32_t producer_id, uint32_t stat_id, const char* name) {
        std::lock_guard<std::mutex> guard(s->mtx);
        s->stat_names[{producer_id, stat_id}] = name;
    }

    // ============================================================================
    // consumer callback
    // ============================================================================
    void on_event(const PlexdbLogEvent* event, void* ctx) {
        auto* s = static_cast<FilePluginState*>(ctx);
        switch (event->type) {
            case PLEXDB_LOG_PRODUCER_REGISTERED:
                handle_producer_registered(s, event->producer_registered.producer_id, event->producer_registered.name);
                break;
            case PLEXDB_LOG_PRODUCER_META:
                handle_producer_meta(s, event->producer_meta.producer_id, event->producer_meta.key, event->producer_meta.value);
                break;
            case PLEXDB_LOG_MESSAGE:
                handle_message(s, event->message.producer_id, event->message.level, event->message.text, event->message.text_len, event->message.message_id, event->message.message_len);
                break;
            case PLEXDB_LOG_STAT:
                handle_stat(s, event->stat.producer_id, event->stat.stat_id, event->stat.value);
                break;
            case PLEXDB_LOG_STAT_META:
                handle_stat_meta(s, event->stat_meta.producer_id, event->stat_meta.stat_id, event->stat_meta.name);
                break;
            default:
                break;
        }
    }

    // ============================================================================
    // lifecycle
    // ============================================================================
    FilePluginState* file_plugin_init() {
        const char* path_cstr = std::getenv("PLEXDB_LOG_FILE");
        std::string path      = (!path_cstr) ? DEFAULT_PLEXDB_LOG_FILE : std::string(path_cstr);

        size_t      batch     = DEFAULT_PLEXDB_LOG_BATCH;
        const char* batch_env = std::getenv("PLEXDB_LOG_BATCH");
        if (batch_env) {
            size_t v = static_cast<size_t>(std::atol(batch_env));
            if (v > 0) {
                batch = v;
            }
        }

        const char* stderr_env = std::getenv("PLEXDB_LOG_STDERR");
        const char* stdout_env = std::getenv("PLEXDB_LOG_STDOUT");
        std::FILE*  mirror     = stderr_env ? stderr : (stdout_env ? stdout : nullptr);

        auto* s   = new FilePluginState{};
        s->file   = std::fopen(path.c_str(), "a");
        s->mirror = mirror;
        s->batch  = batch;
        return s;
    }

    void file_plugin_fini(FilePluginState* s) {
        if (!s) {
            return;
        }
        flush_buffer(s);
        if (s->file) {
            std::fclose(s->file);
        }
        delete s;
    }

}

__attribute__((constructor)) static void init() {
    g_state = file_plugin_init();
    plexdb_plugin_register_consumer(on_event, g_state);
}

__attribute__((destructor)) static void fini() {
    if (!g_state) {
        return;
    }
    plexdb_plugin_unregister_consumer(on_event, g_state);
    file_plugin_fini(g_state);
    g_state = nullptr;
}
