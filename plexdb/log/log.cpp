module;
#include "macros.h"
#include "log_abi.h"

module plexdb.log;

namespace plexdb::log {
    #ifdef PLEXDB_LOG_ENABLED
        constexpr unsigned MAX_CONSUMERS = 16;
        constexpr unsigned MAX_PRODUCERS = 32;

        struct ConsumerEntry {
            PlexdbLogConsumerFn fn;
            void*               ctx;
        };

        struct ProducerInfo {
            uint32_t    id;
            const char* name;
        };

        static ConsumerEntry consumers[MAX_CONSUMERS];
        static unsigned      consumer_count = 0;

        static ProducerInfo  known_producers[MAX_PRODUCERS];
        static unsigned      producer_count = 0;

        // ========================================================================

        void dispatch(const PlexdbLogEvent& event) {
            for (unsigned i = 0; i < consumer_count; ++i)
                consumers[i].fn(&event, consumers[i].ctx);
        }

        // Auto-assigns an ID, stores producer, fires to all current consumers.
        uint32_t internal_register_producer(const char* name) {
            static uint32_t next_id = 1;
            uint32_t id = next_id++;

            if (producer_count < MAX_PRODUCERS)
                known_producers[producer_count++] = {id, name};

            PlexdbLogEvent e{};
            e.type = PLEXDB_LOG_PRODUCER_REGISTERED;
            e.producer_registered = {id, name};
            dispatch(e);
            return id;
        }

        void internal_unregister_producer(uint32_t id) {
            for (unsigned i = 0; i < producer_count; ++i) {
                if (known_producers[i].id == id) {
                    known_producers[i] = known_producers[--producer_count];
                    return;
                }
            }
        }

        // Adds the consumer and replays PLEXDB_LOG_PRODUCER_REGISTERED for every
        // producer registered so far (catch-up). Messages fired before this call
        // are not buffered and will not be replayed — see load-order contract in
        // log_abi.h.
        static void register_consumer(PlexdbLogConsumerFn fn, void* ctx) {
            if (consumer_count >= MAX_CONSUMERS) return;
            consumers[consumer_count++] = {fn, ctx};

            for (unsigned i = 0; i < producer_count; ++i) {
                PlexdbLogEvent e{};
                e.type = PLEXDB_LOG_PRODUCER_REGISTERED;
                e.producer_registered = {known_producers[i].id, known_producers[i].name};
                fn(&e, ctx);
            }
        }

        static void unregister_consumer(PlexdbLogConsumerFn fn, void* ctx) {
            for (unsigned i = 0; i < consumer_count; ++i) {
                if (consumers[i].fn == fn && consumers[i].ctx == ctx) {
                    consumers[i] = consumers[--consumer_count];
                    return;
                }
            }
        }
    #else // !PLEXDB_LOG_ENABLED
        void dispatch(const PlexdbLogEvent&) {}
        uint32_t internal_register_producer(const char*) { return 0; }
        void internal_unregister_producer(uint32_t) {}
    #endif
}

extern "C" {
#ifdef PLEXDB_LOG_ENABLED
    void plexdb_log_register_consumer(PlexdbLogConsumerFn fn, void* ctx) {
        plexdb::log::register_consumer(fn, ctx);
    }
    void plexdb_log_unregister_consumer(PlexdbLogConsumerFn fn, void* ctx) {
        plexdb::log::unregister_consumer(fn, ctx);
    }
#else
    void plexdb_log_register_consumer(PlexdbLogConsumerFn, void*) {}
    void plexdb_log_unregister_consumer(PlexdbLogConsumerFn, void*) {}
#endif
}
