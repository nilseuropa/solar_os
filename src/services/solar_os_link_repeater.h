#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os_link.h"

#define SOLAR_OS_LINK_REPEATER_PENDING_MAX 4U
#define SOLAR_OS_LINK_REPEATER_CACHE_MAX 16U
#define SOLAR_OS_LINK_REPEATER_DELAY_MIN_MS 80U
#define SOLAR_OS_LINK_REPEATER_DELAY_JITTER_MS 120U
#define SOLAR_OS_LINK_REPEATER_CACHE_TTL_MS 60000U

typedef struct {
    uint32_t source;
    uint32_t destination;
    uint16_t sequence;
    uint8_t type;
} solar_os_link_repeater_identity_t;

typedef struct {
    solar_os_link_repeater_identity_t identity;
    solar_os_link_frame_t frame;
    uint32_t due_ms;
    bool active;
} solar_os_link_repeater_pending_t;

typedef struct {
    solar_os_link_repeater_identity_t identity;
    uint32_t expires_ms;
    bool active;
} solar_os_link_repeater_cache_entry_t;

typedef struct {
    uint32_t local_id;
    solar_os_link_repeater_pending_t pending[SOLAR_OS_LINK_REPEATER_PENDING_MAX];
    solar_os_link_repeater_cache_entry_t cache[SOLAR_OS_LINK_REPEATER_CACHE_MAX];
    size_t cache_next;
    uint32_t repeated;
    uint32_t suppressed;
    uint32_t queue_drops;
    uint32_t invalid_frames;
} solar_os_link_repeater_t;

typedef struct {
    size_t queued;
    uint32_t repeated;
    uint32_t suppressed;
    uint32_t queue_drops;
    uint32_t invalid_frames;
} solar_os_link_repeater_status_t;

typedef enum {
    SOLAR_OS_LINK_REPEATER_IGNORED,
    SOLAR_OS_LINK_REPEATER_QUEUED,
    SOLAR_OS_LINK_REPEATER_SUPPRESSED,
    SOLAR_OS_LINK_REPEATER_DROPPED,
} solar_os_link_repeater_observe_result_t;

void solar_os_link_repeater_reset(solar_os_link_repeater_t *repeater,
                                  uint32_t local_id);
solar_os_link_repeater_observe_result_t solar_os_link_repeater_observe(
    solar_os_link_repeater_t *repeater,
    const solar_os_link_message_t *message,
    const solar_os_link_frame_t *frame,
    uint32_t now_ms,
    uint32_t random_value);
bool solar_os_link_repeater_take_due(solar_os_link_repeater_t *repeater,
                                     uint32_t now_ms,
                                     solar_os_link_frame_t *frame);
void solar_os_link_repeater_note_transmit(solar_os_link_repeater_t *repeater,
                                          bool success);
void solar_os_link_repeater_note_invalid(solar_os_link_repeater_t *repeater);
void solar_os_link_repeater_get_status(
    const solar_os_link_repeater_t *repeater,
    solar_os_link_repeater_status_t *status);
