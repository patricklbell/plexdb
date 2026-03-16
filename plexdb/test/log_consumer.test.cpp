#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include "log_abi.h"

#include <string>
#include <vector>
#include <mutex>

namespace {

struct LogBuffer {
    std::vector<std::string> entries;
    std::mutex               mtx;
    bool                     active = false;

    void clear() {
        std::lock_guard<std::mutex> g(mtx);
        entries.clear();
    }

    void push(const char* level, const char* text, size_t len) {
        std::lock_guard<std::mutex> g(mtx);
        if (!active) return;
        std::string entry = "[";
        entry += level;
        entry += "] ";
        entry.append(text, len);
        entries.push_back(std::move(entry));
        UNSCOPED_INFO(entries.back());
    }

    void push_stat(uint32_t stat_id, int64_t value) {
        std::lock_guard<std::mutex> g(mtx);
        if (!active) return;
        std::string entry = "[STAT] id=" + std::to_string(stat_id) + " value=" + std::to_string(value);
        entries.push_back(std::move(entry));
        UNSCOPED_INFO(entries.back());
    }
};

LogBuffer g_log_buffer;

const char* level_str(uint32_t lvl) {
    switch (lvl) {
        case PLEXDB_LOG_TRACE: return "TRACE";
        case PLEXDB_LOG_DEBUG: return "DEBUG";
        case PLEXDB_LOG_INFO:  return "INFO";
        case PLEXDB_LOG_WARN:  return "WARN";
        case PLEXDB_LOG_ERROR: return "ERROR";
        default:               return "???";
    }
}

void on_log_event(const PlexdbLogEvent* event, void* /*ctx*/) {
    switch (event->type) {
        case PLEXDB_LOG_MESSAGE:
            g_log_buffer.push(
                level_str(event->message.level),
                event->message.text,
                event->message.text_len);
            break;
        case PLEXDB_LOG_STAT:
            g_log_buffer.push_stat(
                event->stat.stat_id,
                event->stat.value);
            break;
        default:
            break;
    }
}

struct LogListener : Catch::EventListenerBase {
    using EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        g_log_buffer.active = true;
        g_log_buffer.clear();
    }
    void testCaseEnded(Catch::TestCaseStats const&) override {
        g_log_buffer.active = false;
        g_log_buffer.clear();
    }
};
CATCH_REGISTER_LISTENER(LogListener)

struct LogConsumerInstaller {
    LogConsumerInstaller()  { plexdb_log_register_consumer(on_log_event, nullptr); }
    ~LogConsumerInstaller() { plexdb_log_unregister_consumer(on_log_event, nullptr); }
};
static LogConsumerInstaller g_install;

} // namespace
