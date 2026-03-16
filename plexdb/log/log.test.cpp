#include <catch2/catch_test_macros.hpp>
#include "log_abi.h"

#include <string>

import plexdb.log;

using namespace plexdb::log;

TEST_CASE("log level to_str", "[plexdb.log]") {
    CHECK(to_str(Level::Trace)[0] == 'T');
    CHECK(to_str(Level::Debug)[0] == 'D');
    CHECK(to_str(Level::Info)[0]  == 'I');
    CHECK(to_str(Level::Warn)[0]  == 'W');
    CHECK(to_str(Level::Error)[0] == 'E');
}

namespace {
    struct TestConsumer {
        unsigned message_count = 0;
        unsigned stat_count    = 0;
        unsigned meta_count    = 0;
        uint32_t last_level    = 0;
        int64_t  last_stat_val = 0;
        uint32_t last_stat_id  = 0;
        uint32_t last_meta_producer = 0;
        uint32_t last_meta_stat_id  = 0;
        const char* last_meta_name  = nullptr;
    };

    void test_on_event(const PlexdbLogEvent* event, void* ctx) {
        auto* c = static_cast<TestConsumer*>(ctx);
        switch (event->type) {
            case PLEXDB_LOG_MESSAGE:
                c->message_count++;
                c->last_level = event->message.level;
                break;
            case PLEXDB_LOG_STAT:
                c->stat_count++;
                c->last_stat_id  = event->stat.stat_id;
                c->last_stat_val = event->stat.value;
                break;
            case PLEXDB_LOG_STAT_META:
                c->meta_count++;
                c->last_meta_producer = event->stat_meta.producer_id;
                c->last_meta_stat_id  = event->stat_meta.stat_id;
                c->last_meta_name     = event->stat_meta.name;
                break;
            default:
                break;
        }
    }
}

TEST_CASE("fire_message with log level", "[plexdb.log]") {
    if constexpr (!enabled) {
        SKIP("logging disabled");
    }

    TestConsumer tc{};
    plexdb_log_register_consumer(test_on_event, &tc);

    Producer p{"test_level"};

    SECTION("default level is Info") {
        fire_message(p.id, "hello", 5);
        CHECK(tc.message_count == 1);
        CHECK(tc.last_level == static_cast<uint32_t>(Level::Info));
    }

    SECTION("explicit Error level") {
        fire_message(p.id, Level::Error, "fail", 4);
        CHECK(tc.message_count == 1);
        CHECK(tc.last_level == static_cast<uint32_t>(Level::Error));
    }

    SECTION("explicit Trace level") {
        fire_message(p.id, Level::Trace, "trace", 5);
        CHECK(tc.message_count == 1);
        CHECK(tc.last_level == static_cast<uint32_t>(Level::Trace));
    }

    plexdb_log_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("fire_stat delivers structured metrics", "[plexdb.log]") {
    if constexpr (!enabled) {
        SKIP("logging disabled");
    }

    TestConsumer tc{};
    plexdb_log_register_consumer(test_on_event, &tc);

    Producer p{"test_stat"};

    fire_stat(p.id, 42, 1000);
    CHECK(tc.stat_count == 1);
    CHECK(tc.last_stat_id == 42);
    CHECK(tc.last_stat_val == 1000);

    fire_stat(p.id, 7, -99);
    CHECK(tc.stat_count == 2);
    CHECK(tc.last_stat_id == 7);
    CHECK(tc.last_stat_val == -99);

    plexdb_log_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("fire_stat_meta delivers metadata", "[plexdb.log]") {
    if constexpr (!enabled) {
        SKIP("logging disabled");
    }

    TestConsumer tc{};
    plexdb_log_register_consumer(test_on_event, &tc);

    Producer p{"test_meta"};

    fire_stat_meta(p.id, 1, "requests_per_sec");
    CHECK(tc.meta_count == 1);
    CHECK(tc.last_meta_producer == p.id);
    CHECK(tc.last_meta_stat_id == 1);
    CHECK(std::string(tc.last_meta_name) == "requests_per_sec");

    fire_stat_meta(p.id, 2, "latency_ns");
    CHECK(tc.meta_count == 2);
    CHECK(tc.last_meta_stat_id == 2);
    CHECK(std::string(tc.last_meta_name) == "latency_ns");

    plexdb_log_unregister_consumer(test_on_event, &tc);
}
