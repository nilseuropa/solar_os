#include "solar_os_robot.h"
#include <string.h>
#include <stdio.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_native.h"
#include "solar_os_memory.h"
#include "solar_os_stream.h"
#include "solar_os_parameters.h"
#include "esp_timer.h"
#include <math.h>

typedef struct {
    uint32_t lease, joint;
    const solar_os_robot_api_v3_t *api;
    char name[SOLAR_OS_ROBOT_JOINT_NAME_MAX], owner[16];
    bool model_owned;
} entry_t;
static EXT_RAM_BSS_ATTR entry_t entries[SOLAR_OS_ROBOT_JOINT_MAX];
static struct { uint32_t lease, id; const solar_os_robot_api_v3_t *api; bool started; } model;
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
    case SOLAR_OS_NATIVE_ERROR_INVALID_SIZE: return ESP_ERR_INVALID_SIZE;
    default: return ESP_FAIL;
    }
}

#include "solar_os_robot_diff_drive.inc"

static bool model_controllers(void)
{
    if(drive_front.id) return true;
    for(unsigned i=0;i<SOLAR_OS_ROBOT_JOINT_MAX;++i) if(entries[i].model_owned) return true;
    return false;
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
        SOLAR_OS_ROBOT_JOINT_API_SIZE, &lease, &table));
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
    esp_err_t err = e ? e->model_owned ? ESP_ERR_INVALID_STATE : result(e->api->configure(e->joint, key, value)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_bind_motor(const char *name, const char *motor, const char *encoder)
{
    lock(); entry_t *e = find(name);
    esp_err_t err = e ? e->model_owned ? ESP_ERR_INVALID_STATE : result(e->api->bind_motor(e->joint, motor, encoder)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_bind_servo(const char *name, const solar_os_native_servo_config_v1_t *config)
{
    lock(); entry_t *e = find(name);
    esp_err_t err = e ? e->model_owned ? ESP_ERR_INVALID_STATE : result(e->api->bind_servo(e->joint, config)) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_command(const char *name, float value)
{
    lock(); entry_t *e = find(name);
    esp_err_t err = e ? e->model_owned && !model.started ? ESP_ERR_INVALID_STATE : result(e->api->command(e->joint, value)) : ESP_ERR_NOT_FOUND;
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
    lock(); entry_t *e = find(name); esp_err_t err = e ? e->model_owned ? ESP_ERR_INVALID_STATE : close_entry(e) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_robot_joint_enable(const char *name)
{
    lock(); entry_t *e = find(name); esp_err_t err = e ? e->model_owned ? ESP_ERR_INVALID_STATE : result(e->api->enable(e->joint)) : ESP_ERR_NOT_FOUND;
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
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (entries[i].lease && !entries[i].model_owned && !strcmp(entries[i].owner, owner)) {
        esp_err_t one = close_entry(&entries[i]); if (err == ESP_OK) err = one;
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_robot_model_load_xml(const char *xml, size_t bytes, char *error, size_t error_bytes)
{
    if (error && error_bytes) *error = 0;
    if (!xml || !bytes || bytes > SOLAR_OS_ROBOT_MODEL_XML_MAX) return ESP_ERR_INVALID_ARG;
    lock();
    if(model.started || model_controllers()) {
        if(error && error_bytes) strlcpy(error,"Run robot stop before replacing the model",error_bytes);
        xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (!model.lease) {
        const void *table = NULL;
        err = result(solar_os_native_library_acquire(SOLAR_OS_ROBOT_LIBRARY, SOLAR_OS_ROBOT_ABI,
            sizeof(solar_os_robot_api_v3_t), &model.lease, &table));
        if (err == ESP_OK) model.api = table;
    }
    uint32_t next = 0;
    if (err == ESP_OK) err = result(model.api->model_load(xml, bytes, &next, error, error_bytes));
    if (err == ESP_OK && model.id) {
        err = result(model.api->model_close(model.id));
        if (err != ESP_OK) (void)model.api->model_close(next);
    }
    if (err == ESP_OK) model.id = next;
    if (err != ESP_OK && model.lease && !model.id) {
        /* Retain a failed release for explicit unload/retry. */
        if (solar_os_native_library_release(model.lease) == SOLAR_OS_NATIVE_OK) memset(&model, 0, sizeof(model));
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_robot_model_load_file(const char *path, char *error, size_t error_bytes)
{
    if (error && error_bytes) *error = 0;
    if (!path || !*path) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    esp_err_t err = ESP_FAIL;
    char *xml = NULL;
    if (fseek(file, 0, SEEK_END)) goto done;
    long length = ftell(file);
    if (length <= 0 || (unsigned long)length > SOLAR_OS_ROBOT_MODEL_XML_MAX) { err = ESP_ERR_INVALID_SIZE; goto done; }
    if (fseek(file, 0, SEEK_SET)) goto done;
    xml = solar_os_memory_alloc((size_t)length, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "robot.urdf");
    if (!xml) { err = ESP_ERR_NO_MEM; goto done; }
    if (fread(xml, 1, (size_t)length, file) != (size_t)length || ferror(file)) goto done;
    /* Reject growth during the read rather than parsing a truncated prefix. */
    if (fgetc(file) != EOF || ferror(file)) goto done;
    err = solar_os_robot_model_load_xml(xml, (size_t)length, error, error_bytes);
done:
    solar_os_memory_free(xml);
    fclose(file);
    return err;
}

esp_err_t solar_os_robot_model_unload(void)
{
    lock();
    if(model.started || model_controllers()) { xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = model.id ? result(model.api->model_close(model.id)) : ESP_OK;
    if (err == ESP_OK) {
        model.id = 0;
        if (model.lease) err = result(solar_os_native_library_release(model.lease));
        if (err == ESP_OK) memset(&model, 0, sizeof(model));
    }
    xSemaphoreGive(mutex); return err;
}

/* Serialized by the frontend mutex. Never discard a failed close or lease
 * release: retaining the entry pins the model until robot stop can retry. */
static esp_err_t stop_model_controllers(void)
{
    model.started=false;
    drive_front_accept(false);
    if(drive_front.id) (void)model.api->diff_drive_disable(drive_front.id);
    esp_err_t err=ESP_OK;
    for(unsigned i=SOLAR_OS_ROBOT_JOINT_MAX;i>0;--i) if(entries[i-1].model_owned) {
        esp_err_t one=close_entry(&entries[i-1]);
        if(err==ESP_OK) err=one;
    }
    if(drive_front.id) {
        esp_err_t one=drive_front_unpublish();if(err==ESP_OK)err=one;
        if(err==ESP_OK) {
            err=result(model.api->diff_drive_close(drive_front.id));
            if(err==ESP_OK)memset(&drive_front,0,sizeof(drive_front));
        }
    }
    return err;
}

esp_err_t solar_os_robot_start(void)
{
    lock();
    if(!model.id) { xSemaphoreGive(mutex); return ESP_ERR_NOT_FOUND; }
    if(model.started || model_controllers()) { xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE; }
    solar_os_robot_joint_config_v3_t config;
    unsigned count=0, free_entries=0;
    for(unsigned i=0;i<SOLAR_OS_ROBOT_JOINT_MAX;++i) free_entries+=!entries[i].lease;
    esp_err_t err=ESP_OK;
    /* Check all names and capacity before allocating any controller. */
    for(;count<SOLAR_OS_ROBOT_JOINT_MAX;++count) {
        err=result(model.api->model_binding(model.id,count,&config));
        if(err==ESP_ERR_NOT_FOUND) { err=ESP_OK; break; }
        if(err!=ESP_OK) break;
        if(find(config.name)) { err=ESP_ERR_INVALID_STATE; break; }
    }
    if(err==ESP_OK && !count) err=ESP_ERR_NOT_FOUND;
    if(err==ESP_OK && count>free_entries) err=ESP_ERR_NO_MEM;
    for(unsigned i=0;err==ESP_OK && i<count;++i) {
        entry_t *e=NULL;
        for(unsigned j=0;j<SOLAR_OS_ROBOT_JOINT_MAX;++j) if(!entries[j].lease) { e=&entries[j]; break; }
        const void *table=NULL;
        uint32_t lease=0;
        err=result(solar_os_native_library_acquire(SOLAR_OS_ROBOT_LIBRARY,SOLAR_OS_ROBOT_ABI,sizeof(*model.api),&lease,&table));
        if(err!=ESP_OK) break;
        *e=(entry_t){.lease=lease,.api=table,.model_owned=true};
        err=result(model.api->model_binding(model.id,i,&config));
        if(err==ESP_OK) {
            strlcpy(e->name,config.name,sizeof(e->name)); strlcpy(e->owner,"robot",sizeof(e->owner));
            err=result(e->api->model_create_joint(model.id,i,&e->joint));
        }
    }
    /* Everything is configured before the first enable. Existing enable keeps
     * velocity outputs stopped until a fresh explicit command arrives. */
    for(unsigned i=0;err==ESP_OK && i<SOLAR_OS_ROBOT_JOINT_MAX;++i)
        if(entries[i].model_owned) err=result(entries[i].api->enable(entries[i].joint));
    if(err==ESP_OK)err=drive_front_start();
    if(err==ESP_OK) { model.started=true;drive_front_accept(true); }
    else {
        esp_err_t cleanup_error=stop_model_controllers();
        if(cleanup_error!=ESP_OK) err=cleanup_error;
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_robot_stop(void)
{
    lock();
    esp_err_t err=stop_model_controllers();
    xSemaphoreGive(mutex); return err;
}

#define MODEL_FRONTEND(expression) do { \
    lock(); esp_err_t err = model.id ? result(expression) : ESP_ERR_NOT_FOUND; \
    xSemaphoreGive(mutex); return err; \
} while (0)
esp_err_t solar_os_robot_model_get_info(solar_os_robot_model_info_t *out)
{ MODEL_FRONTEND(model.api->model_info(model.id, out)); }
esp_err_t solar_os_robot_model_get_link(uint32_t index, char *out, size_t bytes)
{ MODEL_FRONTEND(model.api->model_link(model.id, index, out, bytes)); }
esp_err_t solar_os_robot_model_get_joint(uint32_t index, solar_os_robot_model_joint_info_t *out)
{ MODEL_FRONTEND(model.api->model_joint(model.id, index, out)); }
esp_err_t solar_os_robot_model_update(const char *const *names, const float *positions, uint32_t count)
{ MODEL_FRONTEND(model.api->model_set_positions(model.id, names, positions, count)); }
esp_err_t solar_os_robot_model_transform(const char *target, const char *source, solar_os_robot_transform_t *out)
{ MODEL_FRONTEND(model.api->model_lookup(model.id, target, source, out)); }
esp_err_t solar_os_robot_model_get_extension(char *out, size_t bytes)
{ MODEL_FRONTEND(model.api->model_extension(model.id, out, bytes)); }
#undef MODEL_FRONTEND

static bool model_parent_locked(const char *child, uint32_t count, solar_os_robot_model_joint_info_t *out)
{
    for (uint32_t i = 0; i < count; ++i)
        if (model.api->model_joint(model.id, i, out) == SOLAR_OS_NATIVE_OK && !strcmp(out->child, child)) return true;
    return false;
}

void solar_os_robot_model_transform_error(const char *target, const char *source, char *error, size_t bytes)
{
    if (!error || !bytes) return;
    strlcpy(error, "Joint position unavailable; inspect robot model joints", bytes);
    if (!target || !source) return;
    lock();
    solar_os_robot_model_info_t info;
    if (!model.id || model.api->model_info(model.id, &info) != SOLAR_OS_NATIVE_OK) { xSemaphoreGive(mutex); return; }
    char links[2][SOLAR_OS_ROBOT_MODEL_NAME_MAX];
    unsigned depths[2] = {0};
    solar_os_robot_model_joint_info_t joint;
    const char *frames[2] = {target, source};
    for (unsigned side = 0; side < 2; ++side) {
        strlcpy(links[side], frames[side], sizeof(links[side]));
        while (depths[side] < info.joints && model_parent_locked(links[side], info.joints, &joint)) {
            ++depths[side]; strlcpy(links[side], joint.parent, sizeof(links[side]));
        }
        strlcpy(links[side], frames[side], sizeof(links[side]));
    }
    /* Walk only to the common ancestor: unknown positions above it do not
     * affect this query. No tree-sized allocation on the shell task stack. */
    for (uint32_t step = 0; step < 2 * info.joints && strcmp(links[0], links[1]); ++step) {
        unsigned side = depths[0] > depths[1] ? 0 : 1;
        if (!model_parent_locked(links[side], info.joints, &joint)) break;
        if (!joint.position_known) {
            snprintf(error, bytes, "Joint '%s' has no current position; inspect robot model joints and robot joint status", joint.name);
            break;
        }
        --depths[side]; strlcpy(links[side], joint.parent, sizeof(links[side]));
    }
    xSemaphoreGive(mutex);
}

esp_err_t solar_os_robot_model_apply(const char *target, const char *source, const float in[3], bool direction, float out[3])
{
    lock();
    solar_os_robot_transform_t transform;
    esp_err_t err = model.id ? result(model.api->model_lookup(model.id, target, source, &transform)) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) err = result(model.api->transform_apply(&transform, in, direction, out));
    xSemaphoreGive(mutex); return err;
}
