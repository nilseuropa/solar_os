#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_link_repeater.h"

static solar_os_link_message_t message(uint32_t source,
                                       uint32_t destination,
                                       uint16_t sequence,
                                       solar_os_link_message_type_t type,
                                       uint8_t flags)
{
    return (solar_os_link_message_t){
        .version = 1U,
        .flags = flags,
        .type = type,
        .sequence = sequence,
        .source = source,
        .destination = destination,
    };
}

static solar_os_link_frame_t frame(uint8_t marker)
{
    solar_os_link_frame_t value = {.len = 14U};
    memset(value.data, marker, value.len);
    return value;
}

static solar_os_link_repeater_status_t status(
    const solar_os_link_repeater_t *repeater)
{
    solar_os_link_repeater_status_t value;
    solar_os_link_repeater_get_status(repeater, &value);
    return value;
}

static void test_delayed_forward_and_duplicate_suppression(void)
{
    solar_os_link_repeater_t repeater;
    solar_os_link_repeater_reset(&repeater, 0x30U);
    const solar_os_link_message_t incoming =
        message(0x10U, 0x20U, 1U, SOLAR_OS_LINK_MESSAGE_TEXT, 0U);
    const solar_os_link_frame_t incoming_frame = frame(0x11U);

    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          100U,
                                          0U) == SOLAR_OS_LINK_REPEATER_QUEUED);
    assert(status(&repeater).queued == 1U);

    solar_os_link_frame_t forwarded;
    assert(!solar_os_link_repeater_take_due(&repeater, 179U, &forwarded));
    assert(solar_os_link_repeater_take_due(&repeater, 180U, &forwarded));
    assert(forwarded.len == incoming_frame.len);
    assert(memcmp(forwarded.data, incoming_frame.data, forwarded.len) == 0);
    solar_os_link_repeater_note_transmit(&repeater, true);
    assert(status(&repeater).repeated == 1U);
    assert(status(&repeater).queued == 0U);

    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          200U,
                                          1U) == SOLAR_OS_LINK_REPEATER_SUPPRESSED);
    assert(status(&repeater).suppressed == 1U);
}

static void test_local_and_already_relayed_frames_are_not_forwarded(void)
{
    solar_os_link_repeater_t repeater;
    solar_os_link_repeater_reset(&repeater, 0x30U);
    const solar_os_link_frame_t incoming_frame = frame(0x22U);
    solar_os_link_message_t incoming =
        message(0x10U, 0x30U, 2U, SOLAR_OS_LINK_MESSAGE_BINARY, 0U);

    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          0U,
                                          0U) == SOLAR_OS_LINK_REPEATER_IGNORED);
    incoming = message(0x30U, 0x20U, 3U, SOLAR_OS_LINK_MESSAGE_BINARY, 0U);
    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          0U,
                                          0U) == SOLAR_OS_LINK_REPEATER_IGNORED);

    incoming = message(0x10U, 0x20U, 4U, SOLAR_OS_LINK_MESSAGE_BINARY, 0U);
    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          0U,
                                          0U) == SOLAR_OS_LINK_REPEATER_QUEUED);
    incoming.flags = SOLAR_OS_LINK_FLAG_RELAYED;
    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          10U,
                                          0U) == SOLAR_OS_LINK_REPEATER_SUPPRESSED);
    assert(status(&repeater).queued == 0U);
    assert(status(&repeater).suppressed == 1U);
}

static void test_direct_ack_cancels_data_but_is_itself_repeated(void)
{
    solar_os_link_repeater_t repeater;
    solar_os_link_repeater_reset(&repeater, 0x30U);
    const solar_os_link_message_t data =
        message(0x10U, 0x20U, 7U, SOLAR_OS_LINK_MESSAGE_TEXT, 0U);
    const solar_os_link_frame_t data_frame = frame(0x33U);
    assert(solar_os_link_repeater_observe(&repeater,
                                          &data,
                                          &data_frame,
                                          0U,
                                          0U) == SOLAR_OS_LINK_REPEATER_QUEUED);

    const solar_os_link_message_t ack =
        message(0x20U, 0x10U, 7U, SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT, 0U);
    const solar_os_link_frame_t ack_frame = frame(0x44U);
    assert(solar_os_link_repeater_observe(&repeater,
                                          &ack,
                                          &ack_frame,
                                          10U,
                                          0U) == SOLAR_OS_LINK_REPEATER_QUEUED);
    assert(status(&repeater).queued == 1U);
    assert(status(&repeater).suppressed == 1U);

    solar_os_link_frame_t forwarded;
    assert(solar_os_link_repeater_take_due(&repeater, 90U, &forwarded));
    assert(forwarded.data[0] == 0x44U);
}

static void test_queue_drop_can_be_retried(void)
{
    solar_os_link_repeater_t repeater;
    solar_os_link_repeater_reset(&repeater, 0x30U);
    solar_os_link_frame_t incoming_frame = frame(0x50U);

    for (uint16_t sequence = 1U;
         sequence <= SOLAR_OS_LINK_REPEATER_PENDING_MAX;
         sequence++) {
        const solar_os_link_message_t incoming =
            message(0x10U,
                    0x20U,
                    sequence,
                    SOLAR_OS_LINK_MESSAGE_BINARY,
                    0U);
        assert(solar_os_link_repeater_observe(&repeater,
                                              &incoming,
                                              &incoming_frame,
                                              0U,
                                              sequence) ==
               SOLAR_OS_LINK_REPEATER_QUEUED);
    }
    const solar_os_link_message_t overflow =
        message(0x10U, 0x20U, 9U, SOLAR_OS_LINK_MESSAGE_BINARY, 0U);
    assert(solar_os_link_repeater_observe(&repeater,
                                          &overflow,
                                          &incoming_frame,
                                          0U,
                                          0U) == SOLAR_OS_LINK_REPEATER_DROPPED);
    assert(status(&repeater).queue_drops == 1U);

    solar_os_link_frame_t forwarded;
    assert(solar_os_link_repeater_take_due(&repeater, 200U, &forwarded));
    assert(solar_os_link_repeater_observe(&repeater,
                                          &overflow,
                                          &incoming_frame,
                                          201U,
                                          0U) == SOLAR_OS_LINK_REPEATER_QUEUED);
}

static void test_cache_expiry_and_tick_wrap(void)
{
    solar_os_link_repeater_t repeater;
    solar_os_link_repeater_reset(&repeater, 0x30U);
    const solar_os_link_message_t incoming =
        message(0x10U, 0x20U, 10U, SOLAR_OS_LINK_MESSAGE_TEXT, 0U);
    const solar_os_link_frame_t incoming_frame = frame(0x66U);
    const uint32_t start = UINT32_MAX - 10U;
    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          start,
                                          0U) == SOLAR_OS_LINK_REPEATER_QUEUED);
    solar_os_link_frame_t forwarded;
    assert(!solar_os_link_repeater_take_due(&repeater, 50U, &forwarded));
    assert(solar_os_link_repeater_take_due(&repeater, 70U, &forwarded));

    const uint32_t after_expiry = start + SOLAR_OS_LINK_REPEATER_CACHE_TTL_MS;
    assert(solar_os_link_repeater_observe(&repeater,
                                          &incoming,
                                          &incoming_frame,
                                          after_expiry,
                                          0U) == SOLAR_OS_LINK_REPEATER_QUEUED);
    solar_os_link_repeater_note_invalid(&repeater);
    assert(status(&repeater).invalid_frames == 1U);
}

int main(void)
{
    test_delayed_forward_and_duplicate_suppression();
    test_local_and_already_relayed_frames_are_not_forwarded();
    test_direct_ack_cancels_data_but_is_itself_repeated();
    test_queue_drop_can_be_retried();
    test_cache_expiry_and_tick_wrap();
    puts("Link repeater tests passed");
    return 0;
}
