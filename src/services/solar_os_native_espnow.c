#include "solar_os_native_espnow.h"
#include "solar_os_config.h"
#include <stddef.h>
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_ESPNOW
#include "solar_os.h"
#include "solar_os_espnow.h"
#include "solar_os_memory.h"
#include "solar_os_native_espnow_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

typedef struct {
    char owner[SOLAR_OS_ESPNOW_OWNER_MAX];
    bool running;
} native_espnow_t;

static int radio_destroy(void *opaque)
{
    native_espnow_t *state = opaque;
    if (state->running) {
        const esp_err_t err = solar_os_espnow_stop(state->owner);
        state->running = false;
        if (err != ESP_OK) {
            /* The service has already detached. A retry only releases the handle. */
            return solar_os_native_result(err);
        }
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int radio_open(const char *owner, uint8_t channel, solar_os_espnow_phy_t phy,
                      solar_os_native_handle_t *out)
{
    if (out == NULL) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *out = 0;
    if (owner == NULL || owner[0] == '\0' ||
        strnlen(owner, SOLAR_OS_ESPNOW_OWNER_MAX) >= SOLAR_OS_ESPNOW_OWNER_MAX) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    native_espnow_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.espnow");
    if (state == NULL) {
        return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    strlcpy(state->owner, owner, sizeof(state->owner));

    int result = solar_os_native_result(solar_os_espnow_start(state->owner, channel, phy));
    if (result != SOLAR_OS_NATIVE_OK) {
        solar_os_memory_free(state);
        return result;
    }
    state->running = true;
    const solar_os_app_t *app = solar_os_application_descriptor();
    result = app != NULL ?
        solar_os_native_handle_create_owned(SOLAR_OS_NATIVE_HANDLE_ESPNOW, state, radio_destroy, app, out) :
        solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_ESPNOW, state, radio_destroy, out);
    if (result != SOLAR_OS_NATIVE_OK) {
        (void)solar_os_espnow_stop(state->owner);
        solar_os_memory_free(state);
        return result;
    }
    return SOLAR_OS_NATIVE_OK;
}

static bool radio_acquire(solar_os_native_handle_t handle)
{
    const native_espnow_t *state = solar_os_native_handle_acquire(handle, SOLAR_OS_NATIVE_HANDLE_ESPNOW);
    if (state == NULL) {
        return false;
    }
    if (!state->running) {
        solar_os_native_handle_release(handle);
        return false;
    }
    return true;
}

static int radio_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_ESPNOW);
}

static int radio_status(solar_os_espnow_status_t *out)
{
    if (out == NULL) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    solar_os_espnow_get_status(out);
    out->last_error = solar_os_native_result(out->last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int peer_set(uint32_t link_id, const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    return solar_os_native_result(solar_os_espnow_peer_set(link_id, mac));
}

static int peer_remove(uint32_t link_id)
{
    return solar_os_native_result(solar_os_espnow_peer_remove(link_id));
}

static int peer_learn(solar_os_native_handle_t handle, uint32_t link_id,
                      const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN], int8_t rssi, uint32_t now_ms)
{
    if (!radio_acquire(handle)) {
        return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    }
    const int result = solar_os_native_result(solar_os_espnow_peer_learn(link_id, mac, rssi, now_ms));
    solar_os_native_handle_release(handle);
    return result;
}

static int radio_send(solar_os_native_handle_t handle, uint32_t destination, const uint8_t *data, size_t len)
{
    if (!radio_acquire(handle)) {
        return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    }
    const int result = solar_os_native_result(solar_os_espnow_send(destination, data, len));
    solar_os_native_handle_release(handle);
    return result;
}

static int radio_take_result(solar_os_native_handle_t handle, int *out, uint32_t timeout_ms)
{
    if (out == NULL) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (!radio_acquire(handle)) {
        return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    }
    esp_err_t completion;
    const esp_err_t err = solar_os_espnow_take_send_result(&completion, timeout_ms);
    if (err == ESP_OK) {
        *out = solar_os_native_result(completion);
    }
    solar_os_native_handle_release(handle);
    return solar_os_native_result(err);
}

static int radio_receive(solar_os_native_handle_t handle, solar_os_espnow_packet_t *out, uint32_t timeout_ms)
{
    if (!radio_acquire(handle)) {
        return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    }
    const int result = solar_os_native_result(solar_os_espnow_receive(out, timeout_ms));
    solar_os_native_handle_release(handle);
    return result;
}

static const solar_os_native_espnow_api_v1_t espnow_api = {
    .abi_version = SOLAR_OS_NATIVE_ESPNOW_ABI,
    .struct_size = sizeof(espnow_api),
    .open = radio_open,
    .close = radio_close,
    .status = radio_status,
    .peer_count = solar_os_espnow_peer_count,
    .peer_at = solar_os_espnow_peer_get,
    .peer_set = peer_set,
    .peer_remove = peer_remove,
    .peer_learn = peer_learn,
    .send = radio_send,
    .take_send_result = radio_take_result,
    .receive = radio_receive,
    .parse_mac = solar_os_espnow_parse_mac,
    .format_mac = solar_os_espnow_format_mac,
    .parse_phy = solar_os_espnow_parse_phy,
    .phy_name = solar_os_espnow_phy_name,
};
#endif

const void *solar_os_native_espnow_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_espnow_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
