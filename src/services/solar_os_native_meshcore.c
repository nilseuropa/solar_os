#include "solar_os_native_meshcore.h"
#include "solar_os_config.h"
#include "solar_os_native_support.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_MESHCORE || SOLAR_OS_PACKAGE_SERVICE_MESHCORE_BLE
#include "solar_os_meshcore_instances.h"

static void meshcore_scope_end(void **lease)
{
    solar_os_meshcore_legacy_end(*lease);
}

static bool meshcore_provider_next(unsigned kind, solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *status)
{
    const bool found = solar_os_meshcore_instances_next(kind, cursor, status);
    if (found) status->last_error = solar_os_native_result(status->last_error);
    return found;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MESHCORE
#include "solar_os_meshcore.h"

static bool meshcore_radio_provider_next(solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *status)
{
    return meshcore_provider_next(SOLAR_OS_MESHCORE_BACKEND_RADIO, cursor, status);
}

static int meshcore_radio_status(solar_os_messaging_provider_id_t provider, solar_os_meshcore_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_get_status(status);
    if (error == ESP_OK) status->last_error = solar_os_native_result(status->last_error);
    return solar_os_native_result(error);
}

static int meshcore_radio_identity_generate(solar_os_messaging_provider_id_t provider, bool replace_existing)
{
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_identity_generate(replace_existing);
    return solar_os_native_result(error);
}

static int meshcore_radio_identity_import(solar_os_messaging_provider_id_t provider, const char *private_key_hex)
{
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_identity_import(private_key_hex);
    return solar_os_native_result(error);
}

static int meshcore_radio_identity_public(solar_os_messaging_provider_id_t provider, char *out, size_t capacity)
{
    if (!out || capacity < SOLAR_OS_MESHCORE_PUBLIC_KEY_HEX_LEN) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_identity_public(out);
    return solar_os_native_result(error);
}

static int meshcore_radio_identity_export_private(solar_os_messaging_provider_id_t provider, char *out, size_t capacity)
{
    if (!out || capacity < SOLAR_OS_MESHCORE_PRIVATE_KEY_HEX_LEN) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_identity_export_private(out);
    return solar_os_native_result(error);
}

static int meshcore_radio_get_name(solar_os_messaging_provider_id_t provider, char *out, size_t capacity)
{
    if (!out || capacity <= SOLAR_OS_MESHCORE_NAME_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_name_get(out);
    return solar_os_native_result(error);
}

static int meshcore_radio_set_name(solar_os_messaging_provider_id_t provider, const char *name)
{
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_name_set(name);
    return solar_os_native_result(error);
}

static int meshcore_radio_channels(solar_os_messaging_provider_id_t provider, solar_os_meshcore_channel_t *out,
    size_t capacity, size_t *count)
{
    if (!count || (capacity && !out)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count = 0;
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    if (capacity) *count = solar_os_meshcore_channel_snapshot(out, capacity);
    return SOLAR_OS_NATIVE_OK;
}

static int meshcore_radio_channel_add(solar_os_messaging_provider_id_t provider, const char *name,
    const char *base64_psk)
{
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_channel_add(name, base64_psk);
    return solar_os_native_result(error);
}

static int meshcore_radio_channel_remove(solar_os_messaging_provider_id_t provider, const char *name)
{
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_channel_remove(name);
    return solar_os_native_result(error);
}

static int meshcore_radio_channel_public_set(solar_os_messaging_provider_id_t provider, bool enabled)
{
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_channel_public_set(enabled);
    return solar_os_native_result(error);
}

static int meshcore_radio_advertise(solar_os_messaging_provider_id_t provider, solar_os_meshcore_advert_t mode)
{
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_RADIO, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_request_advert(mode);
    return solar_os_native_result(error);
}

static const solar_os_native_meshcore_radio_api_v1_t meshcore_radio_api = {
    .abi_version = SOLAR_OS_NATIVE_MESHCORE_RADIO_ABI, .struct_size = sizeof(meshcore_radio_api),
    .provider_next = meshcore_radio_provider_next,
    .status = meshcore_radio_status,
    .identity_generate = meshcore_radio_identity_generate,
    .identity_import = meshcore_radio_identity_import,
    .identity_public = meshcore_radio_identity_public,
    .identity_export_private = meshcore_radio_identity_export_private,
    .get_name = meshcore_radio_get_name,
    .set_name = meshcore_radio_set_name,
    .channels = meshcore_radio_channels,
    .channel_add = meshcore_radio_channel_add,
    .channel_remove = meshcore_radio_channel_remove,
    .channel_public_set = meshcore_radio_channel_public_set,
    .advertise = meshcore_radio_advertise,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MESHCORE_BLE
#include "solar_os_meshcore_ble.h"

static bool meshcore_ble_provider_next(solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *status)
{
    return meshcore_provider_next(SOLAR_OS_MESHCORE_BACKEND_BLE, cursor, status);
}

static int meshcore_ble_status(solar_os_messaging_provider_id_t provider, solar_os_meshcore_ble_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *lease __attribute__((cleanup(meshcore_scope_end))) = NULL;
    esp_err_t error = solar_os_meshcore_instance_begin(SOLAR_OS_MESHCORE_BACKEND_BLE, provider, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    error = solar_os_meshcore_ble_get_status(status);
    if (error == ESP_OK) status->last_error = solar_os_native_result(status->last_error);
    return solar_os_native_result(error);
}

static const solar_os_native_meshcore_ble_api_v1_t meshcore_ble_api = {
    .abi_version = SOLAR_OS_NATIVE_MESHCORE_BLE_ABI, .struct_size = sizeof(meshcore_ble_api),
    .provider_next = meshcore_ble_provider_next,
    .status = meshcore_ble_status, .state_name = solar_os_meshcore_ble_state_name,
};
#endif

const void *solar_os_native_meshcore_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_meshcore_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
