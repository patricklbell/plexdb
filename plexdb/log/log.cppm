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
    inline void fire_message(uint32_t producer_id, const char* text, size_t text_len) {
        if constexpr (enabled) {
            PlexdbLogEvent e{};
            e.type    = PLEXDB_LOG_MESSAGE;
            e.message = {producer_id, 0, text, text_len};
            dispatch(e);
        }
    }
}
