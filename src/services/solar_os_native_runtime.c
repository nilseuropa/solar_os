#include "solar_os_native_runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_config.h"
#include "solar_os_jobs.h"
#include "solar_os_memory.h"
#include "solar_os_native_support.h"
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
#include "solar_os_native_image.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_expansion.h"
#endif

static atomic_bool native_running;
/* Only the task holding the loader operation may reclaim idle images inside
 * it. Public module operations remain serialized and non-reentrant. */
static _Thread_local bool native_operation_locked;
static bool native_reclaimer_registered;
static size_t native_reclaim_unused_libraries(void);
static size_t native_pressure_reclaim(const solar_os_memory_pressure_request_t *request, void *user)
{
    (void)request; (void)user;
    return native_reclaim_unused_libraries();
}
static solar_os_memory_reclaimer_t native_reclaimer = {
    .owner = "native.libraries", .reclaim = native_pressure_reclaim, .priority = 200,
};
static esp_err_t native_module_deactivate_locked(solar_os_module_type_t type,
    const char *id, char *detail, size_t detail_len);
#define SOLAR_OS_NATIVE_RESIDENT_MAX 16U
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
    void *image;
    solar_os_native_job_descriptor_v1_t job_descriptor;
    solar_os_native_driver_descriptor_v1_t driver_descriptor;
    solar_os_native_library_descriptor_v1_t library_descriptor;
    solar_os_native_library_api_header_v1_t library_api_header;
    solar_os_native_library_select_api_fn library_select_api;
    solar_os_job_t job;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    solar_os_expansion_driver_t driver;
    solar_os_native_bound_driver_descriptor_v1_t bound_driver;
#endif
} native_resident_slot_t;

static native_resident_slot_t *native_resident_slots;
/* Registration belongs to the entry task. Other tasks cannot register against
 * an in-progress component or race a shared initialization pointer. */
static _Thread_local native_resident_slot_t *native_registering_slot;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
typedef struct {
    native_resident_slot_t *slot;
    const char *name;
    const solar_os_expansion_binding_t *bindings;
    size_t count;
} native_driver_attach_scope_t;
static _Thread_local const native_driver_attach_scope_t *native_driver_scope;
#endif
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

static void native_set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", text != NULL ? text : "native module failed");
    }
}

esp_err_t solar_os_native_operation_begin(void)
{
    if (atomic_exchange(&native_running, true)) return ESP_ERR_INVALID_STATE;
    native_operation_locked = true;
    if (!native_reclaimer_registered) {
        if (!solar_os_memory_register_reclaimer(&native_reclaimer)) {
            solar_os_native_operation_end();
            return ESP_ERR_NO_MEM;
        }
        native_reclaimer_registered = true;
    }
    return ESP_OK;
}

void solar_os_native_operation_end(void)
{
    native_operation_locked = false;
    atomic_store(&native_running, false);
}

static bool native_descriptor_text_valid(const char *text, size_t maximum)
{
    return text != NULL && text[0] != '\0' && strnlen(text, maximum) < maximum;
}

int solar_os_native_register_library(const solar_os_native_library_descriptor_v1_t *descriptor)
{
    native_resident_slot_t *slot = native_registering_slot;
    if (slot == NULL || slot->type != SOLAR_OS_MODULE_TYPE_LIBRARY ||
        slot->registered ||
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
    if (descriptor->struct_size >= sizeof(solar_os_native_library_versioned_descriptor_v1_t)) {
        const solar_os_native_library_versioned_descriptor_v1_t *versioned = (const void *)descriptor;
        slot->library_select_api = versioned->select_api;
    }
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
    uint32_t *token, const void **api, solar_os_native_library_select_api_fn *select_api)
{
    int result = SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    portENTER_CRITICAL(&native_resident_lock);
    for (native_resident_slot_t *slot = native_resident_slots; slot; slot = slot->next) {
        if (slot->type != SOLAR_OS_MODULE_TYPE_LIBRARY || strcmp(slot->id, id)) continue;
        if (!slot->active || slot->deactivating) result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        else {
            const bool default_matches = slot->library_api_header.abi_version == abi_version &&
                slot->library_api_header.struct_size >= minimum_struct_size;
            result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
            if (default_matches || slot->library_select_api) {
                result = native_library_link_lease(lease, slot, token);
                if (result == SOLAR_OS_NATIVE_OK) {
                    if (default_matches) *api = slot->library_descriptor.api;
                    else *select_api = slot->library_select_api;
                }
            }
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
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.library.lease");
    if (!lease) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_native_library_select_api_fn select_api = NULL;
    int result = native_library_acquire_loaded(id, abi_version, minimum_struct_size,
                                              lease, token, api, &select_api);
    if (result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) {
        esp_err_t err;
        if (solar_os_component_find(SOLAR_OS_MODULE_TYPE_INVALID, id)) {
            err = solar_os_native_module_activate(SOLAR_OS_MODULE_TYPE_LIBRARY, id, NULL, NULL, 0);
        } else {
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
            char path[SOLAR_OS_STORAGE_PATH_MAX];
            err = solar_os_module_package_path(SOLAR_OS_MODULE_TYPE_LIBRARY, id, path, sizeof(path));
            if (err == ESP_OK) err = solar_os_native_module_activate(
                SOLAR_OS_MODULE_TYPE_LIBRARY, id, path, NULL, 0);
#else
            err = ESP_ERR_NOT_SUPPORTED;
#endif
        }
        result = err == ESP_OK ? native_library_acquire_loaded(id, abi_version,
            minimum_struct_size, lease, token, api, &select_api) : native_library_result(err);
    }
    if (result != SOLAR_OS_NATIVE_OK) {
        solar_os_memory_free(lease);
    } else if (select_api) {
        const void *selected = NULL;
        result = select_api(abi_version, minimum_struct_size, &selected);
        if (result == SOLAR_OS_NATIVE_OK) {
            solar_os_native_library_api_header_v1_t header = {0};
            if (selected) memcpy(&header, selected, sizeof(header));
            if (header.abi_version != abi_version || header.struct_size < minimum_struct_size)
                result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        }
        if (result == SOLAR_OS_NATIVE_OK) *api = selected;
        else {
            (void)solar_os_native_library_release(*token);
            *token = 0;
        }
    }
    return result;
}

int solar_os_native_library_retain(uint32_t token, uint32_t *retained)
{
    if (retained) *retained = 0U;
    if (!token || !retained) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_library_lease_t *lease = solar_os_memory_calloc(1, sizeof(*lease),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.library.lease");
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
        const esp_err_t err = native_operation_locked ?
            native_module_deactivate_locked(SOLAR_OS_MODULE_TYPE_LIBRARY,
                ids[i], NULL, 0) :
            solar_os_native_module_deactivate(SOLAR_OS_MODULE_TYPE_LIBRARY,
                ids[i], NULL, 0);
        if (err == ESP_OK) released++;
    }
    return released;
}

int solar_os_native_register_job(
    const solar_os_native_job_descriptor_v1_t *descriptor)
{
    native_resident_slot_t *slot = native_registering_slot;
    if (slot == NULL || slot->type != SOLAR_OS_MODULE_TYPE_JOB ||
        slot->registered ||
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

int solar_os_native_register_driver(
    const solar_os_native_driver_descriptor_v1_t *descriptor)
{
    native_resident_slot_t *slot = native_registering_slot;
    if (slot == NULL || slot->type != SOLAR_OS_MODULE_TYPE_DRIVER ||
        slot->registered ||
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

int solar_os_native_register_bound_driver(
    const solar_os_native_bound_driver_descriptor_v1_t *descriptor)
{
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    native_resident_slot_t *slot = native_registering_slot;
    if (slot == NULL || slot->type != SOLAR_OS_MODULE_TYPE_DRIVER || slot->registered ||
        descriptor == NULL || descriptor->abi_version != SOLAR_OS_NATIVE_DRIVER_REGISTRATION_ABI ||
        descriptor->struct_size < sizeof(*descriptor) ||
        !native_descriptor_text_valid(descriptor->id, SOLAR_OS_MODULE_PACKAGE_ID_MAX) ||
        strcmp(descriptor->id, slot->id) != 0 ||
        !native_descriptor_text_valid(descriptor->summary, SOLAR_OS_NATIVE_SUMMARY_MAX) ||
        (unsigned)descriptor->category >= SOLAR_OS_EXPANSION_CATEGORY_COUNT ||
        descriptor->binding_spec_count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX ||
        (descriptor->binding_spec_count && !descriptor->binding_specs) ||
        !descriptor->attach || !descriptor->detach) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    slot->bound_driver = *descriptor;
    snprintf(slot->summary, sizeof(slot->summary), "%s", descriptor->summary);
    slot->registered = true;
    return SOLAR_OS_NATIVE_OK;
#else
    (void)descriptor;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
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

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
void *solar_os_native_driver_pin(void)
{
    return native_driver_scope && native_slot_acquire(native_driver_scope->slot) ? native_driver_scope->slot : NULL;
}

void solar_os_native_driver_unpin(void *pin)
{
    native_slot_release(pin);
}

bool solar_os_native_driver_has_binding(const char *owner, solar_os_expansion_binding_kind_t kind, const char *target)
{
    const native_driver_attach_scope_t *scope = native_driver_scope;
    if (!scope || strcmp(scope->name, owner)) return false;
    for (size_t i = 0; i < scope->count; ++i) {
        if (scope->bindings[i].kind == kind && !strcmp(scope->bindings[i].target, target)) return true;
    }
    return false;
}
#endif

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
    size_t binding_count,
    void **instance)
{
    native_resident_slot_t *slot = user;
    if (!native_slot_acquire(slot)) return ESP_ERR_INVALID_STATE;
    esp_err_t result;
    if (slot->bound_driver.attach) {
        const native_driver_attach_scope_t scope = {slot, name, bindings, binding_count};
        const native_driver_attach_scope_t *previous = native_driver_scope;
        native_driver_scope = &scope;
        result = solar_os_native_callback_result(slot->bound_driver.attach(name, bindings, binding_count, instance));
        native_driver_scope = previous;
    } else {
        result = binding_count == 0 ? (esp_err_t)slot->driver_descriptor.attach(name) : ESP_ERR_INVALID_ARG;
    }
    native_slot_release(slot);
    return result;
}

static esp_err_t native_driver_detach_callback(void *user, const char *name, void *instance)
{
    native_resident_slot_t *slot = (native_resident_slot_t *)user;
    if (!native_slot_acquire(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = slot->bound_driver.detach ?
        solar_os_native_callback_result(slot->bound_driver.detach(instance)) :
        (esp_err_t)slot->driver_descriptor.detach(name);
    native_slot_release(slot);
    return result;
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

static esp_err_t native_initialize_builtin_component(native_resident_slot_t *slot,
    int (*entry)(int, char **), char *detail, size_t detail_len)
{
    char *argv[] = {slot->id, NULL};
    native_registering_slot = slot;
    const int code = entry(1, argv);
    native_registering_slot = NULL;
    if (code || !slot->registered) {
        native_set_detail(detail, detail_len, "compiled component initialization failed");
        return code == SOLAR_OS_NATIVE_ERROR_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t native_load_resident(native_resident_slot_t *slot,
    const char *path, char *detail, size_t detail_len)
{
    const solar_os_component_t *component = solar_os_component_find(SOLAR_OS_MODULE_TYPE_INVALID, slot->id);
    if (component) {
        if (component->type != slot->type || component->lifecycle_abi != 1U) {
            native_set_detail(detail, detail_len, "compiled component has a different type or lifecycle");
            return ESP_ERR_NOT_SUPPORTED;
        }
        return native_initialize_builtin_component(slot, component->entry, detail, detail_len);
    }
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    esp_err_t err = solar_os_native_image_load(path, &slot->image, detail, detail_len);
    if (err != ESP_OK) return err;
    char *argv[] = {slot->id, NULL};
    native_registering_slot = slot;
    const int code = solar_os_native_image_request(slot->image, 1, argv);
    native_registering_slot = NULL;
    if (code != 0 || !slot->registered) {
        native_set_detail(detail, detail_len, "ELF did not register the expected module lifecycle");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
#else
    (void)path;
    native_set_detail(detail, detail_len, "external module loading is not supported by this firmware");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void native_close_resident(native_resident_slot_t *slot)
{
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    if (slot->image) solar_os_native_image_close(slot->image);
#else
    (void)slot;
#endif
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
            .category = slot->bound_driver.attach ? slot->bound_driver.category : SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
            .binding_specs = slot->bound_driver.binding_specs,
            .binding_spec_count = slot->bound_driver.binding_spec_count,
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
        ((!path || !path[0]) && !solar_os_component_find(SOLAR_OS_MODULE_TYPE_INVALID, id))) {
        native_set_detail(detail, detail_len, "invalid resident module request");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t admission = solar_os_native_operation_begin();
    if (admission != ESP_OK) {
        native_set_detail(detail, detail_len, admission == ESP_ERR_NO_MEM ?
            "could not register module memory reclamation" : "another native module operation is running");
        return admission;
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
                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
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
        native_close_resident(slot);
        solar_os_memory_free(slot);
    }
    solar_os_native_operation_end();
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
    const esp_err_t admission = solar_os_native_operation_begin();
    if (admission != ESP_OK) {
        native_set_detail(detail, detail_len, admission == ESP_ERR_NO_MEM ?
            "could not register module memory reclamation" : "another native module operation is running");
        return admission;
    }
    const esp_err_t err = native_module_deactivate_locked(type, id, detail, detail_len);
    solar_os_native_operation_end();
    return err;
}

static esp_err_t native_module_deactivate_locked(solar_os_module_type_t type,
    const char *id, char *detail, size_t detail_len)
{
    native_resident_slot_t *slot = native_find_slot(type, id);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&native_resident_lock);
    if ((type == SOLAR_OS_MODULE_TYPE_LIBRARY || type == SOLAR_OS_MODULE_TYPE_DRIVER) && slot->callback_refs != 0U) {
        portEXIT_CRITICAL(&native_resident_lock);
        native_set_detail(detail, detail_len, type == SOLAR_OS_MODULE_TYPE_DRIVER ?
            "detach all devices and close their providers before unloading" :
            "library is in use; release all clients and workers before unloading");
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
    native_close_resident(slot);
    solar_os_memory_free(slot);
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
