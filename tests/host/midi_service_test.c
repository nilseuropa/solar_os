#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_midi.h"
#include "solar_os_stream.h"

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

uint64_t solar_os_time_uptime_ms(void)
{
    return 1234U;
}

esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch_ms)
{
    *epoch_ms = 5678U;
    return ESP_OK;
}

int main(void)
{
    assert(solar_os_stream_init() == ESP_OK);
    assert(solar_os_midi_cc_stream_add(0U, 74U) == ESP_ERR_INVALID_ARG);
    assert(solar_os_midi_cc_stream_add(1U, 128U) == ESP_ERR_INVALID_ARG);
    assert(solar_os_midi_cc_stream_add(1U, 74U) == ESP_OK);
    assert(solar_os_midi_cc_stream_add(1U, 74U) == ESP_ERR_INVALID_STATE);
    assert(solar_os_midi_cc_stream_count() == 1U);

    solar_os_midi_cc_stream_info_t info;
    assert(solar_os_midi_cc_stream_get(0U, &info));
    assert(strcmp(info.id, "midi.cc.1.74") == 0);
    assert(info.channel == 1U && info.controller == 74U);
    assert(!info.has_value && info.updates == 0U);

    solar_os_stream_info_t stream_info;
    assert(solar_os_stream_get_info("midi.cc.1.74", &stream_info) == ESP_OK);
    assert(stream_info.type == SOLAR_OS_STREAM_TYPE_SCALAR);
    assert(strcmp(stream_info.provider, "midi") == 0);

    solar_os_stream_handle_t stream = SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open("midi.cc.1.74", "test", &stream) == ESP_OK);
    float value = 0.0f;
    assert(solar_os_stream_read_scalar(&stream, NULL, &value) ==
           ESP_ERR_INVALID_STATE);
    assert(solar_os_midi_worker_start("midi0") == ESP_OK);
    assert(solar_os_stream_read_scalar(&stream, NULL, &value) ==
           ESP_ERR_INVALID_STATE);

    const solar_os_midi_message_t note = {
        .status = 0x90U,
        .data1 = 60U,
        .data2 = 100U,
        .length = 3U,
    };
    solar_os_midi_worker_publish(&note);
    assert(solar_os_stream_read_scalar(&stream, NULL, &value) ==
           ESP_ERR_INVALID_STATE);

    const solar_os_midi_message_t other_cc = {
        .status = 0xb1U,
        .data1 = 74U,
        .data2 = 63U,
        .length = 3U,
    };
    solar_os_midi_worker_publish(&other_cc);
    assert(solar_os_stream_read_scalar(&stream, NULL, &value) ==
           ESP_ERR_INVALID_STATE);

    const solar_os_midi_message_t matching_cc = {
        .status = 0xb0U,
        .data1 = 74U,
        .data2 = 96U,
        .length = 3U,
    };
    solar_os_midi_subscription_t subscription =
        SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
    assert(solar_os_midi_subscribe("test", &subscription) == ESP_OK);
    solar_os_midi_worker_publish(&matching_cc);
    assert(solar_os_stream_read_scalar(&stream, NULL, &value) == ESP_OK);
    assert(value == 96.0f);
    assert(solar_os_midi_cc_stream_get(0U, &info));
    assert(info.has_value && info.value == 96U && info.updates == 1U);
    solar_os_midi_message_t received;
    assert(solar_os_midi_receive(&subscription, &received) == ESP_OK);
    assert(memcmp(&received, &matching_cc, sizeof(received)) == 0);
    assert(solar_os_midi_unsubscribe(&subscription) == ESP_OK);

    solar_os_midi_worker_stop();
    assert(solar_os_stream_read_scalar(&stream, NULL, &value) ==
           ESP_ERR_INVALID_STATE);
    assert(solar_os_midi_cc_stream_get(0U, &info) && !info.has_value);
    assert(solar_os_midi_worker_start("midi0") == ESP_OK);
    assert(solar_os_stream_read_scalar(&stream, NULL, &value) ==
           ESP_ERR_INVALID_STATE);

    assert(solar_os_midi_cc_stream_remove(1U, 74U) ==
           ESP_ERR_INVALID_STATE);
    solar_os_stream_close(&stream);
    assert(solar_os_midi_cc_stream_remove(1U, 74U) == ESP_OK);
    assert(solar_os_stream_get_info("midi.cc.1.74", &stream_info) ==
           ESP_ERR_NOT_FOUND);

    for (uint8_t controller = 0U;
         controller < SOLAR_OS_MIDI_CC_STREAM_MAX; controller++) {
        assert(solar_os_midi_cc_stream_add(2U, controller) == ESP_OK);
    }
    assert(solar_os_midi_cc_stream_add(
               2U, SOLAR_OS_MIDI_CC_STREAM_MAX) == ESP_ERR_NO_MEM);
    size_t removed = 0U;
    assert(solar_os_midi_cc_stream_clear(&removed) == ESP_OK);
    assert(removed == SOLAR_OS_MIDI_CC_STREAM_MAX);
    assert(solar_os_midi_cc_stream_count() == 0U);

    solar_os_midi_worker_stop();
    puts("midi service tests: ok");
    return 0;
}
