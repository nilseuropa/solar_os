#include "solar_os_link_repeater.h"

#include <string.h>

static bool repeater_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static solar_os_link_repeater_identity_t repeater_identity(
    const solar_os_link_message_t *message)
{
    return (solar_os_link_repeater_identity_t){
        .source = message->source,
        .destination = message->destination,
        .sequence = message->sequence,
        .type = (uint8_t)message->type,
    };
}

static bool repeater_identity_equal(
    const solar_os_link_repeater_identity_t *left,
    const solar_os_link_repeater_identity_t *right)
{
    return left->source == right->source &&
        left->destination == right->destination &&
        left->sequence == right->sequence &&
        left->type == right->type;
}

static void repeater_cache_prune(solar_os_link_repeater_t *repeater,
                                 uint32_t now_ms)
{
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_CACHE_MAX; i++) {
        solar_os_link_repeater_cache_entry_t *entry = &repeater->cache[i];
        if (entry->active && repeater_time_reached(now_ms, entry->expires_ms)) {
            entry->active = false;
        }
    }
}

static bool repeater_cache_contains(
    const solar_os_link_repeater_t *repeater,
    const solar_os_link_repeater_identity_t *identity)
{
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_CACHE_MAX; i++) {
        const solar_os_link_repeater_cache_entry_t *entry = &repeater->cache[i];
        if (entry->active && repeater_identity_equal(&entry->identity, identity)) {
            return true;
        }
    }
    return false;
}

static void repeater_cache_remember(
    solar_os_link_repeater_t *repeater,
    const solar_os_link_repeater_identity_t *identity,
    uint32_t now_ms)
{
    size_t selected = SOLAR_OS_LINK_REPEATER_CACHE_MAX;
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_CACHE_MAX; i++) {
        solar_os_link_repeater_cache_entry_t *entry = &repeater->cache[i];
        if (entry->active && repeater_identity_equal(&entry->identity, identity)) {
            selected = i;
            break;
        }
        if (selected == SOLAR_OS_LINK_REPEATER_CACHE_MAX && !entry->active) {
            selected = i;
        }
    }
    if (selected == SOLAR_OS_LINK_REPEATER_CACHE_MAX) {
        selected = repeater->cache_next++ % SOLAR_OS_LINK_REPEATER_CACHE_MAX;
    }
    repeater->cache[selected] = (solar_os_link_repeater_cache_entry_t){
        .identity = *identity,
        .expires_ms = now_ms + SOLAR_OS_LINK_REPEATER_CACHE_TTL_MS,
        .active = true,
    };
}

static bool repeater_cancel_identity(
    solar_os_link_repeater_t *repeater,
    const solar_os_link_repeater_identity_t *identity)
{
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_PENDING_MAX; i++) {
        solar_os_link_repeater_pending_t *pending = &repeater->pending[i];
        if (pending->active &&
            repeater_identity_equal(&pending->identity, identity)) {
            pending->active = false;
            return true;
        }
    }
    return false;
}

static bool repeater_cancel_acknowledged(
    solar_os_link_repeater_t *repeater,
    const solar_os_link_message_t *message)
{
    if (message->type != SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT) {
        return false;
    }
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_PENDING_MAX; i++) {
        solar_os_link_repeater_pending_t *pending = &repeater->pending[i];
        if (!pending->active ||
            (pending->identity.type != SOLAR_OS_LINK_MESSAGE_TEXT &&
             pending->identity.type != SOLAR_OS_LINK_MESSAGE_BINARY)) {
            continue;
        }
        if (pending->identity.source == message->destination &&
            pending->identity.destination == message->source &&
            pending->identity.sequence == message->sequence) {
            pending->active = false;
            return true;
        }
    }
    return false;
}

void solar_os_link_repeater_reset(solar_os_link_repeater_t *repeater,
                                  uint32_t local_id)
{
    if (repeater == NULL) {
        return;
    }
    memset(repeater, 0, sizeof(*repeater));
    repeater->local_id = local_id;
}

solar_os_link_repeater_observe_result_t solar_os_link_repeater_observe(
    solar_os_link_repeater_t *repeater,
    const solar_os_link_message_t *message,
    const solar_os_link_frame_t *frame,
    uint32_t now_ms,
    uint32_t random_value)
{
    if (repeater == NULL || message == NULL || frame == NULL ||
        frame->len == 0U || frame->len > SOLAR_OS_LINK_FRAME_MAX) {
        return SOLAR_OS_LINK_REPEATER_IGNORED;
    }

    repeater_cache_prune(repeater, now_ms);
    if (repeater_cancel_acknowledged(repeater, message)) {
        repeater->suppressed++;
    }

    if (message->source == 0U || message->source == repeater->local_id ||
        message->destination == 0U || message->destination == repeater->local_id) {
        return SOLAR_OS_LINK_REPEATER_IGNORED;
    }

    const solar_os_link_repeater_identity_t identity =
        repeater_identity(message);
    if ((message->flags & SOLAR_OS_LINK_FLAG_RELAYED) != 0U) {
        (void)repeater_cancel_identity(repeater, &identity);
        repeater_cache_remember(repeater, &identity, now_ms);
        repeater->suppressed++;
        return SOLAR_OS_LINK_REPEATER_SUPPRESSED;
    }
    if (repeater_cache_contains(repeater, &identity)) {
        repeater->suppressed++;
        return SOLAR_OS_LINK_REPEATER_SUPPRESSED;
    }

    size_t selected = SOLAR_OS_LINK_REPEATER_PENDING_MAX;
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_PENDING_MAX; i++) {
        if (!repeater->pending[i].active) {
            selected = i;
            break;
        }
    }
    if (selected == SOLAR_OS_LINK_REPEATER_PENDING_MAX) {
        repeater->queue_drops++;
        return SOLAR_OS_LINK_REPEATER_DROPPED;
    }

    repeater_cache_remember(repeater, &identity, now_ms);
    const uint32_t delay_ms = SOLAR_OS_LINK_REPEATER_DELAY_MIN_MS +
        random_value % (SOLAR_OS_LINK_REPEATER_DELAY_JITTER_MS + 1U);
    repeater->pending[selected] = (solar_os_link_repeater_pending_t){
        .identity = identity,
        .frame = *frame,
        .due_ms = now_ms + delay_ms,
        .active = true,
    };
    return SOLAR_OS_LINK_REPEATER_QUEUED;
}

bool solar_os_link_repeater_take_due(solar_os_link_repeater_t *repeater,
                                     uint32_t now_ms,
                                     solar_os_link_frame_t *frame)
{
    if (repeater == NULL || frame == NULL) {
        return false;
    }
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_PENDING_MAX; i++) {
        solar_os_link_repeater_pending_t *pending = &repeater->pending[i];
        if (!pending->active || !repeater_time_reached(now_ms, pending->due_ms)) {
            continue;
        }
        *frame = pending->frame;
        pending->active = false;
        return true;
    }
    return false;
}

void solar_os_link_repeater_note_transmit(solar_os_link_repeater_t *repeater,
                                          bool success)
{
    if (repeater != NULL && success) {
        repeater->repeated++;
    }
}

void solar_os_link_repeater_note_invalid(solar_os_link_repeater_t *repeater)
{
    if (repeater != NULL) {
        repeater->invalid_frames++;
    }
}

void solar_os_link_repeater_get_status(
    const solar_os_link_repeater_t *repeater,
    solar_os_link_repeater_status_t *status)
{
    if (repeater == NULL || status == NULL) {
        return;
    }
    *status = (solar_os_link_repeater_status_t){
        .repeated = repeater->repeated,
        .suppressed = repeater->suppressed,
        .queue_drops = repeater->queue_drops,
        .invalid_frames = repeater->invalid_frames,
    };
    for (size_t i = 0; i < SOLAR_OS_LINK_REPEATER_PENDING_MAX; i++) {
        if (repeater->pending[i].active) {
            status->queued++;
        }
    }
}
