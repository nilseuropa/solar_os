#pragma once

#include "solar_os_link_types_v1.h"
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_LINK_MESSAGING_SERVICE "link.messaging"
#define SOLAR_OS_NATIVE_LINK_MESSAGING_ABI 1U

/* Bind a Link transport to the shared conversations, contacts and outbox.
 * Use the messages service to read and send conversations. Feed transport
 * events here and advance the binding with monotonic milliseconds. End the
 * transport's ticks before stopping its binding. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*start)(const char *link);
    void (*stop)(const char *link);
    void (*process)(const char *link, uint32_t now_ms);
    void (*note_transmit)(const char *link, const solar_os_link_frame_t *frame,
                          int result, uint32_t now_ms);
    int (*note_ingest)(const char *link, const solar_os_link_ingest_result_t *result, uint32_t now_ms);
} solar_os_native_link_messaging_api_v1_t;
