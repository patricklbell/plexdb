module;
#include "macros.h"
#include "log_abi.h"

module plexdb.log;

namespace plexdb::log {
    #ifdef PLEXDB_LOG_ENABLED
        constexpr unsigned MAX_CONSUMERS = 16;
        constexpr unsigned MAX_PRODUCERS = 32;
        constexpr unsigned MAX_STATS     = 64;

        struct ConsumerEntry {
            PlexdbLogConsumerFn fn;
            void*               ctx;
        };

        struct ProducerInfo {
            uint32_t    id;
            const char* name;
        };

        struct StatInfo {
            uint32_t    producer_id;
            uint32_t    stat_id;
            uint32_t    stat_type;
            const char* name;
        };

        static ConsumerEntry consumers[MAX_CONSUMERS];
        static unsigned      consumer_count = 0;

        static ProducerInfo  known_producers[MAX_PRODUCERS];
        static unsigned      producer_count = 0;

        static StatInfo      known_stats[MAX_STATS];
        static unsigned      stat_count = 0;

        // ========================================================================

        void dispatch(const PlexdbLogEvent& event) {
            for (unsigned i = 0; i < consumer_count; ++i)
                consumers[i].fn(&event, consumers[i].ctx);
        }

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

        uint32_t internal_register_stat(uint32_t producer_id, uint32_t stat_type, const char* name) {
            static uint32_t next_stat_id = 1;
            uint32_t sid = next_stat_id++;

            if (stat_count < MAX_STATS)
                known_stats[stat_count++] = {producer_id, sid, stat_type, name};

            PlexdbLogEvent e{};
            e.type = PLEXDB_LOG_STAT_META;
            e.stat_meta = {producer_id, sid, stat_type, name};
            dispatch(e);
            return sid;
        }

        void internal_unregister_stat(uint32_t producer_id, uint32_t stat_id) {
            for (unsigned i = 0; i < stat_count; ++i) {
                if (known_stats[i].producer_id == producer_id && known_stats[i].stat_id == stat_id) {
                    known_stats[i] = known_stats[--stat_count];
                    return;
                }
            }
        }

        // Adds the consumer and replays PLEXDB_LOG_PRODUCER_REGISTERED for every
        // producer and PLEXDB_LOG_STAT_META for every stat registered so far
        // (catch-up).
        static void register_consumer(PlexdbLogConsumerFn fn, void* ctx) {
            if (consumer_count >= MAX_CONSUMERS) return;
            consumers[consumer_count++] = {fn, ctx};

            for (unsigned i = 0; i < producer_count; ++i) {
                PlexdbLogEvent e{};
                e.type = PLEXDB_LOG_PRODUCER_REGISTERED;
                e.producer_registered = {known_producers[i].id, known_producers[i].name};
                fn(&e, ctx);
            }

            for (unsigned i = 0; i < stat_count; ++i) {
                PlexdbLogEvent e{};
                e.type = PLEXDB_LOG_STAT_META;
                e.stat_meta = {known_stats[i].producer_id, known_stats[i].stat_id,
                               known_stats[i].stat_type, known_stats[i].name};
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
        uint32_t internal_register_stat(uint32_t, uint32_t, const char*) { return 0; }
        void internal_unregister_stat(uint32_t, uint32_t) {}
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
