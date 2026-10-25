#include "solar_os_robot.h"
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_native.h"

typedef struct {
    uint32_t lease, joint;
    const solar_os_robot_api_v3_t *api;
    char name[SOLAR_OS_ROBOT_JOINT_NAME_MAX], owner[16];
} entry_t;
static EXT_RAM_BSS_ATTR entry_t entries[SOLAR_OS_ROBOT_JOINT_MAX];
static StaticSemaphore_t storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static void lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (!mutex) mutex = xSemaphoreCreateMutexStatic(&storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(mutex, portMAX_DELAY);
}
static entry_t *find(const char *name)
{ for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (entries[i].lease && name && !strcmp(entries[i].name, name)) return &entries[i]; return NULL; }
static esp_err_t result(int e)
{
    switch (e) {
    case SOLAR_OS_NATIVE_OK: return ESP_OK;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: return ESP_ERR_INVALID_ARG;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: return ESP_ERR_INVALID_STATE;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return ESP_ERR_NO_MEM;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: return ESP_ERR_TIMEOUT;
    default: return ESP_FAIL;
    }
}

esp_err_t solar_os_robot_joint_create(const char *name, uint32_t kind, const char *owner)
{
    if (!name || !*name || strlen(name) >= SOLAR_OS_ROBOT_JOINT_NAME_MAX || !owner || !*owner || strlen(owner) >= 16) return ESP_ERR_INVALID_ARG;
    lock();
    if (find(name)) { xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE; }
    entry_t *entry = NULL;
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (!entries[i].lease) { entry = &entries[i]; break; }
    if (!entry) { xSemaphoreGive(mutex); return ESP_ERR_NO_MEM; }
    const void *table = NULL;
    uint32_t lease = 0, joint = 0;
    esp_err_t err = result(solar_os_native_library_acquire(SOLAR_OS_ROBOT_LIBRARY, SOLAR_OS_ROBOT_ABI,
        sizeof(solar_os_robot_api_v3_t), &lease, &table));
    const solar_os_robot_api_v3_t *api = table;
    if (err == ESP_OK) err = result(api->create(name, kind, &joint));
    if (err == ESP_OK) {
        *entry = (entry_t){.lease = lease, .joint = joint, .api = api};
        strlcpy(entry->name, name, sizeof(entry->name)); strlcpy(entry->owner, owner, sizeof(entry->owner));
    } else if (lease) (void)solar_os_native_library_release(lease);
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_robot_joint_configure(const char *name, const char *key, float value)
{
    lock(); entry_t *e = find(name);
    esp_err_t err = e ? result(e->api->configure(e->joint, key, value)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_bind_motor(const char *name, const char *motor, const char *encoder)
{
    lock(); entry_t *e = find(name);
    esp_err_t err = e ? result(e->api->bind_motor(e->joint, motor, encoder)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_bind_servo(const char *name, const solar_os_native_servo_config_v1_t *config)
{
    lock(); entry_t *e = find(name);
    esp_err_t err = e ? result(e->api->bind_servo(e->joint, config)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_command(const char *name, float value)
{
    lock(); entry_t *e = find(name);
    esp_err_t err = e ? result(e->api->command(e->joint, value)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
static esp_err_t close_entry(entry_t *e)
{
    esp_err_t err = e->joint ? result(e->api->close(e->joint)) : ESP_OK;
    if (err == ESP_OK) {
        e->joint = 0;
        err = result(solar_os_native_library_release(e->lease));
        if (err == ESP_OK) memset(e, 0, sizeof(*e));
    }
    return err;
}
esp_err_t solar_os_robot_joint_close(const char *name)
{
    lock(); entry_t *e = find(name); esp_err_t err = e ? close_entry(e) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_enable(const char *name)
{
    lock(); entry_t *e = find(name); esp_err_t err = e ? result(e->api->enable(e->joint)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_disable(const char *name)
{
    lock(); entry_t *e = find(name); esp_err_t err = e ? result(e->api->disable(e->joint)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_status(const char *name, solar_os_robot_joint_info_v3_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    lock(); entry_t *e = find(name); esp_err_t err = e ? result(e->api->status(e->joint, info)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
size_t solar_os_robot_joint_count(void)
{
    lock(); size_t count = 0; for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) count += entries[i].lease != 0;
    xSemaphoreGive(mutex); return count;
}
bool solar_os_robot_joint_get_info(size_t index, solar_os_robot_joint_info_v3_t *info)
{
    if (!info) return false;
    lock(); bool found = false;
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (entries[i].lease && index-- == 0) {
        found = entries[i].api->status(entries[i].joint, info) == SOLAR_OS_NATIVE_OK; break;
    }
    xSemaphoreGive(mutex); return found;
}
esp_err_t solar_os_robot_release_owner(const char *owner)
{
    if (!owner) return ESP_ERR_INVALID_ARG;
    lock(); esp_err_t err = ESP_OK;
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (entries[i].lease && !strcmp(entries[i].owner, owner)) {
        esp_err_t one = close_entry(&entries[i]); if (err == ESP_OK) err = one;
    }
    xSemaphoreGive(mutex); return err;
}
