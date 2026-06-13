// OpenTelemetry plugin: exports plexdb stats as OTLP/gRPC metrics
// and MESSAGE events with message_id as OTLP log records.
//
// Stat types:
//   Counter (PLEXDB_STAT_COUNTER) — monotonically increasing, deltas accumulated
//   Gauge   (PLEXDB_STAT_GAUGE)   — point-in-time, latest value reported
//
// Message events with a non-null message_id are emitted as log records with
// the producer scope and message_id forming the OTel attribute name
// (e.g. producer "otlp.db" + message_id "query.text" → "db.query.text").
//
// Configuration (environment variables):
//   PLEXDB_OTLP_ENDPOINT    – gRPC endpoint     (default: localhost:4317)
//   PLEXDB_OTLP_INTERVAL_MS – export interval ms (default: 10000)
//   PLEXDB_OTLP_SERVICE     – service.name attr  (default: plexdb)

#include <plexb/plugin_abi/plugin_abi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h>
#include <opentelemetry/context/context.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/severity.h>
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/sdk/logs/logger_provider_factory.h>
#include <opentelemetry/sdk/logs/simple_log_record_processor_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/resource/resource.h>

namespace metrics_sdk  = opentelemetry::sdk::metrics;
namespace metrics_api  = opentelemetry::metrics;
namespace logs_sdk     = opentelemetry::sdk::logs;
namespace logs_api     = opentelemetry::logs;
namespace otlp         = opentelemetry::exporter::otlp;
namespace resource_sdk = opentelemetry::sdk::resource;

namespace {

    // ============================================================================
    // naming — strip "otlp." prefix for OTel semantic convention names
    // ============================================================================
    std::string otel_scope(const std::string& producer_name) {
        constexpr std::string_view prefix = "otlp.";
        if (producer_name.starts_with(prefix)) {
            return producer_name.substr(prefix.size());
        }
        return producer_name;
    }

    // ============================================================================
    // severity mapping
    // ============================================================================
    logs_api::Severity to_severity(uint32_t level) {
        switch (level) {
            case PLEXDB_LOG_TRACE:
                return logs_api::Severity::kTrace;
            case PLEXDB_LOG_DEBUG:
                return logs_api::Severity::kDebug;
            case PLEXDB_LOG_INFO:
                return logs_api::Severity::kInfo;
            case PLEXDB_LOG_WARN:
                return logs_api::Severity::kWarn;
            case PLEXDB_LOG_ERROR:
                return logs_api::Severity::kError;
            default:
                return logs_api::Severity::kInfo;
        }
    }

    // ============================================================================
    // metric storage
    // ============================================================================
    struct MetricKey {
        uint32_t producer_id;
        uint32_t stat_id;
        bool     operator<(const MetricKey& o) const {
            if (producer_id != o.producer_id) {
                return producer_id < o.producer_id;
            }
            return stat_id < o.stat_id;
        }
    };

    struct Instrument {
        uint32_t                        stat_type;
        metrics_api::Counter<uint64_t>* counter = nullptr;
        metrics_api::Gauge<int64_t>*    gauge   = nullptr;
    };

    // ============================================================================
    // state
    // ============================================================================
    struct OtelState {
        std::shared_ptr<metrics_sdk::MeterProvider> meter_provider;
        std::shared_ptr<logs_sdk::LoggerProvider>   logger_provider;

        opentelemetry::nostd::shared_ptr<metrics_api::Meter> meter;
        opentelemetry::nostd::shared_ptr<logs_api::Logger>   logger;

        std::mutex                      mtx;
        std::map<uint32_t, std::string> producers;
        std::map<MetricKey, Instrument> instruments;

        // own the instrument unique_ptrs to keep them alive
        std::vector<opentelemetry::nostd::unique_ptr<metrics_api::Counter<uint64_t>>> owned_counters;
        std::vector<opentelemetry::nostd::unique_ptr<metrics_api::Gauge<int64_t>>>    owned_gauges;
    };

    OtelState* g_state = nullptr;

    // ============================================================================
    // event handlers
    // ============================================================================
    void handle_producer_registered(OtelState* s, uint32_t id, const char* name) {
        std::lock_guard<std::mutex> guard(s->mtx);
        s->producers[id] = name;
    }

    void handle_stat_meta(OtelState* s, uint32_t pid, uint32_t sid,
                          uint32_t stat_type, const char* name) {
        std::lock_guard<std::mutex> guard(s->mtx);
        MetricKey                   key{pid, sid};
        if (s->instruments.count(key)) {
            return;
        }

        auto        it        = s->producers.find(pid);
        std::string scope     = (it != s->producers.end()) ? otel_scope(it->second) : "unknown";
        std::string full_name = scope + "." + name;

        Instrument inst{stat_type};
        if (stat_type == PLEXDB_STAT_COUNTER) {
            auto handle  = s->meter->CreateUInt64Counter(full_name);
            inst.counter = handle.get();
            s->owned_counters.push_back(std::move(handle));
        } else {
            auto handle = s->meter->CreateInt64Gauge(full_name);
            inst.gauge  = handle.get();
            s->owned_gauges.push_back(std::move(handle));
        }
        s->instruments[key] = inst;
    }

    void handle_stat(OtelState* s, uint32_t pid, uint32_t sid, int64_t value) {
        std::lock_guard<std::mutex> guard(s->mtx);
        MetricKey                   key{pid, sid};
        auto                        it = s->instruments.find(key);
        if (it == s->instruments.end()) {
            return;
        }

        auto& inst = it->second;
        if (inst.stat_type == PLEXDB_STAT_COUNTER) {
            inst.counter->Add(static_cast<uint64_t>(value));
        } else {
            inst.gauge->Record(value);
        }
    }

    void handle_message(OtelState* s, uint32_t pid, uint32_t level,
                        const char* text, size_t text_len, const char* message_id) {
        if (!message_id) {
            return;
        }

        std::string producer_name;
        {
            std::lock_guard<std::mutex> guard(s->mtx);
            auto                        it = s->producers.find(pid);
            if (it != s->producers.end()) {
                producer_name = it->second;
            }
        }

        std::string scope     = otel_scope(producer_name);
        std::string attr_name = scope + "." + message_id;
        std::string body{text, text_len};

        auto record = s->logger->CreateLogRecord();
        record->SetSeverity(to_severity(level));
        record->SetBody(body);
        record->SetAttribute(attr_name, body);
        s->logger->EmitLogRecord(std::move(record));
    }

    // ============================================================================
    // consumer callback
    // ============================================================================
    void on_event(const PlexdbLogEvent* event, void* ctx) {
        auto* s = static_cast<OtelState*>(ctx);
        switch (event->type) {
            case PLEXDB_LOG_PRODUCER_REGISTERED:
                handle_producer_registered(s,
                                           event->producer_registered.producer_id,
                                           event->producer_registered.name);
                break;
            case PLEXDB_LOG_STAT_META:
                handle_stat_meta(s,
                                 event->stat_meta.producer_id,
                                 event->stat_meta.stat_id,
                                 event->stat_meta.stat_type,
                                 event->stat_meta.name);
                break;
            case PLEXDB_LOG_STAT:
                handle_stat(s,
                            event->stat.producer_id,
                            event->stat.stat_id,
                            event->stat.value);
                break;
            case PLEXDB_LOG_MESSAGE:
                handle_message(s,
                               event->message.producer_id,
                               event->message.level,
                               event->message.text,
                               event->message.text_len,
                               event->message.message_id);
                break;
            default:
                break;
        }
    }

    // ============================================================================
    // lifecycle
    // ============================================================================
    OtelState* otel_init() {
        auto* s = new OtelState{};

        const char* ep       = std::getenv("PLEXDB_OTLP_ENDPOINT");
        std::string endpoint = ep ? ep : "localhost:4317";

        const char* interval_env = std::getenv("PLEXDB_OTLP_INTERVAL_MS");
        int         interval_ms  = interval_env ? std::atoi(interval_env) : 10000;
        if (interval_ms <= 0) {
            interval_ms = 10000;
        }

        const char* svc     = std::getenv("PLEXDB_OTLP_SERVICE");
        std::string service = svc ? svc : "plexdb";

        auto res = resource_sdk::Resource::Create({
            {"service.name", service}
        });

        // --- metrics ---
        otlp::OtlpGrpcMetricExporterOptions metric_opts;
        metric_opts.endpoint            = endpoint;
        metric_opts.use_ssl_credentials = false;
        auto metric_exporter            = otlp::OtlpGrpcMetricExporterFactory::Create(metric_opts);

        metrics_sdk::PeriodicExportingMetricReaderOptions reader_opts;
        reader_opts.export_interval_millis = std::chrono::milliseconds(interval_ms);
        reader_opts.export_timeout_millis  = std::chrono::milliseconds(interval_ms / 2);
        auto reader                        = metrics_sdk::PeriodicExportingMetricReaderFactory::Create(
            std::move(metric_exporter), reader_opts);

        s->meter_provider = std::make_shared<metrics_sdk::MeterProvider>(
            std::make_unique<metrics_sdk::ViewRegistry>(), res);
        s->meter_provider->AddMetricReader(std::move(reader));
        s->meter = s->meter_provider->GetMeter("plexdb.plugin");

        // --- logs ---
        otlp::OtlpGrpcLogRecordExporterOptions log_opts;
        log_opts.endpoint            = endpoint;
        log_opts.use_ssl_credentials = false;
        auto log_exporter            = otlp::OtlpGrpcLogRecordExporterFactory::Create(log_opts);
        auto log_processor           = logs_sdk::SimpleLogRecordProcessorFactory::Create(
            std::move(log_exporter));

        s->logger_provider = std::make_shared<logs_sdk::LoggerProvider>(
            std::move(log_processor), res);
        s->logger = s->logger_provider->GetLogger("plexdb.plugin", "plexdb");

        std::fprintf(stderr, "[otel] exporting to %s every %d ms (service: %s)\n",
                     endpoint.c_str(), interval_ms, service.c_str());

        return s;
    }

    void otel_fini(OtelState* s) {
        if (!s) {
            return;
        }
        if (s->meter_provider) {
            s->meter_provider->Shutdown();
        }
        if (s->logger_provider) {
            s->logger_provider->Shutdown();
        }
        delete s;
    }

} // namespace

__attribute__((constructor)) static void init() {
    g_state = otel_init();
    if (g_state) {
        plexdb_plugin_register_consumer(on_event, g_state);
    }
}

__attribute__((destructor)) static void fini() {
    if (!g_state) {
        return;
    }
    plexdb_plugin_unregister_consumer(on_event, g_state);
    otel_fini(g_state);
    g_state = nullptr;
}
