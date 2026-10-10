#include "solar_os_native.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_elf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "solar_os_config.h"
#include "solar_os_jobs.h"
#include "solar_os_memory.h"
#include "solar_os_native_driver_abi.h"
#include "solar_os_native_ble_abi.h"
#include "solar_os_native_job_abi.h"
#include "solar_os_native_media.h"
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_expansion.h"
#endif
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

static atomic_bool native_running;
static atomic_bool native_symbols_registered;
static solar_os_native_run_options_t native_active_options;

#define SOLAR_OS_NATIVE_RESIDENT_MAX SOLAR_OS_MODULE_PACKAGE_COUNT_MAX
#define SOLAR_OS_NATIVE_SUMMARY_MAX 96U

typedef struct native_resident_slot {
    struct native_resident_slot *next;
    bool active;
    bool deactivating;
    bool registered;
    size_t callback_refs;
    solar_os_module_type_t type;
    char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    char summary[SOLAR_OS_NATIVE_SUMMARY_MAX];
    esp_elf_t elf;
    bool elf_initialized;
    solar_os_native_elf_info_t elf_info;
    solar_os_native_job_descriptor_v1_t job_descriptor;
    solar_os_native_driver_descriptor_v1_t driver_descriptor;
    solar_os_job_t job;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    solar_os_expansion_driver_t driver;
#endif
} native_resident_slot_t;

static native_resident_slot_t *native_resident_slots;
static native_resident_slot_t *native_registering_slot;
static portMUX_TYPE native_resident_lock = portMUX_INITIALIZER_UNLOCKED;

static int native_register_job(
    const solar_os_native_job_descriptor_v1_t *descriptor);
static int native_register_driver(
    const solar_os_native_driver_descriptor_v1_t *descriptor);
static const void *native_job_get_service(const char *name,
                                          uint32_t abi_version,
                                          uint32_t minimum_struct_size);

static esp_err_t native_write_utf8(const char *text, size_t text_len)
{
    if (text == NULL || (text_len > 0U && native_active_options.write == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (text_len == 0U) {
        return ESP_OK;
    }
    return native_active_options.write(text,
                                       text_len,
                                       native_active_options.write_user);
}

static const solar_os_native_host_api_v1_t native_host_v1 = {
    .abi_version = SOLAR_OS_NATIVE_ABI_VERSION,
    .struct_size = sizeof(solar_os_native_host_api_v1_t),
    .target = CONFIG_IDF_TARGET,
    .firmware_version = SOLAR_OS_VERSION,
    .write_utf8 = native_write_utf8,
    .get_service = native_job_get_service,
};

const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void)
{
    return &native_host_v1;
}

#if SOLAR_OS_PACKAGE_SERVICE_BLE
_Static_assert(SOLAR_OS_NATIVE_BLE_VALUE_MAX == SOLAR_OS_BLE_GATT_VALUE_MAX,
               "native BLE value ABI must match the service");
_Static_assert(SOLAR_OS_NATIVE_BLE_NAME_MAX == SOLAR_OS_BLE_NAME_MAX,
               "native BLE name ABI must match the service");
_Static_assert(SOLAR_OS_NATIVE_BLE_UUID_MAX == SOLAR_OS_BLE_GATT_UUID_MAX,
               "native BLE UUID ABI must match the service");

static int native_ble_result(esp_err_t result)
{
    switch (result) {
    case ESP_OK: return SOLAR_OS_NATIVE_BLE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_BLE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_BLE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_BLE_ERROR_TIMEOUT;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_SIZE;
    case SOLAR_OS_BLE_ERR_CANCELLED: return SOLAR_OS_NATIVE_BLE_ERROR_CANCELLED;
    case SOLAR_OS_BLE_ERR_CAPACITY: return SOLAR_OS_NATIVE_BLE_ERROR_CAPACITY;
    default: return SOLAR_OS_NATIVE_BLE_ERROR_FAILED;
    }
}

static int native_ble_init(void)
{
    return native_ble_result(solar_os_ble_init());
}

static int native_ble_scan(solar_os_native_ble_scan_result_v1_t *results,
                           size_t max_results, size_t *found)
{
    if (found == NULL || max_results > SOLAR_OS_BLE_SCAN_MAX_RESULTS ||
        (max_results != 0U && results == NULL)) {
        return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    }
    solar_os_ble_scan_result_t *temporary = max_results != 0U ?
        solar_os_memory_calloc(max_results, sizeof(*temporary),
                               SOLAR_OS_MEMORY_TRANSIENT, "native.ble.scan") : NULL;
    if (max_results != 0U && temporary == NULL) {
        return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    }
    const esp_err_t ret = solar_os_ble_scan(temporary, max_results, found);
    if (ret == ESP_OK) {
        const size_t count = *found < max_results ? *found : max_results;
        for (size_t i = 0; i < count; ++i) {
            memcpy(results[i].bda, temporary[i].bda, sizeof(results[i].bda));
            results[i].addr_type = temporary[i].addr_type;
            results[i].rssi = temporary[i].rssi;
            results[i].appearance = temporary[i].appearance;
            results[i].hid_service = temporary[i].hid_service;
            results[i].keyboard_like = temporary[i].keyboard_like;
            results[i].remembered = temporary[i].remembered;
            results[i].connected = temporary[i].connected;
            memcpy(results[i].name, temporary[i].name, sizeof(results[i].name));
        }
    }
    solar_os_memory_free(temporary);
    return native_ble_result(ret);
}

static int native_ble_session_create(const char *owner,
                                     solar_os_native_ble_session_t *session)
{
    return native_ble_result(solar_os_ble_session_create(owner, session));
}

static int native_ble_session_cancel(solar_os_native_ble_session_t session)
{
    return native_ble_result(solar_os_ble_session_cancel(session));
}

static int native_ble_session_close(solar_os_native_ble_session_t session)
{
    return native_ble_result(solar_os_ble_session_close(session));
}

static int native_ble_peer_connect(solar_os_native_ble_session_t session,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms,
    solar_os_native_ble_peer_t *peer)
{
    return native_ble_result(solar_os_ble_peer_connect(session, bda, addr_type,
                                                        timeout_ms, peer));
}

static int native_ble_peer_disconnect(solar_os_native_ble_session_t session,
                                      solar_os_native_ble_peer_t peer)
{
    return native_ble_result(solar_os_ble_peer_disconnect(session, peer));
}

static int native_ble_peer_pair(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint32_t passkey, uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_pair(session, peer, passkey,
                                                     timeout_ms));
}

static int native_ble_peer_info(solar_os_native_ble_session_t session,
                                solar_os_native_ble_peer_t peer,
                                solar_os_native_ble_peer_info_v1_t *info)
{
    if (info == NULL) return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_session_info_t source = {0};
    const esp_err_t ret = solar_os_ble_peer_get_info(session, peer, &source);
    if (ret != ESP_OK) return native_ble_result(ret);
    memset(info, 0, sizeof(*info));
    memcpy(info->owner, source.owner, sizeof(info->owner));
    info->busy = source.busy;
    info->retiring = source.retiring;
    info->event_capacity = source.event_capacity;
    info->event_count = source.event_count;
    info->events_dropped = source.events_dropped;
    info->gatt.connected = source.gatt.connected;
    info->gatt.encrypted = source.gatt.encrypted;
    info->gatt.bonded = source.gatt.bonded;
    memcpy(info->gatt.bda, source.gatt.bda, sizeof(info->gatt.bda));
    info->gatt.addr_type = source.gatt.addr_type;
    info->gatt.conn_id = source.gatt.conn_id;
    info->gatt.mtu = source.gatt.mtu;
    info->gatt.service_count = source.gatt.service_count;
    memcpy(info->gatt.status, source.gatt.status, sizeof(info->gatt.status));
    return SOLAR_OS_NATIVE_BLE_OK;
}

static int native_ble_peer_services(solar_os_native_ble_session_t session,
                                    solar_os_native_ble_peer_t peer,
                                    solar_os_native_ble_service_v1_t *services,
                                    size_t max_services, size_t *count)
{
    if (count == NULL || max_services > SOLAR_OS_BLE_GATT_MAX_SERVICES ||
        (max_services != 0U && services == NULL))
        return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_gatt_service_t *temporary = max_services != 0U ?
        solar_os_memory_calloc(max_services, sizeof(*temporary),
                               SOLAR_OS_MEMORY_TRANSIENT, "native.ble.svcs") : NULL;
    if (max_services != 0U && temporary == NULL)
        return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    const esp_err_t ret = solar_os_ble_peer_services(session, peer, temporary,
                                                      max_services, count);
    if (ret == ESP_OK) {
        const size_t copied = *count < max_services ? *count : max_services;
        for (size_t i = 0; i < copied; ++i) {
            services[i].start_handle = temporary[i].start_handle;
            services[i].end_handle = temporary[i].end_handle;
            services[i].primary = temporary[i].primary;
            memcpy(services[i].uuid, temporary[i].uuid, sizeof(services[i].uuid));
        }
    }
    solar_os_memory_free(temporary);
    return native_ble_result(ret);
}

static int native_ble_peer_characteristics(
    solar_os_native_ble_session_t session, solar_os_native_ble_peer_t peer,
    size_t service_index, solar_os_native_ble_characteristic_v1_t *chars,
    size_t max_chars, size_t *count)
{
    if (count == NULL || max_chars > SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS ||
        (max_chars != 0U && chars == NULL))
        return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_gatt_characteristic_t *temporary = max_chars != 0U ?
        solar_os_memory_calloc(max_chars, sizeof(*temporary),
                               SOLAR_OS_MEMORY_TRANSIENT, "native.ble.chars") : NULL;
    if (max_chars != 0U && temporary == NULL)
        return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    const esp_err_t ret = solar_os_ble_peer_characteristics(session, peer,
        service_index, temporary, max_chars, count);
    if (ret == ESP_OK) {
        const size_t copied = *count < max_chars ? *count : max_chars;
        for (size_t i = 0; i < copied; ++i) {
            chars[i].handle = temporary[i].handle;
            chars[i].properties = temporary[i].properties;
            memcpy(chars[i].uuid, temporary[i].uuid, sizeof(chars[i].uuid));
        }
    }
    solar_os_memory_free(temporary);
    return native_ble_result(ret);
}

static int native_ble_peer_configure_queue(
    solar_os_native_ble_session_t session, solar_os_native_ble_peer_t peer,
    size_t capacity)
{
    return native_ble_result(solar_os_ble_peer_configure_queue(session, peer,
                                                                capacity));
}

static int native_ble_peer_subscribe(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint16_t handle, uint8_t mode,
    uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_subscribe(session, peer, handle,
                                                          mode, timeout_ms));
}

static int native_ble_peer_poll(solar_os_native_ble_session_t session,
                                solar_os_native_ble_peer_t peer,
                                solar_os_native_ble_notification_v1_t *event)
{
    if (event == NULL) return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_notification_t source = {0};
    const esp_err_t ret = solar_os_ble_peer_poll(session, peer, &source);
    if (ret != ESP_OK) return native_ble_result(ret);
    event->handle = source.handle;
    event->indication = source.indication;
    event->value_len = source.value_len;
    memcpy(event->value, source.value, source.value_len);
    return SOLAR_OS_NATIVE_BLE_OK;
}

static int native_ble_peer_read(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint16_t handle, uint8_t *value,
    size_t max_len, size_t *value_len, uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_read(session, peer, handle,
        value, max_len, value_len, timeout_ms));
}

static int native_ble_peer_write(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint16_t handle, const uint8_t *value,
    size_t value_len, bool with_response, uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_write(session, peer, handle,
        value, value_len, with_response, timeout_ms));
}

static const solar_os_native_ble_client_api_v1_t native_ble_client_v1 = {
    .abi_version = SOLAR_OS_NATIVE_BLE_CLIENT_ABI,
    .struct_size = sizeof(solar_os_native_ble_client_api_v1_t),
    .max_value_size = SOLAR_OS_BLE_GATT_VALUE_MAX,
    .init = native_ble_init,
    .scan = native_ble_scan,
    .session_create = native_ble_session_create,
    .session_cancel = native_ble_session_cancel,
    .session_close = native_ble_session_close,
    .peer_capacity = solar_os_ble_peer_capacity,
    .peer_connect = native_ble_peer_connect,
    .peer_disconnect = native_ble_peer_disconnect,
    .peer_pair = native_ble_peer_pair,
    .peer_get_info = native_ble_peer_info,
    .peer_services = native_ble_peer_services,
    .peer_characteristics = native_ble_peer_characteristics,
    .peer_configure_queue = native_ble_peer_configure_queue,
    .peer_subscribe = native_ble_peer_subscribe,
    .peer_poll = native_ble_peer_poll,
    .peer_read = native_ble_peer_read,
    .peer_write = native_ble_peer_write,
};
#endif

static const void *native_job_get_service(const char *name,
                                          uint32_t abi_version,
                                          uint32_t minimum_struct_size)
{
    const void *service = solar_os_native_media_get_service(name, abi_version,
                                                           minimum_struct_size);
    if (service != NULL) return service;
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (name != NULL &&
        strcmp(name, SOLAR_OS_NATIVE_BLE_CLIENT_SERVICE) == 0 &&
        abi_version == SOLAR_OS_NATIVE_BLE_CLIENT_ABI &&
        minimum_struct_size <= sizeof(native_ble_client_v1)) {
        return &native_ble_client_v1;
    }
#else
    (void)name;
    (void)abi_version;
    (void)minimum_struct_size;
#endif
    return NULL;
}

static const solar_os_native_job_host_api_v1_t native_job_host_v1 = {
    .abi_version = SOLAR_OS_NATIVE_JOB_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_job_host_api_v1_t),
    .target = CONFIG_IDF_TARGET,
    .firmware_version = SOLAR_OS_VERSION,
    .register_job = native_register_job,
    .get_service = native_job_get_service,
};

const solar_os_native_job_host_api_v1_t *solar_os_native_job_host_v1(void)
{
    return &native_job_host_v1;
}

static const solar_os_native_driver_host_api_v1_t native_driver_host_v1 = {
    .abi_version = SOLAR_OS_NATIVE_DRIVER_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_driver_host_api_v1_t),
    .target = CONFIG_IDF_TARGET,
    .firmware_version = SOLAR_OS_VERSION,
    .register_driver = native_register_driver,
};

const solar_os_native_driver_host_api_v1_t *solar_os_native_driver_host_v1(void)
{
    return &native_driver_host_v1;
}

static const struct esp_elfsym native_symbols[] = {
    ESP_ELFSYM_EXPORT(solar_os_native_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_job_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_driver_host_v1),
    /* Compilers emit these standard calls even for struct assignment and
     * simple loops. Keep the native substrate independent of broad SDK exports. */
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_END,
};

static void native_set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", text != NULL ? text : "native module failed");
    }
}

static esp_err_t native_register_symbols(char *detail, size_t detail_len)
{
    if (atomic_load(&native_symbols_registered)) {
        return ESP_OK;
    }
    const int ret = esp_elf_register_symbol(native_symbols);
    if (ret != 0) {
        native_set_detail(detail, detail_len, "could not register the SolarOS native ABI");
        return ESP_FAIL;
    }
    atomic_store(&native_symbols_registered, true);
    return ESP_OK;
}

static bool native_descriptor_text_valid(const char *text, size_t maximum)
{
    return text != NULL && text[0] != '\0' && strnlen(text, maximum) < maximum;
}

static int native_register_job(
    const solar_os_native_job_descriptor_v1_t *descriptor)
{
    native_resident_slot_t *slot = native_registering_slot;
    if (slot == NULL || slot->type != SOLAR_OS_MODULE_TYPE_JOB ||
        descriptor == NULL ||
        descriptor->abi_version != SOLAR_OS_NATIVE_JOB_LIFECYCLE_ABI ||
        descriptor->struct_size < sizeof(*descriptor) ||
        !native_descriptor_text_valid(descriptor->id,
                                      SOLAR_OS_MODULE_PACKAGE_ID_MAX) ||
        strcmp(descriptor->id, slot->id) != 0 ||
        !native_descriptor_text_valid(descriptor->summary,
                                      SOLAR_OS_NATIVE_SUMMARY_MAX) ||
        descriptor->start == NULL || descriptor->stop == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    slot->job_descriptor = *descriptor;
    snprintf(slot->summary, sizeof(slot->summary), "%s", descriptor->summary);
    slot->registered = true;
    return ESP_OK;
}

static int native_register_driver(
    const solar_os_native_driver_descriptor_v1_t *descriptor)
{
    native_resident_slot_t *slot = native_registering_slot;
    if (slot == NULL || slot->type != SOLAR_OS_MODULE_TYPE_DRIVER ||
        descriptor == NULL ||
        descriptor->abi_version != SOLAR_OS_NATIVE_DRIVER_LIFECYCLE_ABI ||
        descriptor->struct_size < sizeof(*descriptor) ||
        !native_descriptor_text_valid(descriptor->id,
                                      SOLAR_OS_MODULE_PACKAGE_ID_MAX) ||
        strcmp(descriptor->id, slot->id) != 0 ||
        !native_descriptor_text_valid(descriptor->summary,
                                      SOLAR_OS_NATIVE_SUMMARY_MAX) ||
        descriptor->attach == NULL || descriptor->detach == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    slot->driver_descriptor = *descriptor;
    snprintf(slot->summary, sizeof(slot->summary), "%s", descriptor->summary);
    slot->registered = true;
    return ESP_OK;
}

static bool native_slot_acquire(native_resident_slot_t *slot)
{
    bool acquired = false;
    portENTER_CRITICAL(&native_resident_lock);
    if (slot != NULL && slot->active && !slot->deactivating) {
        slot->callback_refs++;
        acquired = true;
    }
    portEXIT_CRITICAL(&native_resident_lock);
    return acquired;
}

static void native_slot_release(native_resident_slot_t *slot)
{
    portENTER_CRITICAL(&native_resident_lock);
    if (slot != NULL && slot->callback_refs > 0U) {
        slot->callback_refs--;
    }
    portEXIT_CRITICAL(&native_resident_lock);
}

static esp_err_t native_job_start_callback(void *user,
                                           solar_os_context_t *ctx,
                                           int argc,
                                           char **argv)
{
    (void)ctx;
    native_resident_slot_t *slot = (native_resident_slot_t *)user;
    if (!native_slot_acquire(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int result = slot->job_descriptor.start(argc, argv);
    native_slot_release(slot);
    return (esp_err_t)result;
}

static void native_job_stop_callback(void *user, solar_os_context_t *ctx)
{
    (void)ctx;
    native_resident_slot_t *slot = (native_resident_slot_t *)user;
    if (!native_slot_acquire(slot)) {
        return;
    }
    slot->job_descriptor.stop();
    native_slot_release(slot);
}

static bool native_job_event_callback(void *user,
                                      solar_os_context_t *ctx,
                                      const solar_os_event_t *event)
{
    (void)ctx;
    native_resident_slot_t *slot = (native_resident_slot_t *)user;
    if (event == NULL || event->type != SOLAR_OS_EVENT_TICK ||
        slot == NULL || slot->job_descriptor.tick == NULL ||
        !native_slot_acquire(slot)) {
        return false;
    }
    const bool handled = slot->job_descriptor.tick(event->data.tick_ms);
    native_slot_release(slot);
    return handled;
}

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
static esp_err_t native_driver_attach_callback(
    void *user,
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    (void)bindings;
    native_resident_slot_t *slot = (native_resident_slot_t *)user;
    if (binding_count != 0U || !native_slot_acquire(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int result = slot->driver_descriptor.attach(name);
    native_slot_release(slot);
    return (esp_err_t)result;
}

static esp_err_t native_driver_detach_callback(void *user, const char *name)
{
    native_resident_slot_t *slot = (native_resident_slot_t *)user;
    if (!native_slot_acquire(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int result = slot->driver_descriptor.detach(name);
    native_slot_release(slot);
    return (esp_err_t)result;
}
#endif

static native_resident_slot_t *native_find_slot(solar_os_module_type_t type,
                                                const char *id)
{
    native_resident_slot_t *found = NULL;
    portENTER_CRITICAL(&native_resident_lock);
    for (native_resident_slot_t *slot = native_resident_slots;
         slot != NULL;
         slot = slot->next) {
        if (slot->active && slot->type == type && strcmp(slot->id, id) == 0) {
            found = slot;
            break;
        }
    }
    portEXIT_CRITICAL(&native_resident_lock);
    return found;
}

static size_t native_slot_count(void)
{
    size_t count = 0U;
    portENTER_CRITICAL(&native_resident_lock);
    for (const native_resident_slot_t *slot = native_resident_slots;
         slot != NULL;
         slot = slot->next) {
        count++;
    }
    portEXIT_CRITICAL(&native_resident_lock);
    return count;
}

static bool native_slot_add(native_resident_slot_t *slot)
{
    if (slot == NULL) {
        return false;
    }
    bool added = false;
    portENTER_CRITICAL(&native_resident_lock);
    size_t count = 0U;
    for (const native_resident_slot_t *current = native_resident_slots;
         current != NULL;
         current = current->next) {
        count++;
    }
    if (count < SOLAR_OS_NATIVE_RESIDENT_MAX) {
        slot->next = native_resident_slots;
        slot->active = true;
        native_resident_slots = slot;
        added = true;
    }
    portEXIT_CRITICAL(&native_resident_lock);
    return added;
}

static void native_slot_remove(native_resident_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    portENTER_CRITICAL(&native_resident_lock);
    native_resident_slot_t **current = &native_resident_slots;
    while (*current != NULL) {
        if (*current == slot) {
            *current = slot->next;
            slot->next = NULL;
            slot->active = false;
            break;
        }
        current = &(*current)->next;
    }
    portEXIT_CRITICAL(&native_resident_lock);
}

static esp_err_t native_load_resident(native_resident_slot_t *slot,
                                      const char *path,
                                      char *detail,
                                      size_t detail_len)
{
    solar_os_storage_metadata_t metadata;
    esp_err_t err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK || metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
        native_set_detail(detail, detail_len, "ELF file was not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (metadata.size_bytes < 52U ||
        metadata.size_bytes > SOLAR_OS_NATIVE_ELF_MAX_BYTES ||
        metadata.size_bytes > SIZE_MAX) {
        native_set_detail(detail, detail_len, "ELF file size is outside the supported range");
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t file_size = (size_t)metadata.size_bytes;
    uint8_t *data = solar_os_memory_alloc(file_size,
                                          SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                          "native.resident.elf");
    if (data == NULL) {
        native_set_detail(detail, detail_len, "not enough external memory for the ELF file");
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = 0U;
    err = solar_os_storage_read_file(path, data, file_size, &read_len);
    if (err != ESP_OK || read_len != file_size) {
        native_set_detail(detail, detail_len, "could not read the complete ELF file");
        err = ESP_FAIL;
        goto done;
    }
    err = solar_os_native_elf_validate(data,
                                       file_size,
                                       SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                       &slot->elf_info,
                                       detail,
                                       detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    err = native_register_symbols(detail, detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    if (esp_elf_init(&slot->elf) != 0) {
        native_set_detail(detail, detail_len, "Espressif ELF loader initialization failed");
        err = ESP_FAIL;
        goto done;
    }
    slot->elf_initialized = true;
    if (esp_elf_relocate(&slot->elf, data) != 0) {
        native_set_detail(detail, detail_len, "ELF relocation or symbol resolution failed");
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    char *argv[] = {slot->id, NULL};
    native_registering_slot = slot;
    const int loader_ret = esp_elf_request(&slot->elf, 0, 1, argv);
    native_registering_slot = NULL;
    if (loader_ret != 0 || !slot->registered) {
        native_set_detail(detail,
                          detail_len,
                          "ELF did not register the expected module lifecycle");
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    err = ESP_OK;

done:
    native_registering_slot = NULL;
    solar_os_memory_free(data);
    if (err != ESP_OK && slot->elf_initialized) {
        esp_elf_deinit(&slot->elf);
        slot->elf_initialized = false;
    }
    return err;
}

static esp_err_t native_register_resident(native_resident_slot_t *slot)
{
    if (slot->type == SOLAR_OS_MODULE_TYPE_JOB) {
        slot->job = (solar_os_job_t) {
            .name = slot->id,
            .summary = slot->summary,
            .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
            .tick_interval_ms = slot->job_descriptor.tick_interval_ms,
            .tick_deadline_ms = slot->job_descriptor.tick_deadline_ms,
            .callback_user = slot,
            .start_with_user = native_job_start_callback,
            .stop_with_user = native_job_stop_callback,
            .event_with_user = slot->job_descriptor.tick != NULL ?
                native_job_event_callback : NULL,
        };
        return solar_os_jobs_register_dynamic(slot->id,
                                              slot->summary,
                                              &slot->job);
    }
    if (slot->type == SOLAR_OS_MODULE_TYPE_DRIVER) {
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        if (strlen(slot->id) >= SOLAR_OS_EXPANSION_DRIVER_NAME_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        slot->driver = (solar_os_expansion_driver_t) {
            .name = slot->id,
            .summary = slot->summary,
            .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
            .callback_user = slot,
            .attach_with_user = native_driver_attach_callback,
            .detach_with_user = native_driver_detach_callback,
        };
        return solar_os_expansion_register_driver(&slot->driver);
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t native_unregister_resident(native_resident_slot_t *slot)
{
    if (slot->type == SOLAR_OS_MODULE_TYPE_JOB) {
        return solar_os_jobs_unregister_dynamic(slot->id, &slot->job);
    }
    if (slot->type == SOLAR_OS_MODULE_TYPE_DRIVER) {
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        return solar_os_expansion_unregister_driver(&slot->driver);
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_native_run(const char *path,
                              int argc,
                              char **argv,
                              const solar_os_native_run_options_t *options,
                              solar_os_native_run_result_t *result,
                              char *detail,
                              size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || argc < 1 || argv == NULL ||
        options == NULL || options->write == NULL) {
        native_set_detail(detail, detail_len, "missing path, arguments, or output callback");
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&native_running, true)) {
        native_set_detail(detail, detail_len, "another native module is already running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    uint8_t *data = NULL;
    esp_elf_t elf;
    bool elf_initialized = false;
    solar_os_storage_metadata_t metadata;
    solar_os_native_elf_info_t elf_info;

    err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK || metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
        native_set_detail(detail, detail_len, "ELF file was not found");
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (metadata.size_bytes < 52U ||
        metadata.size_bytes > SOLAR_OS_NATIVE_ELF_MAX_BYTES ||
        metadata.size_bytes > SIZE_MAX) {
        native_set_detail(detail, detail_len, "ELF file size is outside the supported range");
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    const size_t file_size = (size_t)metadata.size_bytes;
    data = solar_os_memory_alloc(file_size,
                                 SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                 "native.elf");
    if (data == NULL) {
        native_set_detail(detail, detail_len, "not enough external memory for the ELF file");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t read_len = 0U;
    err = solar_os_storage_read_file(path, data, file_size, &read_len);
    if (err != ESP_OK || read_len != file_size) {
        native_set_detail(detail, detail_len, "could not read the complete ELF file");
        err = ESP_FAIL;
        goto done;
    }

    err = solar_os_native_elf_validate(data,
                                       file_size,
                                       SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                       &elf_info,
                                       detail,
                                       detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    err = native_register_symbols(detail, detail_len);
    if (err != ESP_OK) {
        goto done;
    }

    int loader_ret = esp_elf_init(&elf);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "Espressif ELF loader initialization failed");
        err = ESP_FAIL;
        goto done;
    }
    elf_initialized = true;

    loader_ret = esp_elf_relocate(&elf, data);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "ELF relocation or symbol resolution failed");
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    native_active_options = *options;
    loader_ret = esp_elf_request(&elf, 0, argc, argv);
    memset(&native_active_options, 0, sizeof(native_active_options));
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "ELF entry point failed to run");
        err = ESP_FAIL;
        goto done;
    }

    if (result != NULL) {
        *result = (solar_os_native_run_result_t){.elf = elf_info};
    }

done:
    memset(&native_active_options, 0, sizeof(native_active_options));
    if (elf_initialized) {
        esp_elf_deinit(&elf);
    }
    solar_os_memory_free(data);
    atomic_store(&native_running, false);
    return err;
}

esp_err_t solar_os_native_module_activate(solar_os_module_type_t type,
                                          const char *id,
                                          const char *path,
                                          char *detail,
                                          size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if ((type != SOLAR_OS_MODULE_TYPE_JOB &&
         type != SOLAR_OS_MODULE_TYPE_DRIVER) ||
        id == NULL || id[0] == '\0' ||
        strnlen(id, SOLAR_OS_MODULE_PACKAGE_ID_MAX) >=
            SOLAR_OS_MODULE_PACKAGE_ID_MAX ||
        path == NULL || path[0] == '\0') {
        native_set_detail(detail, detail_len, "invalid resident module request");
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&native_running, true)) {
        native_set_detail(detail, detail_len, "another native module operation is running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    native_resident_slot_t *slot = NULL;
    bool slot_linked = false;
    if (native_find_slot(type, id) != NULL) {
        goto done;
    }
    if (native_slot_count() >= SOLAR_OS_NATIVE_RESIDENT_MAX) {
        native_set_detail(detail, detail_len, "no resident native module slot is available");
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    slot = solar_os_memory_calloc(1,
                                  sizeof(*slot),
                                  SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                  "native.resident.slot");
    if (slot == NULL) {
        native_set_detail(detail, detail_len, "not enough external memory for the resident module");
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    slot->type = type;
    snprintf(slot->id, sizeof(slot->id), "%s", id);

    err = native_load_resident(slot, path, detail, detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    slot_linked = native_slot_add(slot);
    if (!slot_linked) {
        native_set_detail(detail, detail_len, "no resident native module slot is available");
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    err = native_register_resident(slot);
    if (err != ESP_OK) {
        native_set_detail(detail,
                          detail_len,
                          err == ESP_ERR_NOT_SUPPORTED ?
                              "module lifecycle is not supported by this firmware" :
                              "module could not be registered with SolarOS");
        goto done;
    }
    slot = NULL;

done:
    if (slot != NULL) {
        if (slot_linked) {
            native_slot_remove(slot);
        }
        if (slot->elf_initialized) {
            esp_elf_deinit(&slot->elf);
        }
        solar_os_memory_free(slot);
    }
    atomic_store(&native_running, false);
    return err;
}

esp_err_t solar_os_native_module_deactivate(solar_os_module_type_t type,
                                            const char *id,
                                            char *detail,
                                            size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (id == NULL || id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&native_running, true)) {
        native_set_detail(detail, detail_len, "another native module operation is running");
        return ESP_ERR_INVALID_STATE;
    }
    native_resident_slot_t *slot = native_find_slot(type, id);
    if (slot == NULL) {
        atomic_store(&native_running, false);
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&native_resident_lock);
    slot->deactivating = true;
    portEXIT_CRITICAL(&native_resident_lock);
    esp_err_t err = native_unregister_resident(slot);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&native_resident_lock);
        slot->deactivating = false;
        portEXIT_CRITICAL(&native_resident_lock);
        native_set_detail(detail,
                          detail_len,
                          type == SOLAR_OS_MODULE_TYPE_JOB ?
                              "stop the job before removing its module" :
                              "detach all devices before removing their driver module");
        atomic_store(&native_running, false);
        return err;
    }

    for (;;) {
        portENTER_CRITICAL(&native_resident_lock);
        const size_t refs = slot->callback_refs;
        portEXIT_CRITICAL(&native_resident_lock);
        if (refs == 0U) {
            break;
        }
        vTaskDelay(1);
    }
    native_slot_remove(slot);
    if (slot->elf_initialized) {
        esp_elf_deinit(&slot->elf);
    }
    solar_os_memory_free(slot);
    atomic_store(&native_running, false);
    return ESP_OK;
}

bool solar_os_native_module_active(solar_os_module_type_t type,
                                   const char *id)
{
    if (id == NULL) {
        return false;
    }
    const bool active = native_find_slot(type, id) != NULL;
    return active;
}
