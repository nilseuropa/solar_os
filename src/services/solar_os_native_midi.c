#include "solar_os_native_midi.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_midi_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_MIDI
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_midi.h"
#include "solar_os_native_handles.h"

static int midi_subscription_destroy(void *opaque)
{
    solar_os_midi_subscription_t *state = opaque;
    const esp_err_t error = solar_os_midi_unsubscribe(state);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int midi_subscribe(const char *owner, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_midi_subscription_t *state = solar_os_memory_calloc(
        1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.midi");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const esp_err_t error = solar_os_midi_subscribe(owner, state);
    if (error != ESP_OK) {
        solar_os_memory_free(state);
        return solar_os_native_result(error);
    }

    const solar_os_app_t *app = solar_os_application_descriptor();
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_MIDI_SUBSCRIPTION;
    const int result = app ? solar_os_native_handle_create_owned(kind, state, midi_subscription_destroy, app, out) :
        solar_os_native_handle_create(kind, state, midi_subscription_destroy, out);
    if (result) (void)midi_subscription_destroy(state);
    return result;
}

static int midi_unsubscribe(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_MIDI_SUBSCRIPTION);
}

static int midi_receive(solar_os_native_handle_t id, solar_os_midi_message_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_midi_message_t){0};
    solar_os_midi_subscription_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_MIDI_SUBSCRIPTION);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_midi_receive(state, out);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int midi_send(const solar_os_midi_message_t *message)
{
    return solar_os_native_result(solar_os_midi_send(message));
}

static int midi_status(solar_os_midi_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_midi_get_status(status);
    status->last_error = solar_os_native_result(status->last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int midi_cc_add(uint8_t channel, uint8_t controller)
{
    return solar_os_native_result(solar_os_midi_cc_stream_add(channel, controller));
}

static int midi_cc_remove(uint8_t channel, uint8_t controller)
{
    return solar_os_native_result(solar_os_midi_cc_stream_remove(channel, controller));
}

static int midi_cc_clear(size_t *removed)
{
    return solar_os_native_result(solar_os_midi_cc_stream_clear(removed));
}

static int midi_cc_info(size_t index, solar_os_midi_cc_stream_info_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_midi_cc_stream_info_t){0};
    return solar_os_midi_cc_stream_get(index, out) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static const solar_os_native_midi_api_v1_t midi_api = {
    .abi_version = SOLAR_OS_NATIVE_MIDI_ABI,
    .struct_size = sizeof(midi_api),
    .subscribe = midi_subscribe,
    .unsubscribe = midi_unsubscribe,
    .receive = midi_receive,
    .send = midi_send,
    .status = midi_status,
    .cc_add = midi_cc_add,
    .cc_remove = midi_cc_remove,
    .cc_clear = midi_cc_clear,
    .cc_count = solar_os_midi_cc_stream_count,
    .cc_info = midi_cc_info,
    .decoder_reset = solar_os_midi_decoder_reset,
    .decode_byte = solar_os_midi_decode_byte,
    .message_length = solar_os_midi_message_length,
    .message_valid = solar_os_midi_message_valid,
    .encode = solar_os_midi_encode,
    .format_monitor = solar_os_midi_format_monitor,
};
#endif

const void *solar_os_native_midi_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_midi_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
