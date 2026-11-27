#include "solar_os_native_link.h"
#include "solar_os_config.h"
#include "solar_os_native_link_abi.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_LINK
#include "solar_os.h"
#include "solar_os_link.h"
#include "solar_os_link_repeater.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_link_repeater_abi.h"
#include "solar_os_link_stream.h"
#include "solar_os_native_support.h"

static int link_create(const char *name, uint32_t local_id, size_t frame_mtu)
{
    return solar_os_native_result(solar_os_link_create(name, local_id, frame_mtu));
}

static int link_destroy(const char *name)
{
    return solar_os_native_result(solar_os_link_destroy(name));
}

static bool link_status_at(size_t index, solar_os_link_status_t *status)
{
    const bool found = solar_os_link_get(index, status);
    if (found)
        status->last_error = solar_os_native_result(status->last_error);
    return found;
}

static int link_status(const char *name, solar_os_link_status_t *status)
{
    const esp_err_t err = solar_os_link_get_status(name, status);
    if (err == ESP_OK)
        status->last_error = solar_os_native_result(status->last_error);
    return solar_os_native_result(err);
}

static int link_send(const char *name, solar_os_link_message_type_t type, uint32_t destination,
                     const void *payload, size_t payload_len, uint16_t *sequence)
{
    return solar_os_native_result(
        solar_os_link_send(name, type, destination, payload, payload_len, sequence));
}

static int link_take_tx(const char *name, solar_os_link_frame_t *frame, uint32_t timeout_ms)
{
    return solar_os_native_result(solar_os_link_take_tx(name, frame, timeout_ms));
}

static int link_ingest(const char *name, const uint8_t *frame, size_t frame_len,
                       solar_os_link_ingest_result_t *result)
{
    return solar_os_native_result(solar_os_link_ingest(name, frame, frame_len, result));
}

static int link_receive(const char *name, solar_os_link_message_t *message, uint32_t timeout_ms)
{
    return solar_os_native_result(solar_os_link_receive(name, message, timeout_ms));
}

static int link_encode(const solar_os_link_message_t *message, solar_os_link_frame_t *frame)
{
    return solar_os_native_result(solar_os_link_encode(message, frame));
}

static int link_decode(const uint8_t *frame, size_t frame_len, solar_os_link_message_t *message)
{
    return solar_os_native_result(solar_os_link_decode(frame, frame_len, message));
}

static const solar_os_native_link_api_v1_t link_api = {
    .abi_version = SOLAR_OS_NATIVE_LINK_ABI,
    .struct_size = sizeof(link_api),
    .default_local_id = solar_os_link_default_local_id,
    .create = link_create,
    .destroy = link_destroy,
    .count = solar_os_link_count,
    .status_at = link_status_at,
    .status = link_status,
    .send = link_send,
    .take_tx = link_take_tx,
    .ingest = link_ingest,
    .receive = link_receive,
    .encode = link_encode,
    .decode = link_decode,
    .message_type_name = solar_os_link_message_type_name,
};

_Static_assert(SOLAR_OS_LINK_STREAM_PORT_NAME_CAPACITY == SOLAR_OS_PORT_NAME_MAX,
               "Link stream port name capacity");

static int stream_create(const char *link, const char *port, uint32_t peer,
                         const solar_os_link_stream_config_t *config)
{
    return solar_os_native_result(
        config ? solar_os_link_stream_create_configured(link, port, peer, config)
               : solar_os_link_stream_create(link, port, peer));
}

static int stream_remove(const char *port)
{
    return solar_os_native_result(solar_os_link_stream_remove(port));
}

static bool stream_status_at(size_t index, solar_os_link_stream_status_t *status)
{
    const bool found = solar_os_link_stream_get(index, status);
    if (found)
        status->last_error = solar_os_native_result(status->last_error);
    return found;
}

static int stream_status(const char *port, solar_os_link_stream_status_t *status)
{
    const esp_err_t err = solar_os_link_stream_get_status(port, status);
    if (err == ESP_OK)
        status->last_error = solar_os_native_result(status->last_error);
    return solar_os_native_result(err);
}

static int stream_ingest(const char *link, const solar_os_link_message_t *message, uint32_t now_ms)
{
    return solar_os_native_result(solar_os_link_stream_ingest(link, message, now_ms));
}

static void stream_process(const char *link, uint32_t now_ms, uint8_t window_limit)
{
    if (window_limit) {
        solar_os_link_stream_process_with_window(link, now_ms, window_limit);
    } else {
        solar_os_link_stream_process(link, now_ms);
    }
}

static const solar_os_native_link_stream_api_v1_t link_stream_api = {
    .abi_version = SOLAR_OS_NATIVE_LINK_STREAM_ABI,
    .struct_size = sizeof(link_stream_api),
    .create = stream_create,
    .default_config = solar_os_link_stream_config_default,
    .remove = stream_remove,
    .count = solar_os_link_stream_count,
    .status_at = stream_status_at,
    .status = stream_status,
    .ingest = stream_ingest,
    .process = stream_process,
    .transport_stopped = solar_os_link_stream_transport_stopped,
};

static int repeater_destroy(void *state)
{
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int repeater_open(uint32_t local_id, solar_os_native_handle_t *out)
{
    if (out == NULL) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *out = 0;
    solar_os_link_repeater_t *state = solar_os_memory_alloc(sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.link.repeater");
    if (state == NULL) {
        return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    solar_os_link_repeater_reset(state, local_id);
    const solar_os_app_t *app = solar_os_application_descriptor();
    const int result = app != NULL ?
        solar_os_native_handle_create_owned(SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, state, repeater_destroy, app, out) :
        solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, state, repeater_destroy, out);
    if (result != SOLAR_OS_NATIVE_OK) {
        solar_os_memory_free(state);
    }
    return result;
}

static int repeater_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER);
}

static int repeater_reset(solar_os_native_handle_t handle, uint32_t local_id)
{
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, &state);
    if (result != SOLAR_OS_NATIVE_OK) {
        return result;
    }
    solar_os_link_repeater_reset(state, local_id);
    solar_os_native_handle_release_exclusive(handle);
    return SOLAR_OS_NATIVE_OK;
}

static int repeater_observe(solar_os_native_handle_t handle, const solar_os_link_message_t *message,
                            const solar_os_link_frame_t *frame, uint32_t now_ms, uint32_t random_value,
                            solar_os_link_repeater_observe_result_t *out)
{
    if (message == NULL || frame == NULL || out == NULL || frame->len == 0 ||
        frame->len > SOLAR_OS_LINK_FRAME_MAX) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }

    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, &state);
    if (result != SOLAR_OS_NATIVE_OK) {
        return result;
    }
    *out = solar_os_link_repeater_observe(state, message, frame, now_ms, random_value);
    solar_os_native_handle_release_exclusive(handle);
    return SOLAR_OS_NATIVE_OK;
}

static int repeater_take_due(solar_os_native_handle_t handle, uint32_t now_ms, solar_os_link_frame_t *frame,
                             bool *ready)
{
    if (frame == NULL || ready == NULL) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *ready = false;
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, &state);
    if (result != SOLAR_OS_NATIVE_OK) {
        return result;
    }
    *ready = solar_os_link_repeater_take_due(state, now_ms, frame);
    solar_os_native_handle_release_exclusive(handle);
    return SOLAR_OS_NATIVE_OK;
}

static int repeater_note_transmit(solar_os_native_handle_t handle, bool success)
{
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, &state);
    if (result != SOLAR_OS_NATIVE_OK) {
        return result;
    }
    solar_os_link_repeater_note_transmit(state, success);
    solar_os_native_handle_release_exclusive(handle);
    return SOLAR_OS_NATIVE_OK;
}

static int repeater_note_invalid(solar_os_native_handle_t handle)
{
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, &state);
    if (result != SOLAR_OS_NATIVE_OK) {
        return result;
    }
    solar_os_link_repeater_note_invalid(state);
    solar_os_native_handle_release_exclusive(handle);
    return SOLAR_OS_NATIVE_OK;
}

static int repeater_status(solar_os_native_handle_t handle, solar_os_link_repeater_status_t *out)
{
    if (out == NULL) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }

    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER, &state);
    if (result != SOLAR_OS_NATIVE_OK) {
        return result;
    }
    solar_os_link_repeater_get_status(state, out);
    solar_os_native_handle_release_exclusive(handle);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_link_repeater_api_v1_t repeater_api = {
    .abi_version = SOLAR_OS_NATIVE_LINK_REPEATER_ABI,
    .struct_size = sizeof(repeater_api),
    .open = repeater_open,
    .close = repeater_close,
    .reset = repeater_reset,
    .observe = repeater_observe,
    .take_due = repeater_take_due,
    .note_transmit = repeater_note_transmit,
    .note_invalid = repeater_note_invalid,
    .status = repeater_status,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MESHCORE
#include "solar_os_meshcore_stream.h"

static int meshcore_stream_create(const char *port, solar_os_endpoint_id_t endpoint)
{
    return solar_os_native_result(solar_os_meshcore_stream_create(port, endpoint));
}

static int meshcore_stream_remove(const char *port)
{
    return solar_os_native_result(solar_os_meshcore_stream_remove(port));
}

static void meshcore_stream_status_errors(solar_os_meshcore_stream_status_t *status)
{
    status->transport_last_error = solar_os_native_result(status->transport_last_error);
    status->stream.last_error = solar_os_native_result(status->stream.last_error);
}

static bool meshcore_stream_status_at(size_t index, solar_os_meshcore_stream_status_t *status)
{
    const bool found = solar_os_meshcore_stream_get(index, status);
    if (found) meshcore_stream_status_errors(status);
    return found;
}

static int meshcore_stream_status(const char *port, solar_os_meshcore_stream_status_t *status)
{
    const esp_err_t error = solar_os_meshcore_stream_get_status(port, status);
    if (error == ESP_OK) meshcore_stream_status_errors(status);
    return solar_os_native_result(error);
}

static const solar_os_native_meshcore_stream_api_v1_t meshcore_stream_api = {
    .abi_version = SOLAR_OS_NATIVE_MESHCORE_STREAM_ABI, .struct_size = sizeof(meshcore_stream_api),
    .create = meshcore_stream_create, .remove = meshcore_stream_remove, .count = solar_os_meshcore_stream_count,
    .status_at = meshcore_stream_status_at, .status = meshcore_stream_status,
    .transport_active = solar_os_meshcore_stream_transport_active,
};
#endif

const void *solar_os_native_link_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_link_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
