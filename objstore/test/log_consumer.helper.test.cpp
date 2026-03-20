#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include "log_abi.h"

#include <map>
#include <string>
#include <vector>
#include <mutex>

namespace {

// ============================================================================
// log buffer state — plain struct, manipulated by free functions
// ============================================================================
struct LogBuffer {
    std::vector<std::string>                                   entries;
    std::map<std::pair<uint32_t,uint32_t>, std::string>        stat_names;
    std::map<uint32_t, std::string>                            producer_names;
    std::map<std::pair<uint32_t,std::string>, std::string>     producer_meta;
    std::mutex                                                 mtx;
    bool                                                       active = false;
};

LogBuffer g_log_buffer;

void log_buffer_clear(LogBuffer& lb) {
    std::lock_guard<std::mutex> g(lb.mtx);
    lb.entries.clear();
}

void log_buffer_push_message(LogBuffer& lb, const char* level, const char* text, size_t len) {
    std::lock_guard<std::mutex> g(lb.mtx);
    if (!lb.active) return;
    std::string entry = "[";
    entry += level;
    entry += "] ";
    entry.append(text, len);
    lb.entries.push_back(std::move(entry));
    UNSCOPED_INFO(lb.entries.back());
}

void log_buffer_push_stat(LogBuffer& lb, uint32_t producer_id, uint32_t stat_id, int64_t value) {
    std::lock_guard<std::mutex> g(lb.mtx);
    if (!lb.active) return;
    auto it = lb.stat_names.find({producer_id, stat_id});
    std::string entry;
    if (it != lb.stat_names.end()) {
        entry = "[STAT:" + it->second + "] " + std::to_string(value);
    } else {
        entry = "[STAT] id=" + std::to_string(stat_id) + " value=" + std::to_string(value);
    }
    lb.entries.push_back(std::move(entry));
    UNSCOPED_INFO(lb.entries.back());
}

void log_buffer_register_stat_name(LogBuffer& lb, uint32_t producer_id, uint32_t stat_id, const char* name) {
    std::lock_guard<std::mutex> g(lb.mtx);
    lb.stat_names[{producer_id, stat_id}] = name;
}

void log_buffer_register_producer(LogBuffer& lb, uint32_t id, const char* name) {
    std::lock_guard<std::mutex> g(lb.mtx);
    lb.producer_names[id] = name;
    if (lb.active) {
        std::string entry = "[PRODUCER] " + std::string(name) + " (id=" + std::to_string(id) + ")";
        lb.entries.push_back(std::move(entry));
        UNSCOPED_INFO(lb.entries.back());
    }
}

void log_buffer_register_producer_meta(LogBuffer& lb, uint32_t producer_id, const char* key, const char* value) {
    std::lock_guard<std::mutex> g(lb.mtx);
    lb.producer_meta[{producer_id, std::string(key)}] = value;
    if (lb.active) {
        std::string entry = "[PRODUCER_META] id=" + std::to_string(producer_id)
                          + " " + key + "=" + value;
        lb.entries.push_back(std::move(entry));
        UNSCOPED_INFO(lb.entries.back());
    }
}

// ============================================================================
// level string
// ============================================================================
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

// ============================================================================
// consumer callback
// ============================================================================
void on_log_event(const PlexdbLogEvent* event, void* /*ctx*/) {
    switch (event->type) {
        case PLEXDB_LOG_PRODUCER_REGISTERED:
            log_buffer_register_producer(
                g_log_buffer,
                event->producer_registered.producer_id,
                event->producer_registered.name);
            break;
        case PLEXDB_LOG_MESSAGE:
            log_buffer_push_message(
                g_log_buffer,
                level_str(event->message.level),
                event->message.text,
                event->message.text_len);
            break;
        case PLEXDB_LOG_STAT:
            log_buffer_push_stat(
                g_log_buffer,
                event->stat.producer_id,
                event->stat.stat_id,
                event->stat.value);
            break;
        case PLEXDB_LOG_STAT_META:
            log_buffer_register_stat_name(
                g_log_buffer,
                event->stat_meta.producer_id,
                event->stat_meta.stat_id,
                event->stat_meta.name);
            break;
        case PLEXDB_LOG_PRODUCER_META:
            log_buffer_register_producer_meta(
                g_log_buffer,
                event->producer_meta.producer_id,
                event->producer_meta.key,
                event->producer_meta.value);
            break;
        default:
            break;
    }
}

// ============================================================================
// Catch2 listener — activates/deactivates log capture per test case
// ============================================================================
struct LogListener : Catch::EventListenerBase {
    using EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        g_log_buffer.active = true;
        log_buffer_clear(g_log_buffer);
    }
    void testCaseEnded(Catch::TestCaseStats const&) override {
        g_log_buffer.active = false;
        log_buffer_clear(g_log_buffer);
    }
};
CATCH_REGISTER_LISTENER(LogListener)

struct LogConsumerInstaller {
    LogConsumerInstaller()  { plexdb_log_register_consumer(on_log_event, nullptr); }
    ~LogConsumerInstaller() { plexdb_log_unregister_consumer(on_log_event, nullptr); }
};
static LogConsumerInstaller g_log_install;

} // namespace
