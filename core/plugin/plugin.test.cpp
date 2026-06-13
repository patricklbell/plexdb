#include <catch2/catch_test_macros.hpp>
#include <plexb/plugin_abi/plugin_abi.h>

import plexdb.base;
import plexdb.plugin;

using namespace plexdb;
using namespace plexdb::plugin;

TEST_CASE("log level to_str", "[plexdb.plugin]") {
    CHECK(to_str(Level::Trace)[0] == 'T');
    CHECK(to_str(Level::Debug)[0] == 'D');
    CHECK(to_str(Level::Info)[0] == 'I');
    CHECK(to_str(Level::Warn)[0] == 'W');
    CHECK(to_str(Level::Error)[0] == 'E');
}

namespace {
    struct TestConsumer {
        unsigned    message_count             = 0;
        unsigned    stat_count                = 0;
        unsigned    meta_count                = 0;
        unsigned    producer_registered_count = 0;
        uint32_t    last_level                = 0;
        int64_t     last_stat_val             = 0;
        uint32_t    last_stat_id              = 0;
        uint32_t    last_meta_producer        = 0;
        uint32_t    last_meta_stat_id         = 0;
        uint32_t    last_meta_stat_type       = 0;
        const char* last_meta_name            = nullptr;
        const char* last_producer_name        = nullptr;
        const char* last_message_id           = nullptr;
    };

    void test_on_event(const PlexdbLogEvent* event, void* ctx) {
        auto* c = static_cast<TestConsumer*>(ctx);
        switch (event->type) {
            case PLEXDB_LOG_PRODUCER_REGISTERED:
                c->producer_registered_count++;
                c->last_producer_name = event->producer_registered.name;
                break;
            case PLEXDB_LOG_MESSAGE:
                c->message_count++;
                c->last_level      = event->message.level;
                c->last_message_id = event->message.message_id;
                break;
            case PLEXDB_LOG_STAT:
                c->stat_count++;
                c->last_stat_id  = event->stat.stat_id;
                c->last_stat_val = event->stat.value;
                break;
            case PLEXDB_LOG_STAT_META:
                c->meta_count++;
                c->last_meta_producer  = event->stat_meta.producer_id;
                c->last_meta_stat_id   = event->stat_meta.stat_id;
                c->last_meta_stat_type = event->stat_meta.stat_type;
                c->last_meta_name      = event->stat_meta.name;
                break;
            default:
                break;
        }
    }
}

TEST_CASE("message with log level", "[plexdb.plugin]") {
    if constexpr (!enabled) {
        return;
    }

    TestConsumer tc{};
    plexdb_plugin_register_consumer(test_on_event, &tc);

    Producer p{"test_level"};

    SECTION("default level is Info") {
        message(p, "hello");
        CHECK(tc.message_count == 1);
        CHECK(tc.last_level == static_cast<uint32_t>(Level::Info));
    }

    SECTION("explicit Error level") {
        message(p, Level::Error, "fail");
        CHECK(tc.message_count == 1);
        CHECK(tc.last_level == static_cast<uint32_t>(Level::Error));
    }

    SECTION("explicit Trace level") {
        message(p, Level::Trace, "trace");
        CHECK(tc.message_count == 1);
        CHECK(tc.last_level == static_cast<uint32_t>(Level::Trace));
    }

    plexdb_plugin_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("stat delivers structured metrics", "[plexdb.plugin]") {
    if constexpr (!enabled) {
        return;
    }

    TestConsumer tc{};
    plexdb_plugin_register_consumer(test_on_event, &tc);

    Producer p{"test_stat"};
    Stat     s1{&p, "requests"};
    Stat     s2{&p, "latency"};

    stat(s1, 1000);
    CHECK(tc.stat_count == 1);
    CHECK(tc.last_stat_id == s1.id);
    CHECK(tc.last_stat_val == 1000);

    stat(s2, -99);
    CHECK(tc.stat_count == 2);
    CHECK(tc.last_stat_id == s2.id);
    CHECK(tc.last_stat_val == -99);

    plexdb_plugin_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("stat registration fires stat_meta", "[plexdb.plugin]") {
    if constexpr (!enabled) {
        return;
    }

    TestConsumer tc{};
    plexdb_plugin_register_consumer(test_on_event, &tc);

    Producer p{"test_meta"};
    Stat     s{&p, "requests_per_sec"};

    auto meta_before = tc.meta_count;
    stat(s, 42);
    CHECK(tc.meta_count == meta_before + 1);
    CHECK(tc.last_meta_producer == p.id);
    CHECK(tc.last_meta_stat_id == s.id);
    CHECK(String8(tc.last_meta_name) == "requests_per_sec");
    CHECK(tc.last_meta_stat_type == PLEXDB_STAT_GAUGE);

    plexdb_plugin_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("stat type counter and gauge", "[plexdb.plugin]") {
    if constexpr (!enabled) {
        return;
    }

    TestConsumer tc{};
    plexdb_plugin_register_consumer(test_on_event, &tc);

    Producer p{"test_types"};
    Stat     counter{&p, "total_requests", StatType::Counter};
    Stat     gauge{&p, "active_connections", StatType::Gauge};

    stat(counter, 5);
    CHECK(tc.meta_count >= 1);
    CHECK(tc.last_meta_stat_type == PLEXDB_STAT_COUNTER);
    CHECK(String8(tc.last_meta_name) == "total_requests");

    stat(gauge, 10);
    CHECK(tc.last_meta_stat_type == PLEXDB_STAT_GAUGE);
    CHECK(String8(tc.last_meta_name) == "active_connections");

    plexdb_plugin_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("lazy producer registration", "[plexdb.plugin]") {
    if constexpr (!enabled) {
        return;
    }

    Producer p{"lazy_producer"};
    CHECK(p.id == 0);

    TestConsumer tc{};
    plexdb_plugin_register_consumer(test_on_event, &tc);

    message(p, "hello");
    CHECK(p.id != 0);
    CHECK(tc.message_count == 1);

    plexdb_plugin_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("message with message_id", "[plexdb.plugin]") {
    if constexpr (!enabled) {
        return;
    }

    TestConsumer tc{};
    plexdb_plugin_register_consumer(test_on_event, &tc);

    Producer p{"test_msg_id"};

    SECTION("message_id is null by default") {
        message(p, Level::Info, "hello");
        CHECK(tc.message_count == 1);
        CHECK(tc.last_message_id == nullptr);
    }

    SECTION("message_id is propagated") {
        message(p, Level::Debug, "SELECT 1", "query.text");
        CHECK(tc.message_count == 1);
        CHECK(tc.last_level == static_cast<uint32_t>(Level::Debug));
        CHECK(String8(tc.last_message_id) == "query.text");
    }

    plexdb_plugin_unregister_consumer(test_on_event, &tc);
}

TEST_CASE("stat and producer catch-up on consumer registration", "[plexdb.plugin]") {
    if constexpr (!enabled) {
        return;
    }

    Producer p{"catchup_producer"};
    Stat     s{&p, "catchup_stat"};

    // Force registration before consumer exists
    stat(s, 100);
    CHECK(p.id != 0);
    CHECK(s.id != 0);

    // Register consumer after producer+stat already registered
    TestConsumer tc{};
    plexdb_plugin_register_consumer(test_on_event, &tc);

    // Catch-up should replay producer registration and stat meta
    CHECK(tc.producer_registered_count >= 1);
    CHECK(tc.meta_count >= 1);

    plexdb_plugin_unregister_consumer(test_on_event, &tc);
}
