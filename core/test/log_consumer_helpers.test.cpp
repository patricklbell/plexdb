module;
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <plexb/plugin_abi/plugin_abi.h>

#include <map>
#include <string>
#include <vector>
#include <mutex>

module plexdb.test.log_consumer_helpers;

namespace {
    struct LogBuffer {
        std::map<std::pair<uint32_t, uint32_t>, std::string>    stat_names;
        std::map<uint32_t, std::string>                         producer_names;
        std::map<std::pair<uint32_t, std::string>, std::string> producer_meta;
        std::mutex                                              mtx;
        bool                                                    active = false;
    };

    LogBuffer g_log_buffer;

    void log_buffer_push_message(LogBuffer& lb, const char* level, const char* text, size_t len) {
        std::lock_guard<std::mutex> g(lb.mtx);
        if (!lb.active) {
            return;
        }
        std::string entry = "[";
        entry += level;
        entry += "] ";
        entry.append(text, len);
        UNSCOPED_INFO(entry);
    }

    void log_buffer_push_stat(LogBuffer& lb, uint32_t producer_id, uint32_t stat_id, int64_t value) {
        std::lock_guard<std::mutex> g(lb.mtx);
        if (!lb.active) {
            return;
        }
        auto        it = lb.stat_names.find({producer_id, stat_id});
        std::string entry;
        if (it != lb.stat_names.end()) {
            entry = "[STAT:" + it->second + "] " + std::to_string(value);
        } else {
            entry = "[STAT] id=" + std::to_string(stat_id) + " value=" + std::to_string(value);
        }
        UNSCOPED_INFO(entry);
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
            UNSCOPED_INFO(entry);
        }
    }

    void log_buffer_register_producer_meta(LogBuffer& lb, uint32_t producer_id, const char* key, const char* value) {
        std::lock_guard<std::mutex> g(lb.mtx);
        lb.producer_meta[{producer_id, std::string(key)}] = value;
        if (lb.active) {
            std::string entry = "[PRODUCER_META] id=" + std::to_string(producer_id) + " " + key + "=" + value;
            UNSCOPED_INFO(entry);
        }
    }

    const char* level_str(uint32_t lvl) {
        switch (lvl) {
            case PLEXDB_LOG_TRACE:
                return "TRACE";
            case PLEXDB_LOG_DEBUG:
                return "DEBUG";
            case PLEXDB_LOG_INFO:
                return "INFO";
            case PLEXDB_LOG_WARN:
                return "WARN";
            case PLEXDB_LOG_ERROR:
                return "ERROR";
            default:
                return "???";
        }
    }

    void on_log_event(const PlexdbLogEvent* event, void* /*ctx*/) {
        switch (event->type) {
            case PLEXDB_LOG_PRODUCER_REGISTERED:
                log_buffer_register_producer(
                    g_log_buffer,
                    event->producer_registered.producer_id,
                    event->producer_registered.name
                );
                break;
            case PLEXDB_LOG_MESSAGE:
                log_buffer_push_message(
                    g_log_buffer,
                    level_str(event->message.level),
                    event->message.text,
                    event->message.text_len
                );
                break;
            case PLEXDB_LOG_STAT:
                log_buffer_push_stat(
                    g_log_buffer,
                    event->stat.producer_id,
                    event->stat.stat_id,
                    event->stat.value
                );
                break;
            case PLEXDB_LOG_STAT_META:
                log_buffer_register_stat_name(
                    g_log_buffer,
                    event->stat_meta.producer_id,
                    event->stat_meta.stat_id,
                    event->stat_meta.name
                );
                break;
            case PLEXDB_LOG_PRODUCER_META:
                log_buffer_register_producer_meta(
                    g_log_buffer,
                    event->producer_meta.producer_id,
                    event->producer_meta.key,
                    event->producer_meta.value
                );
                break;
            default:
                break;
        }
    }

    struct LogListener : Catch::EventListenerBase {
        using EventListenerBase::EventListenerBase;

        void testCaseStarting(Catch::TestCaseInfo const&) override {
            g_log_buffer.active = true;
        }
        void testCaseEnded(Catch::TestCaseStats const&) override {
            g_log_buffer.active = false;
        }
    };
    CATCH_REGISTER_LISTENER(LogListener)
}

void register_test_log_consumer() {
    plexdb_plugin_register_consumer(on_log_event, nullptr);
}
void unregister_test_log_consumer() {
    plexdb_plugin_unregister_consumer(on_log_event, nullptr);
}
