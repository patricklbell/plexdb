#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// data types
// ============================================================================
typedef enum PlexdbLogEventType : uint32_t {
    PLEXDB_LOG_PRODUCER_REGISTERED = 1,  // producer announces itself
    PLEXDB_LOG_MESSAGE             = 2,  // generic string message
} PlexdbLogEventType;

typedef struct {
    uint32_t    producer_id;
    const char* name;
} PlexdbLogProducerRegistered;

typedef struct {
    uint32_t    producer_id;
    uint32_t    _pad;   // @padding
    const char* text;
    size_t      text_len;
} PlexdbLogMessage;

// @note fat struct with a type tag and union payload
typedef struct {
    uint32_t type;   // PlexdbLogEventType
    uint32_t _pad;   // @padding
    union {
        PlexdbLogProducerRegistered producer_registered;
        PlexdbLogMessage            message;
    };
} PlexdbLogEvent;

// ============================================================================
// ABI
// ============================================================================
typedef void (*PlexdbLogConsumerFn)(const PlexdbLogEvent* event, void* ctx);

// Exported from the host binary. Plugins call these from their
// __attribute__((constructor/destructor)) to self-register via normal
// shared-object symbol resolution (LD_PRELOAD or equivalent).
//
// @note Load-order contract: plugins MUST be loaded before main() starts
//   (e.g. via LD_PRELOAD). A consumer only observes messages fired after
//   plexdb_log_register_consumer returns. No buffering is performed.
//   PLEXDB_LOG_PRODUCER_REGISTERED catch-up IS provided: all producers that
//   registered before the consumer is added are replayed immediately, so the
//   consumer always has a complete view of producer IDs and names.
void plexdb_log_register_consumer(PlexdbLogConsumerFn fn, void* ctx);
void plexdb_log_unregister_consumer(PlexdbLogConsumerFn fn, void* ctx);

#ifdef __cplusplus
}
#endif
