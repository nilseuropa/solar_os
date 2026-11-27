#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_midi.h"
#include "solar_os_midi.h"
#include "solar_os_stream.h"

static const solar_os_app_t midi_app = {.name = "midi-test"};
static const solar_os_app_t *active_app;
static unsigned allocations;

const solar_os_app_t *solar_os_application_descriptor(void) { return active_app; }

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t category, const char *tag)
{
    (void)category;
    (void)tag;
    void *memory = calloc(count, size);
    if (memory) allocations++;
    return memory;
}

void solar_os_memory_free(void *memory)
{
    if (memory) allocations--;
    free(memory);
}

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

static void test_native_midi(void)
{
    const solar_os_native_midi_api_v1_t *api = solar_os_native_midi_get_service(
        SOLAR_OS_NATIVE_MIDI_SERVICE, SOLAR_OS_NATIVE_MIDI_ABI, sizeof(*api));
    assert(api);
    assert(!solar_os_native_midi_get_service(SOLAR_OS_NATIVE_MIDI_SERVICE, 1, sizeof(*api) + 1));

    assert(solar_os_midi_worker_start("midi0") == ESP_OK);
    solar_os_midi_decoder_t decoder;
    api->decoder_reset(&decoder);
    solar_os_midi_message_t message;
    assert(api->decode_byte(&decoder, 0x90, &message) == SOLAR_OS_MIDI_DECODE_NONE);
    assert(api->decode_byte(&decoder, 61, &message) == SOLAR_OS_MIDI_DECODE_NONE);
    assert(api->decode_byte(&decoder, 0xf8, &message) == SOLAR_OS_MIDI_DECODE_MESSAGE && message.status == 0xf8);
    assert(api->decode_byte(&decoder, 90, &message) == SOLAR_OS_MIDI_DECODE_MESSAGE);
    assert(message.status == 0x90 && message.data1 == 61 && message.data2 == 90);
    assert(api->message_valid(&message) && api->message_length(message.status) == 3);
    uint8_t bytes[3];
    assert(api->encode(&message, bytes) == 3 && bytes[0] == 0x90 && bytes[1] == 61 && bytes[2] == 90);
    assert(api->send(&message) == SOLAR_OS_NATIVE_OK);
    assert(solar_os_midi_worker_take_tx(&message) && message.data1 == 61);
    const solar_os_midi_message_t invalid = {0};
    assert(!api->message_valid(&invalid) && api->send(&invalid) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);

    active_app = &midi_app;
    solar_os_native_handle_t first, second;
    assert(api->subscribe("listener", &first) == SOLAR_OS_NATIVE_OK);
    assert(api->subscribe("listener", &second) == SOLAR_OS_NATIVE_OK && second != first);
    active_app = NULL;
    assert(api->receive(first, &message) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && !message.length);

    assert(api->cc_add(3, 21) == SOLAR_OS_NATIVE_OK && api->cc_count() == 1);
    const solar_os_midi_message_t cc = {.status = 0xb2, .data1 = 21, .data2 = 15, .length = 3};
    solar_os_midi_worker_publish(&cc);
    assert(api->receive(first, &message) == SOLAR_OS_NATIVE_OK && message.data2 == 15);
    assert(api->receive(second, &message) == SOLAR_OS_NATIVE_OK && message.data2 == 15);
    solar_os_midi_cc_stream_info_t info;
    assert(api->cc_info(0, &info) == SOLAR_OS_NATIVE_OK);
    assert(!strcmp(info.id, "midi.cc.3.21") && info.has_value && info.value == 15);
    size_t removed = 0;
    assert(api->cc_clear(&removed) == SOLAR_OS_NATIVE_OK && removed == 1);

    solar_os_midi_worker_note_error(ESP_ERR_TIMEOUT);
    solar_os_midi_status_t status;
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK && status.running && !strcmp(status.bus_name, "midi0"));
    assert(status.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT && status.rx_messages == 1);
    assert(api->unsubscribe(first) == SOLAR_OS_NATIVE_OK);
    assert(solar_os_native_handle_close_owner(&midi_app) == SOLAR_OS_NATIVE_OK);
    assert(api->receive(second, &message) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!allocations && !solar_os_native_handle_count());
    solar_os_midi_worker_stop();
    puts("MIDI ABI: byte codec, queued I/O, independent subscriptions and controller streams passed");
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
    test_native_midi();
    return 0;
}
