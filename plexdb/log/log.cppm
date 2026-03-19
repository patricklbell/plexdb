module;
#include "log_abi.h"

export module plexdb.log;

namespace plexdb::log {
    uint32_t internal_register_producer(const char* name);
    void     internal_unregister_producer(uint32_t id);
    void     dispatch(const PlexdbLogEvent& event);
}

export namespace plexdb::log {
    inline constexpr bool enabled =
#ifdef PLEXDB_LOG_ENABLED
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
    //   registers itself on construction, deregisters on destruction.
    //   The ID is auto-assigned by register_producer to avoid collisions.
    //   When disabled, construction/destruction are no-ops and id is 0.
    // ========================================================================
    class Producer {
    public:
        uint32_t id;

        Producer(const char* name) : id(0) {
            if constexpr (enabled)
                id = internal_register_producer(name);
        }
        ~Producer() {
            if constexpr (enabled)
                internal_unregister_producer(id);
        }

        Producer(const Producer&)            = delete;
        Producer& operator=(const Producer&) = delete;
    };

    // ========================================================================
    // messaging
    //   zero-overhead when disabled via if constexpr.
    // ========================================================================
    inline void fire_message(uint32_t producer_id, Level level, const char* text, size_t text_len) {
        if constexpr (enabled) {
            PlexdbLogEvent e{};
            e.type    = PLEXDB_LOG_MESSAGE;
            e.message = {producer_id, static_cast<uint32_t>(level), text, text_len};
            dispatch(e);
        }
    }

    inline void fire_message(uint32_t producer_id, const char* text, size_t text_len) {
        fire_message(producer_id, Level::Info, text, text_len);
    }

    // ========================================================================
    // stats
    //   zero-overhead structured numeric events for realtime metrics.
    //   no string formatting overhead — consumers read the value directly.
    // ========================================================================
    inline void fire_stat_meta(uint32_t producer_id, uint32_t stat_id, const char* name) {
        if constexpr (enabled) {
            PlexdbLogEvent e{};
            e.type = PLEXDB_LOG_STAT_META;
            e.stat_meta = {producer_id, stat_id, name};
            dispatch(e);
        }
    }

    inline void fire_stat(uint32_t producer_id, uint32_t stat_id, int64_t value) {
        if constexpr (enabled) {
            PlexdbLogEvent e{};
            e.type = PLEXDB_LOG_STAT;
            e.stat = {producer_id, stat_id, value};
            dispatch(e);
        }
    }

    inline void fire_producer_meta(uint32_t producer_id, const char* key, const char* value) {
        if constexpr (enabled) {
            PlexdbLogEvent e{};
            e.type = PLEXDB_LOG_PRODUCER_META;
            e.producer_meta = {producer_id, key, value};
            dispatch(e);
        }
    }
}
