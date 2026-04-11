module;
#include "log_abi.h"

export module plexdb.log;

import plexdb.base.string;

namespace plexdb::log {
    uint32_t internal_register_producer(const char* name);
    void     internal_unregister_producer(uint32_t id);
    uint32_t internal_register_stat(uint32_t producer_id, uint32_t stat_type, const char* name);
    void     internal_unregister_stat(uint32_t producer_id, uint32_t stat_id);
    void     dispatch(const PlexdbLogEvent& event);
}

export namespace plexdb::log {
    inline constexpr bool enabled =
#ifdef PLEXDB_ENABLE_LOGGING
        true;
#else
        false;
#endif

    // ========================================================================
    // log levels
    // ========================================================================
    enum class Level : uint32_t {
        Trace = PLEXDB_LOG_TRACE,
        Debug = PLEXDB_LOG_DEBUG,
        Info  = PLEXDB_LOG_INFO,
        Warn  = PLEXDB_LOG_WARN,
        Error = PLEXDB_LOG_ERROR,
    };

    inline constexpr const char* to_str(Level lvl) {
        switch (lvl) {
            case Level::Trace: return "TRACE";
            case Level::Debug: return "DEBUG";
            case Level::Info:  return "INFO";
            case Level::Warn:  return "WARN";
            case Level::Error: return "ERROR";
            default:           return "???";
        }
    }

    // ========================================================================
    // producer
    //   @note lazy registration: ID is assigned on first use, not at construction.
    //   Construction order between producers and stats does not matter.
    //   When a consumer registers, all known producers are replayed (catch-up).
    // ========================================================================
    class Producer {
    public:
        mutable uint32_t id;
        const char* name;

        Producer(const char* name) : id(0), name(name) {}
        ~Producer() {
            if constexpr (enabled)
                if (id) internal_unregister_producer(id);
        }

        Producer(const Producer&)            = delete;
        Producer& operator=(const Producer&) = delete;
    };

    inline void ensure_registered(const Producer& p) {
        if constexpr (enabled) {
            if (p.id == 0)
                p.id = internal_register_producer(p.name);
        }
    }

    // ========================================================================
    // stat types
    // ========================================================================
    enum class StatType : uint32_t {
        Counter = PLEXDB_STAT_COUNTER,  // monotonically increasing cumulative value
        Gauge   = PLEXDB_STAT_GAUGE,    // point-in-time measurement
    };

    // ========================================================================
    // stat
    //   @note lazy registration: producer and stat are both registered on first fire.
    //   Construction order does not matter.
    //   When a consumer registers, all known stats are replayed (catch-up).
    // ========================================================================
    class Stat {
    public:
        const Producer* producer;
        mutable uint32_t id;
        const char* name;
        StatType type;

        Stat(const Producer* producer, const char* name, StatType type = StatType::Gauge)
            : producer(producer), id(0), name(name), type(type) {}
        ~Stat() {
            if constexpr (enabled)
                if (id) internal_unregister_stat(producer->id, id);
        }

        Stat(const Stat&)            = delete;
        Stat& operator=(const Stat&) = delete;
    };

    inline void ensure_registered(const Stat& s) {
        if constexpr (enabled) {
            ensure_registered(*s.producer);
            if (s.id == 0)
                s.id = internal_register_stat(s.producer->id, static_cast<uint32_t>(s.type), s.name);
        }
    }

    // ========================================================================
    // messaging
    //   zero-overhead when disabled via if constexpr.
    // ========================================================================
    inline void message(const Producer& producer, Level level, const String8& text) {
        if constexpr (enabled) {
            ensure_registered(producer);
            PlexdbLogEvent e{};
            e.type    = PLEXDB_LOG_MESSAGE;
            e.message = {producer.id, static_cast<uint32_t>(level), text.data, text.length, nullptr, 0};
            dispatch(e);
        }
    }

    inline void message(const Producer& producer, Level level, const String8& text, const String8& message_id) {
        if constexpr (enabled) {
            ensure_registered(producer);
            PlexdbLogEvent e{};
            e.type    = PLEXDB_LOG_MESSAGE;
            e.message = {producer.id, static_cast<uint32_t>(level), text.data, text.length, message_id.data, message_id.length};
            dispatch(e);
        }
    }

    inline void message(const Producer& producer, const String8& text) {
        message(producer, Level::Info, text);
    }

    // ========================================================================
    // stats
    //   zero-overhead structured numeric events for realtime metrics.
    //   no string formatting overhead — consumers read the value directly.
    // ========================================================================
    inline void stat(const Stat& s, int64_t value) {
        if constexpr (enabled) {
            ensure_registered(s);
            PlexdbLogEvent e{};
            e.type = PLEXDB_LOG_STAT;
            e.stat = {s.producer->id, s.id, value};
            dispatch(e);
        }
    }

}
