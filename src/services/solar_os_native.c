#include "solar_os_native.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <math.h>

#include "esp_elf.h"
#include "esp_timer.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_native_thread_abi.h"
#include "solar_os_native_libc_abi.h"
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
static size_t native_reclaim_unused_libraries(void);
typedef struct native_run_scope {
    struct native_run_scope *next;
    TaskHandle_t task;
    solar_os_native_run_options_t options;
} native_run_scope_t;

/* ESP-IDF gives every FreeRTOS task its own compiler TLS area. Keep the
 * execution-scope stack there so service calls need no cross-core lock or
 * task-list traversal. This does not consume a FreeRTOS TLS pointer slot. */
static _Thread_local native_run_scope_t *native_run_scope_top;

static native_run_scope_t *native_run_scope_current(void)
{
    return native_run_scope_top;
}

static void native_run_scope_add(native_run_scope_t *scope)
{
    scope->next = native_run_scope_top;
    native_run_scope_top = scope;
}

static void native_run_scope_remove(native_run_scope_t *scope)
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
    solar_os_native_library_descriptor_v1_t library_descriptor;
    solar_os_native_library_api_header_v1_t library_api_header;
    solar_os_job_t job;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    solar_os_expansion_driver_t driver;
#endif
} native_resident_slot_t;

static native_resident_slot_t *native_resident_slots;
static native_resident_slot_t *native_registering_slot;
static TaskHandle_t native_registering_task;
static portMUX_TYPE native_resident_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct native_library_lease {
    struct native_library_lease *next;
    native_resident_slot_t *slot;
    uint32_t id;
} native_library_lease_t;
static native_library_lease_t *native_library_leases;
static uint32_t native_library_next_lease;
#define SOLAR_OS_NATIVE_LIBRARY_LEASES_MAX 128U
static size_t native_library_lease_count;

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

static int native_register_job(
    const solar_os_native_job_descriptor_v1_t *descriptor);
static int native_register_driver(
    const solar_os_native_driver_descriptor_v1_t *descriptor);
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

static const void *native_job_get_service(const char *name,
                                          uint32_t abi_version,
                                          uint32_t minimum_struct_size)
{
    if (name != NULL && strcmp(name, SOLAR_OS_NATIVE_TIME_SERVICE) == 0 &&
        abi_version == SOLAR_OS_NATIVE_TIME_ABI && minimum_struct_size <= sizeof(native_time_api))
        return &native_time_api;
    if (name != NULL && strcmp(name, SOLAR_OS_NATIVE_THREAD_SERVICE) == 0 &&
        abi_version == SOLAR_OS_NATIVE_THREAD_ABI && minimum_struct_size <= sizeof(native_thread_api))
        return &native_thread_api;
    if (name != NULL && strcmp(name, SOLAR_OS_NATIVE_LIBC_SERVICE) == 0 &&
        abi_version == SOLAR_OS_NATIVE_LIBC_ABI && minimum_struct_size <= sizeof(native_libc_api))
        return &native_libc_api;
    if (name != NULL && strcmp(name, SOLAR_OS_NATIVE_LIBRARY_SERVICE) == 0 &&
        abi_version == SOLAR_OS_NATIVE_LIBRARY_ABI &&
        minimum_struct_size <= sizeof(native_library_api)) {
        return &native_library_api;
    }
    native_run_scope_t *scope = native_run_scope_current();
    if (scope != NULL && scope->options.get_service != NULL) {
        const void *custom = scope->options.get_service(name, abi_version,
            minimum_struct_size, scope->options.service_user);
        if (custom != NULL) return custom;
    }
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

static int native_register_library(const solar_os_native_library_descriptor_v1_t *descriptor);
static const solar_os_native_library_host_api_v1_t native_library_host_v1 = {
    .abi_version = SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI,
    .struct_size = sizeof(native_library_host_v1),
    .target = CONFIG_IDF_TARGET,
    .firmware_version = SOLAR_OS_VERSION,
    .register_library = native_register_library,
    .get_service = native_job_get_service,
};

const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void)
{
    return &native_library_host_v1;
}

static const struct esp_elfsym native_symbols[] = {
    ESP_ELFSYM_EXPORT(solar_os_native_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_job_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_driver_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_library_host_v1),
    /* Compilers emit these standard calls even for struct assignment and
     * simple loops. Keep the native substrate independent of broad SDK exports. */
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(setjmp),
    ESP_ELFSYM_EXPORT(longjmp),
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
    solar_os_memory_set_reclaimer(native_reclaim_unused_libraries);
    return ESP_OK;
}

static bool native_descriptor_text_valid(const char *text, size_t maximum)
{
    return text != NULL && text[0] != '\0' && strnlen(text, maximum) < maximum;
}

static int native_register_library(const solar_os_native_library_descriptor_v1_t *descriptor)
{
    native_resident_slot_t *slot = native_registering_slot;
    if (slot == NULL || slot->type != SOLAR_OS_MODULE_TYPE_LIBRARY ||
        native_registering_task != xTaskGetCurrentTaskHandle() || slot->registered ||
        descriptor == NULL ||
        descriptor->abi_version != SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI ||
        descriptor->struct_size < sizeof(*descriptor) ||
        !native_descriptor_text_valid(descriptor->id, SOLAR_OS_MODULE_PACKAGE_ID_MAX) ||
        strcmp(descriptor->id, slot->id) != 0 ||
        !native_descriptor_text_valid(descriptor->summary, SOLAR_OS_NATIVE_SUMMARY_MAX) ||
        descriptor->api == NULL ||
        descriptor->shutdown == NULL) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    /* Provider tables may have a distinct C struct type. Copy the common
     * prefix instead of aliasing that struct through the header type. */
    solar_os_native_library_api_header_v1_t header;
    memcpy(&header, descriptor->api, sizeof(header));
    if (!header.abi_version || header.struct_size < sizeof(header))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    slot->library_descriptor = *descriptor;
    slot->library_api_header = header;
    snprintf(slot->summary, sizeof(slot->summary), "%s", descriptor->summary);
    slot->registered = true;
    return SOLAR_OS_NATIVE_OK;
}

static int native_library_result(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}

/* Called with native_resident_lock held. The registry owns unique tokens,
 * rather than exposing slot addresses that can be reused after unload. */
static int native_library_link_lease(native_library_lease_t *lease,
                                     native_resident_slot_t *slot, uint32_t *token)
{
    if (native_library_lease_count >= SOLAR_OS_NATIVE_LIBRARY_LEASES_MAX ||
        native_library_next_lease == UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    lease->id = ++native_library_next_lease;
    lease->slot = slot;
    lease->next = native_library_leases;
    native_library_leases = lease;
    native_library_lease_count++;
    slot->callback_refs++;
    *token = lease->id;
    return SOLAR_OS_NATIVE_OK;
}

static int native_library_acquire_loaded(const char *id, uint32_t abi_version,
    uint32_t minimum_struct_size, native_library_lease_t *lease,
    uint32_t *token, const void **api)
{
    int result = SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    portENTER_CRITICAL(&native_resident_lock);
    for (native_resident_slot_t *slot = native_resident_slots; slot; slot = slot->next) {
        if (slot->type != SOLAR_OS_MODULE_TYPE_LIBRARY || strcmp(slot->id, id)) continue;
        if (!slot->active || slot->deactivating) result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        else if (slot->library_api_header.abi_version != abi_version ||
                 slot->library_api_header.struct_size < minimum_struct_size)
            result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        else {
            result = native_library_link_lease(lease, slot, token);
            if (result == SOLAR_OS_NATIVE_OK) *api = slot->library_descriptor.api;
        }
        break;
    }
    portEXIT_CRITICAL(&native_resident_lock);
    return result;
}

static bool native_library_id_valid(const char *id)
{
    if (!native_descriptor_text_valid(id, SOLAR_OS_MODULE_PACKAGE_ID_MAX) ||
        !strcmp(id, ".") || !strcmp(id, "..")) return false;
    for (const char *p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return false;
    }
    return true;
}

int solar_os_native_library_acquire(const char *id, uint32_t abi_version,
    uint32_t minimum_struct_size, uint32_t *token, const void **api)
{
    if (token) *token = 0U;
    if (api) *api = NULL;
    if (!native_library_id_valid(id) ||
        !abi_version || minimum_struct_size < sizeof(solar_os_native_library_api_header_v1_t) ||
        !token || !api) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_library_lease_t *lease = solar_os_memory_calloc(1, sizeof(*lease),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.library.lease");
    if (!lease) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = native_library_acquire_loaded(id, abi_version, minimum_struct_size,
                                              lease, token, api);
    if (result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        esp_err_t err = solar_os_module_package_path(SOLAR_OS_MODULE_TYPE_LIBRARY,
                                                    id, path, sizeof(path));
        if (err == ESP_OK) err = solar_os_native_module_activate(
            SOLAR_OS_MODULE_TYPE_LIBRARY, id, path, NULL, 0);
        result = err == ESP_OK ? native_library_acquire_loaded(id, abi_version,
            minimum_struct_size, lease, token, api) : native_library_result(err);
    }
    if (result != SOLAR_OS_NATIVE_OK) solar_os_memory_free(lease);
    return result;
}

int solar_os_native_library_retain(uint32_t token, uint32_t *retained)
{
    if (retained) *retained = 0U;
    if (!token || !retained) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_library_lease_t *lease = solar_os_memory_calloc(1, sizeof(*lease),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.library.lease");
    if (!lease) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    portENTER_CRITICAL(&native_resident_lock);
    for (native_library_lease_t *source = native_library_leases; source; source = source->next) {
        if (source->id != token) continue;
        result = source->slot->deactivating ? SOLAR_OS_NATIVE_ERROR_INVALID_STATE :
            native_library_link_lease(lease, source->slot, retained);
        break;
    }
    portEXIT_CRITICAL(&native_resident_lock);
    if (result != SOLAR_OS_NATIVE_OK) solar_os_memory_free(lease);
    return result;
}

int solar_os_native_library_release(uint32_t token)
{
    if (!token) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_library_lease_t *lease = NULL;
    portENTER_CRITICAL(&native_resident_lock);
    native_library_lease_t **link = &native_library_leases;
    while (*link && (*link)->id != token) link = &(*link)->next;
    if (*link) {
        lease = *link;
        *link = lease->next;
        lease->slot->callback_refs--;
        native_library_lease_count--;
    }
    portEXIT_CRITICAL(&native_resident_lock);
    const bool found = lease != NULL;
    solar_os_memory_free(lease);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

int solar_os_native_library_unload(const char *id)
{
    return native_library_result(solar_os_native_module_deactivate(
        SOLAR_OS_MODULE_TYPE_LIBRARY, id, NULL, 0));
}

static size_t native_reclaim_unused_libraries(void)
{
    char ids[SOLAR_OS_NATIVE_RESIDENT_MAX][SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    size_t count = 0, released = 0;
    portENTER_CRITICAL(&native_resident_lock);
    for (native_resident_slot_t *slot = native_resident_slots; slot; slot = slot->next) {
        if (slot->type == SOLAR_OS_MODULE_TYPE_LIBRARY && slot->active &&
            !slot->deactivating && slot->callback_refs == 0U &&
            count < SOLAR_OS_NATIVE_RESIDENT_MAX) {
            memcpy(ids[count++], slot->id, sizeof(ids[0]));
        }
    }
    portEXIT_CRITICAL(&native_resident_lock);
    for (size_t i = 0; i < count; i++) {
        /* Deactivation rechecks leases under lock and honors shutdown refusal.
         * An acquisition racing this snapshot therefore remains protected. */
        if (solar_os_native_library_unload(ids[i]) == SOLAR_OS_NATIVE_OK) released++;
    }
    return released;
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
    native_registering_task = xTaskGetCurrentTaskHandle();
    const int loader_ret = esp_elf_request(&slot->elf, 0, 1, argv);
    native_registering_slot = NULL;
    native_registering_task = NULL;
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
    native_registering_task = NULL;
    solar_os_memory_free(data);
    if (err != ESP_OK && slot->elf_initialized) {
        esp_elf_deinit(&slot->elf);
        slot->elf_initialized = false;
    }
    return err;
}

static esp_err_t native_register_resident(native_resident_slot_t *slot)
{
    if (slot->type == SOLAR_OS_MODULE_TYPE_LIBRARY) return ESP_OK;
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
    if (slot->type == SOLAR_OS_MODULE_TYPE_LIBRARY) {
        return slot->library_descriptor.shutdown() == SOLAR_OS_NATIVE_OK ?
            ESP_OK : ESP_ERR_INVALID_STATE;
    }
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
    bool operation_locked = true;
    bool scope_linked = false;
    native_run_scope_t scope = {
        .task = xTaskGetCurrentTaskHandle(), .options = *options,
    };
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
    if ((elf_info.foreground && options->expected_manifest == NULL) ||
        (options->expected_manifest != NULL &&
         (!elf_info.foreground || memcmp(&elf_info.app, options->expected_manifest,
                                         sizeof(elf_info.app)) != 0))) {
        native_set_detail(detail, detail_len, "foreground app requires its admitted session manifest");
        err = ESP_ERR_INVALID_STATE;
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

    /* Relocation and symbol registration are serialized. Module execution
     * can remain resident while another task loads a different application. */
    solar_os_memory_free(data);
    data = NULL;
    native_run_scope_add(&scope);
    scope_linked = true;
    atomic_store(&native_running, false);
    operation_locked = false;
    loader_ret = esp_elf_request(&elf, 0, argc, argv);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "ELF entry point failed to run");
        err = ESP_FAIL;
        goto done;
    }

    if (result != NULL) {
        *result = (solar_os_native_run_result_t){.elf = elf_info};
    }

done:
    if (options->cleanup != NULL) options->cleanup(options->service_user);
    if (scope_linked) native_run_scope_remove(&scope);
    if (elf_initialized) {
        esp_elf_deinit(&elf);
    }
    solar_os_memory_free(data);
    if (operation_locked) atomic_store(&native_running, false);
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
         type != SOLAR_OS_MODULE_TYPE_DRIVER &&
         type != SOLAR_OS_MODULE_TYPE_LIBRARY) ||
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
    if (type == SOLAR_OS_MODULE_TYPE_LIBRARY && slot->callback_refs != 0U) {
        portEXIT_CRITICAL(&native_resident_lock);
        native_set_detail(detail, detail_len, "library is in use; release all clients and workers before unloading");
        atomic_store(&native_running, false);
        return ESP_ERR_INVALID_STATE;
    }
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
                          type == SOLAR_OS_MODULE_TYPE_LIBRARY ?
                              "library shutdown refused; it remains loaded" :
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
