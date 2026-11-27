#include "solar_os_native_radio.h"

#include <stdatomic.h>
#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_radio_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_RADIO
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_device_providers.h"
#include "solar_os_native_handles.h"
#include "solar_os_radio.h"

typedef struct {
    solar_os_radio_handle_t raw;
    atomic_bool ready;
} native_radio_t;

static int radio_info(size_t index, solar_os_radio_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *info = (solar_os_radio_info_t){0};
    return solar_os_radio_get(index, info) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int radio_find(const char *id, solar_os_radio_info_t *info)
{
    if (info) *info = (solar_os_radio_info_t){0};
    return solar_os_native_result(solar_os_radio_get_info(id, info));
}

static int radio_observe(const char *name, solar_os_radio_status_t *status)
{
    if (status) *status = (solar_os_radio_status_t){0};
    return solar_os_native_result(solar_os_radio_get_status(name, status));
}

static int radio_read_rssi(const char *name, int16_t *dbm)
{
    return solar_os_native_result(solar_os_radio_read_rssi(name, dbm));
}

static int radio_destroy(void *opaque)
{
    native_radio_t *state = opaque;
    if (!atomic_load(&state->ready)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->raw.index >= 0) {
        const esp_err_t error = solar_os_radio_release(&state->raw);
        if (error != ESP_OK) return solar_os_native_result(error);
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int radio_open(const char *name, const char *owner, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    native_radio_t *state = solar_os_memory_calloc(
        1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.radio");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->raw = (solar_os_radio_handle_t)SOLAR_OS_RADIO_HANDLE_INIT;
    atomic_init(&state->ready, false);
    const solar_os_app_t *app = solar_os_application_descriptor();
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_RADIO;
    solar_os_native_handle_t handle;
    int result = app ? solar_os_native_handle_create_owned(kind, state, radio_destroy, app, &handle) :
        solar_os_native_handle_create(kind, state, radio_destroy, &handle);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }

    result = solar_os_native_result(solar_os_radio_claim(name, owner, &state->raw));
    atomic_store(&state->ready, true);
    if (result) {
        (void)solar_os_native_handle_close_kind(handle, kind);
        return result;
    }
    *out = handle;
    return SOLAR_OS_NATIVE_OK;
}

static int radio_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_RADIO);
}

static native_radio_t *radio_acquire(solar_os_native_handle_t id)
{
    native_radio_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_RADIO);
    if (state && !atomic_load(&state->ready)) {
        solar_os_native_handle_release(id);
        state = NULL;
    }
    return state;
}

static bool radio_valid(solar_os_native_handle_t id)
{
    native_radio_t *state = radio_acquire(id);
    if (!state) return false;
    const bool valid = solar_os_radio_handle_valid(&state->raw);
    solar_os_native_handle_release(id);
    return valid;
}

static int radio_status(solar_os_native_handle_t id, solar_os_radio_status_t *status)
{
    if (status) *status = (solar_os_radio_status_t){0};
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_get_status(&state->raw, status);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_configure(solar_os_native_handle_t id, const solar_os_radio_config_t *config)
{
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_configure(&state->raw, config);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_set_state(solar_os_native_handle_t id, solar_os_radio_state_t mode)
{
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_set_state(&state->raw, mode);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_tune_rx(solar_os_native_handle_t id, uint32_t frequency_hz, uint32_t *settle_ms)
{
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_tune_rx(&state->raw, frequency_hz, settle_ms);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_send(solar_os_native_handle_t id, const solar_os_radio_packet_t *packet,
                            uint32_t timeout_ms)
{
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_send(&state->raw, packet, timeout_ms);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_send_stream(solar_os_native_handle_t id, const void *bytes, size_t size,
                             uint32_t timeout_ms)
{
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_send_stream(&state->raw, bytes, size, timeout_ms);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_receive(solar_os_native_handle_t id, solar_os_radio_packet_t *packet,
                         uint32_t timeout_ms)
{
    if (packet) *packet = (solar_os_radio_packet_t){0};
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_receive(&state->raw, packet, timeout_ms);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_profile_apply(solar_os_native_handle_t id, const char *name)
{
    native_radio_t *state = radio_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_radio_handle_profile_apply(&state->raw, name);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int radio_profile_list(solar_os_radio_profile_t *profiles, size_t capacity, size_t *count)
{
    return solar_os_native_result(solar_os_radio_profile_list(profiles, capacity, count));
}

static int radio_profile_get(const char *name, solar_os_radio_profile_t *profile)
{
    if (profile) *profile = (solar_os_radio_profile_t){0};
    return solar_os_native_result(solar_os_radio_profile_get(name, profile));
}

static int radio_profile_save(const char *radio, const char *name)
{
    return solar_os_native_result(solar_os_radio_profile_save(radio, name));
}

static int radio_profile_remove(const char *name)
{
    return solar_os_native_result(solar_os_radio_profile_remove(name));
}

static const solar_os_native_radio_api_v1_t radio_api = {
    .abi_version = SOLAR_OS_NATIVE_RADIO_ABI,
    .struct_size = sizeof(radio_api),
    .count = solar_os_radio_count,
    .info = radio_info,
    .find = radio_find,
    .observe = radio_observe,
    .read_rssi = radio_read_rssi,
    .open = radio_open,
    .close = radio_close,
    .valid = radio_valid,
    .status = radio_status,
    .configure = radio_configure,
    .set_state = radio_set_state,
    .tune_rx = radio_tune_rx,
    .send = radio_send,
    .send_stream = radio_send_stream,
    .receive = radio_receive,
    .profile_list = radio_profile_list,
    .profile_get = radio_profile_get,
    .profile_apply = radio_profile_apply,
    .profile_save = radio_profile_save,
    .profile_remove = radio_profile_remove,
    .modulation_name = solar_os_radio_modulation_name,
    .modulation_from_name = solar_os_radio_modulation_from_name,
    .feature_name = solar_os_radio_feature_name,
    .state_name = solar_os_radio_state_name,
    .state_from_name = solar_os_radio_state_from_name,
    .rx_gain_name = solar_os_radio_rx_gain_name,
    .rx_gain_parse = solar_os_radio_rx_gain_parse,
    .format_modulations = solar_os_radio_modulations_format,
    .format_features = solar_os_radio_features_format,
    .format_rx_gains = solar_os_radio_rx_gains_format,
};
static const solar_os_native_radio_provider_api_v1_t radio_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_RADIO_PROVIDER_ABI,
    .struct_size = sizeof(radio_provider_api),
    .provider_register = solar_os_native_radio_provider_register,
    .provider_close = solar_os_native_radio_provider_close,
};
#endif

const void *solar_os_native_radio_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_radio_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
