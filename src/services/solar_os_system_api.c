#include "solar_os_system_api.h"
#include "solar_os_native_components.h"
#include "solar_os_native_application.h"
#include "solar_os_native_app_manifest.h"
#include "solar_os_native_vector.h"
#include "solar_os_native_dsp.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <math.h>

#include "esp_timer.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_native_thread_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_storage_abi.h"
#include "solar_os_native_registry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include "solar_os_native_driver_abi.h"
#include "solar_os_native_ble_abi.h"
#include "solar_os_native_job_abi.h"
#include "solar_os_native_media.h"
#include "solar_os_native_motion.h"
#include "solar_os_native_core.h"
#include "solar_os_native_fs.h"
#include "solar_os_native_network.h"
#include "solar_os_native_http.h"
#include "solar_os_native_routing.h"
#include "solar_os_native_remote.h"
#include "solar_os_native_stream.h"
#include "solar_os_native_controls_api.h"
#include "solar_os_native_io.h"
#include "solar_os_native_bus.h"
#include "solar_os_native_diagnostics.h"
#include "solar_os_native_xml.h"
#include "solar_os_native_storage_control.h"
#include "solar_os_native_device.h"
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLLERS
#include "solar_os_native_controllers.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble.h"
#endif
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

/* ESP-IDF gives every FreeRTOS task its own compiler TLS area. Keep the
 * execution-scope stack there so service calls need no cross-core lock or
 * task-list traversal. This does not consume a FreeRTOS TLS pointer slot. */
static _Thread_local native_run_scope_t *native_run_scope_top;

static native_run_scope_t *native_run_scope_current(void)
{
    return native_run_scope_top;
}

void solar_os_system_scope_enter(native_run_scope_t *scope)
{
    scope->next = native_run_scope_top;
    native_run_scope_top = scope;
}

void solar_os_system_scope_leave(native_run_scope_t *scope)
{
    native_run_scope_t **link = &native_run_scope_top;
    while (*link && *link != scope) link = &(*link)->next;
    if (*link) *link = scope->next;
}

void *solar_os_native_run_service_user(void)
{
    native_run_scope_t *scope = native_run_scope_current();
    return scope ? scope->options.service_user : NULL;
}

static const solar_os_native_library_service_api_v1_t native_library_api = {
    .abi_version = SOLAR_OS_NATIVE_LIBRARY_ABI,
    .struct_size = sizeof(native_library_api),
    .acquire = solar_os_native_library_acquire,
    .retain = solar_os_native_library_retain,
    .release = solar_os_native_library_release,
    .unload = solar_os_native_library_unload,
};

static void native_time_sleep_ms(uint32_t milliseconds)
{
    const uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
    vTaskDelay(ticks > portMAX_DELAY - 1U ? portMAX_DELAY - 1U : (TickType_t)ticks);
}
static const solar_os_native_time_api_v1_t native_time_api = {
    .abi_version = SOLAR_OS_NATIVE_TIME_ABI, .struct_size = sizeof(native_time_api),
    .now_us = esp_timer_get_time, .sleep_ms = native_time_sleep_ms,
};
static const solar_os_native_libc_api_v1_t native_libc_api = {
    .abi_version = SOLAR_OS_NATIVE_LIBC_ABI, .struct_size = sizeof(native_libc_api),
    .format = vsnprintf, .parse_float = strtof, .floor_value = floor,
    .exp_float = expf, .floor_float = floorf, .mod_float = fmodf,
    .log_float = logf, .pow_float = powf, .sqrt_float = sqrtf,
    .parse_double = strtod,
    .scale_double = ldexp,
};

static uint32_t native_system_service_count(void);
static int native_system_service_info(uint32_t index, solar_os_native_system_service_v1_t *info);
static const solar_os_native_system_api_v1_t native_system_api = {
    .abi_version = SOLAR_OS_NATIVE_SYSTEM_ABI, .struct_size = sizeof(native_system_api),
    .service_count = native_system_service_count, .service_info = native_system_service_info,
};

static int native_thread_mutex_create(void **out, uint32_t recursive)
{
    if (!out || recursive > 1U) return EINVAL;
    *out = NULL;
    pthread_mutex_t *mutex = solar_os_memory_alloc(sizeof(*mutex),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "native.mutex");
    if (!mutex) return ENOMEM;
    pthread_mutexattr_t attr;
    int error = pthread_mutexattr_init(&attr);
    if (error) { solar_os_memory_free(mutex); return error; }
    if (recursive) error = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (!error) error = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (error) { solar_os_memory_free(mutex); return error; }
    *out = mutex;
    return 0;
}
static int native_thread_mutex_lock(void *mutex)
{ return mutex ? pthread_mutex_lock(mutex) : EINVAL; }
static int native_thread_mutex_unlock(void *mutex)
{ return mutex ? pthread_mutex_unlock(mutex) : EINVAL; }
static int native_thread_mutex_destroy(void *mutex)
{
    if (!mutex) return EINVAL;
    const int error = pthread_mutex_destroy(mutex);
    if (!error) solar_os_memory_free(mutex);
    return error;
}
static int native_thread_key_create(uint32_t *out, void (*destructor)(void *))
{
    if (!out) return EINVAL;
    pthread_key_t key;
    const int error = pthread_key_create(&key, destructor);
    if (!error) *out = key;
    return error;
}
static int native_thread_key_delete(uint32_t key) { return pthread_key_delete(key); }
static void *native_thread_key_get(uint32_t key) { return pthread_getspecific(key); }
static int native_thread_key_set(uint32_t key, const void *value)
{ return pthread_setspecific(key, value); }
static int native_thread_condition_create(void **out)
{
    if (!out) return EINVAL;
    *out = NULL;
    pthread_cond_t *condition = solar_os_memory_alloc(sizeof(*condition),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "native.condition");
    if (!condition) return ENOMEM;
    int error = pthread_cond_init(condition, NULL);
    if (error) { solar_os_memory_free(condition); return error; }
    *out = condition;
    return 0;
}
static int native_thread_condition_wait(void *condition, void *mutex)
{ return condition && mutex ? pthread_cond_wait(condition, mutex) : EINVAL; }
static int native_thread_condition_broadcast(void *condition)
{ return condition ? pthread_cond_broadcast(condition) : EINVAL; }
static int native_thread_condition_destroy(void *condition)
{
    if (!condition) return EINVAL;
    int error = pthread_cond_destroy(condition);
    if (!error) solar_os_memory_free(condition);
    return error;
}
typedef struct {
    StaticTask_t task_buffer;
    TaskHandle_t task;
    void (*entry)(void *);
    void *argument;
    StackType_t stack[];
} native_thread_worker_t;
static void native_thread_worker_entry(void *argument)
{
    native_thread_worker_t *worker = argument;
    worker->entry(worker->argument);
    /* The joining task deletes a suspended worker. Its stack/TCB cannot be
     * released while the other CPU still executes the provider's epilogue. */
    vTaskSuspend(NULL);
}
static int native_thread_worker_start(void **out, void (*entry)(void *), void *argument,
                                     uint32_t stack_bytes, uint32_t core)
{
    if (!out) return EINVAL;
    *out = NULL;
    if (!entry || core >= CONFIG_FREERTOS_NUMBER_OF_CORES ||
        stack_bytes < 2048 || stack_bytes > 16384 || stack_bytes % sizeof(StackType_t))
        return EINVAL;
    native_thread_worker_t *worker = solar_os_memory_calloc(1, sizeof(*worker) + stack_bytes,
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "native.worker");
    if (!worker) return ENOMEM;
    worker->entry = entry; worker->argument = argument;
    worker->task = xTaskCreateStaticPinnedToCore(native_thread_worker_entry, "native_dl",
        stack_bytes, worker, uxTaskPriorityGet(NULL), worker->stack, &worker->task_buffer, core);
    if (!worker->task) { solar_os_memory_free(worker); return ENOMEM; }
    *out = worker;
    return 0;
}
static int native_thread_worker_join(void *value)
{
    native_thread_worker_t *worker = value;
    if (!worker || worker->task == xTaskGetCurrentTaskHandle()) return EINVAL;
    while (eTaskGetState(worker->task) != eSuspended) vTaskDelay(1);
    vTaskDelete(worker->task);
    solar_os_memory_free(worker);
    return 0;
}
static const solar_os_native_thread_api_v1_t native_thread_api = {
    .abi_version = SOLAR_OS_NATIVE_THREAD_ABI, .struct_size = sizeof(native_thread_api),
    .mutex_create = native_thread_mutex_create, .mutex_lock = native_thread_mutex_lock,
    .mutex_unlock = native_thread_mutex_unlock, .mutex_destroy = native_thread_mutex_destroy,
    .key_create = native_thread_key_create, .key_delete = native_thread_key_delete,
    .key_get = native_thread_key_get, .key_set = native_thread_key_set,
    .condition_create = native_thread_condition_create,
    .condition_wait = native_thread_condition_wait,
    .condition_broadcast = native_thread_condition_broadcast,
    .condition_destroy = native_thread_condition_destroy,
    .worker_start = native_thread_worker_start, .worker_join = native_thread_worker_join,
};

static const void *native_job_get_service(const char *name,
                                          uint32_t abi_version,
                                          uint32_t minimum_struct_size);

static esp_err_t native_write_utf8(const char *text, size_t text_len)
{
    native_run_scope_t *scope = native_run_scope_current();
    if (text == NULL || (text_len > 0U && (scope == NULL || scope->options.write == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (text_len == 0U) {
        return ESP_OK;
    }
    return scope->options.write(text, text_len, scope->options.write_user);
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

static const void *native_get_service(const char *name,
                                      uint32_t abi_version,
                                      uint32_t minimum_struct_size,
                                      bool scoped)
{
    const uint32_t abi = abi_version, minimum = minimum_struct_size;
    if (!name) return NULL;
#include "generated/native_root_early_dispatch.inc"
    native_run_scope_t *scope = scoped ? native_run_scope_current() : NULL;
    if (scope != NULL && scope->options.get_service != NULL) {
        const void *custom = scope->options.get_service(name, abi_version,
            minimum_struct_size, scope->options.service_user);
        if (custom != NULL) return custom;
    }
#include "generated/native_root_providers.inc"
#include "generated/native_root_late_dispatch.inc"
    return NULL;
}

static uint32_t native_system_service_count(void)
{
    return sizeof(native_service_descriptors) / sizeof(native_service_descriptors[0]);
}

static int native_system_service_info(uint32_t index, solar_os_native_system_service_v1_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    if (index >= native_system_service_count()) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const native_service_descriptor_t *entry = &native_service_descriptors[index];
    memcpy(info->name, entry->name, strlen(entry->name) + 1U);
    info->abi_version = entry->abi;
    info->flags = entry->flags;
    const void *api = native_get_service(entry->name, entry->abi, 8U, true);
    if (api) {
        uint32_t header[2];
        memcpy(header, api, sizeof(header));
        if (header[0] == entry->abi && header[1] >= sizeof(header)) {
            info->struct_size = header[1];
            info->flags |= SOLAR_OS_NATIVE_SYSTEM_AVAILABLE;
        }
    }
    return SOLAR_OS_NATIVE_OK;
}

static const void *native_job_get_service(const char *name, uint32_t abi, uint32_t size)
{
    return native_get_service(name, abi, size, true);
}

bool solar_os_native_service_available(const char *name, uint32_t abi, uint32_t size)
{
    if (name && !strcmp(name, SOLAR_OS_NATIVE_STORAGE_SERVICE))
        return abi == SOLAR_OS_NATIVE_STORAGE_ABI && size <= sizeof(solar_os_native_storage_api_v1_t);
    return native_get_service(name, abi, size, false) != NULL;
}

static const solar_os_native_job_host_api_v1_t native_job_host_v1 = {
    .abi_version = SOLAR_OS_NATIVE_JOB_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_job_host_api_v1_t),
    .target = CONFIG_IDF_TARGET,
    .firmware_version = SOLAR_OS_VERSION,
    .register_job = solar_os_native_register_job,
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
    .register_driver = solar_os_native_register_driver,
};

const solar_os_native_driver_host_api_v1_t *solar_os_native_driver_host_v1(void)
{
    return &native_driver_host_v1;
}

static const solar_os_native_library_host_api_v1_t native_library_host_v1 = {
    .abi_version = SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI,
    .struct_size = sizeof(native_library_host_v1),
    .target = CONFIG_IDF_TARGET,
    .firmware_version = SOLAR_OS_VERSION,
    .register_library = solar_os_native_register_library,
    .get_service = native_job_get_service,
};

const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void)
{
    return &native_library_host_v1;
}

static esp_err_t native_run_firmware(int (*entry)(int, char **), int argc,
                                      char **argv,
                                      const solar_os_native_run_options_t *options,
                                      int *exit_status)
{
    if (!entry || argc < 1 || !argv || !options || !options->write ||
        !exit_status) return ESP_ERR_INVALID_ARG;
    native_run_scope_t scope = {.options = *options};
    solar_os_system_scope_enter(&scope);
    *exit_status = entry(argc, argv);
    if (options->cleanup) options->cleanup(options->service_user);
    solar_os_system_scope_leave(&scope);
    return ESP_OK;
}

esp_err_t solar_os_native_run_builtin(int (*entry)(int, char **), int argc,
    char **argv, const solar_os_native_run_options_t *options, int *exit_status)
{
    if (!options || options->expected_manifest) return ESP_ERR_INVALID_ARG;
    return native_run_firmware(entry, argc, argv, options, exit_status);
}

esp_err_t solar_os_native_run_compiled_app(const solar_os_component_t *component,
    int argc, char **argv, const solar_os_native_run_options_t *options, int *exit_status)
{
    if (!component || !component->id || component->type != SOLAR_OS_MODULE_TYPE_APP ||
        component != solar_os_component_find(SOLAR_OS_MODULE_TYPE_APP, component->id) ||
        component->lifecycle_abi != 2U || !solar_os_native_app_manifest_valid(component->manifest) ||
        strcmp(component->id, component->manifest->id) || !options || !options->expected_manifest ||
        memcmp(component->manifest, options->expected_manifest, sizeof(*component->manifest)))
        return ESP_ERR_INVALID_ARG;
    return native_run_firmware(component->entry, argc, argv, options, exit_status);
}
