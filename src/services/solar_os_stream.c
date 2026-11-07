#include "solar_os_stream.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "solar_os_time.h"

#define STREAM_BYTE_READ_MAX 64U

typedef struct {
    bool registered;
    uint32_t generation;
    solar_os_stream_driver_t driver;
    uint32_t active_handles;
    uint32_t observers;
    char owner[SOLAR_OS_STREAM_OWNER_MAX];
    uint64_t read_units;
    uint64_t written_units;
    uint32_t overruns;
    uint32_t underruns;
} stream_entry_t;

static EXT_RAM_BSS_ATTR stream_entry_t streams[SOLAR_OS_STREAM_MAX];
static SemaphoreHandle_t streams_mutex;
static StaticSemaphore_t streams_mutex_storage;
static portMUX_TYPE streams_init_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t stream_ensure_init(void)
{
    portENTER_CRITICAL(&streams_init_lock);
    if (streams_mutex == NULL) {
        streams_mutex = xSemaphoreCreateMutexStatic(&streams_mutex_storage);
    }
    SemaphoreHandle_t mutex = streams_mutex;
    portEXIT_CRITICAL(&streams_init_lock);
    return mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void stream_lock(void)
{
    (void)xSemaphoreTake(streams_mutex, portMAX_DELAY);
}

static void stream_unlock(void)
{
    (void)xSemaphoreGive(streams_mutex);
}

static bool stream_text_valid(const char *text, size_t capacity, bool required)
{
    if (text == NULL) {
        return !required;
    }
    const size_t len = strnlen(text, capacity);
    return len < capacity && (!required || len > 0U);
}

static int stream_find_locked(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < SOLAR_OS_STREAM_MAX; i++) {
        if (streams[i].registered &&
            strcmp(streams[i].driver.info.id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool stream_direction_can_read(solar_os_stream_direction_t direction)
{
    return direction == SOLAR_OS_STREAM_DIRECTION_SOURCE ||
           direction == SOLAR_OS_STREAM_DIRECTION_DUPLEX;
}

static bool stream_direction_can_write(solar_os_stream_direction_t direction)
{
    return direction == SOLAR_OS_STREAM_DIRECTION_SINK ||
           direction == SOLAR_OS_STREAM_DIRECTION_DUPLEX;
}

static bool stream_direction_compatible(solar_os_stream_direction_t endpoint,
                                        solar_os_stream_direction_t requested)
{
    if (requested == SOLAR_OS_STREAM_DIRECTION_DUPLEX) {
        return endpoint == SOLAR_OS_STREAM_DIRECTION_DUPLEX;
    }
    if (requested == SOLAR_OS_STREAM_DIRECTION_SOURCE) {
        return stream_direction_can_read(endpoint);
    }
    if (requested == SOLAR_OS_STREAM_DIRECTION_SINK) {
        return stream_direction_can_write(endpoint);
    }
    return false;
}

static bool stream_driver_valid(const solar_os_stream_driver_t *driver)
{
    if (driver == NULL ||
        !stream_text_valid(driver->info.id, sizeof(driver->info.id), true) ||
        !stream_text_valid(driver->info.provider,
                           sizeof(driver->info.provider), true) ||
        !stream_text_valid(driver->info.device,
                           sizeof(driver->info.device), false) ||
        !stream_text_valid(driver->info.unit, sizeof(driver->info.unit), false) ||
        !stream_text_valid(driver->info.format,
                           sizeof(driver->info.format), true) ||
        !stream_text_valid(driver->info.summary,
                           sizeof(driver->info.summary), false) ||
        driver->info.type > SOLAR_OS_STREAM_TYPE_VIDEO ||
        driver->info.direction > SOLAR_OS_STREAM_DIRECTION_DUPLEX ||
        driver->info.sharing > SOLAR_OS_STREAM_SHARING_MIXED) {
        return false;
    }
    if (stream_direction_can_read(driver->info.direction) &&
        driver->read == NULL && driver->read_scalar == NULL && driver->read_scalar_sample == NULL &&
        driver->read_csv == NULL && driver->acquire_frame == NULL) {
        return false;
    }
    if (stream_direction_can_write(driver->info.direction) &&
        driver->write == NULL) {
        return false;
    }
    if (driver->info.has_scalar_range &&
        (driver->info.type != SOLAR_OS_STREAM_TYPE_SCALAR ||
         !stream_direction_can_write(driver->info.direction) ||
         strcmp(driver->info.format, "f32") != 0 ||
         !isfinite(driver->info.scalar_minimum) ||
         !isfinite(driver->info.scalar_maximum) ||
         driver->info.scalar_minimum > driver->info.scalar_maximum)) {
        return false;
    }
    if (driver->info.type == SOLAR_OS_STREAM_TYPE_SCALAR && stream_direction_can_read(driver->info.direction) &&
        driver->read_scalar == NULL && driver->read_scalar_sample == NULL) {
        return false;
    }
    if (driver->info.type == SOLAR_OS_STREAM_TYPE_AUDIO &&
        (driver->info.audio.sample_rate == 0U ||
         driver->info.audio.channels == 0U ||
         driver->info.audio.bits_per_sample == 0U)) {
        return false;
    }
    if (driver->info.type == SOLAR_OS_STREAM_TYPE_VIDEO &&
        (driver->info.direction != SOLAR_OS_STREAM_DIRECTION_SOURCE ||
         driver->info.video.codec != SOLAR_OS_STREAM_VIDEO_JPEG ||
         driver->info.video.width == 0U || driver->info.video.height == 0U ||
         driver->acquire_frame == NULL || driver->release_frame == NULL)) {
        return false;
    }
    return true;
}

static void stream_copy_info_locked(const stream_entry_t *entry,
                                    solar_os_stream_info_t *info)
{
    *info = entry->driver.info;
    info->active_handles = entry->active_handles;
    strlcpy(info->owner, entry->owner, sizeof(info->owner));
    info->read_units = entry->read_units;
    info->written_units = entry->written_units;
    info->overruns = entry->overruns;
    info->underruns = entry->underruns;
}

esp_err_t solar_os_stream_init(void)
{
    return stream_ensure_init();
}

esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *driver)
{
    if (!stream_driver_valid(driver)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = stream_ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    stream_lock();
    if (stream_find_locked(driver->info.id) >= 0) {
        stream_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    stream_entry_t *entry = NULL;
    for (size_t i = 0; i < SOLAR_OS_STREAM_MAX; i++) {
        if (!streams[i].registered) {
            entry = &streams[i];
            break;
        }
    }
    if (entry == NULL) {
        stream_unlock();
        return ESP_ERR_NO_MEM;
    }

    const uint32_t generation = entry->generation + 1U;
    memset(entry, 0, sizeof(*entry));
    entry->registered = true;
    entry->generation = generation != 0U ? generation : 1U;
    entry->driver = *driver;
    entry->driver.info.id[sizeof(entry->driver.info.id) - 1U] = '\0';
    entry->driver.info.provider[sizeof(entry->driver.info.provider) - 1U] = '\0';
    entry->driver.info.device[sizeof(entry->driver.info.device) - 1U] = '\0';
    entry->driver.info.unit[sizeof(entry->driver.info.unit) - 1U] = '\0';
    entry->driver.info.format[sizeof(entry->driver.info.format) - 1U] = '\0';
    entry->driver.info.summary[sizeof(entry->driver.info.summary) - 1U] = '\0';
    entry->driver.info.active_handles = 0U;
    entry->driver.info.owner[0] = '\0';
    entry->driver.info.read_units = 0U;
    entry->driver.info.written_units = 0U;
    entry->driver.info.overruns = 0U;
    entry->driver.info.underruns = 0U;
    stream_unlock();
    return ESP_OK;
}

esp_err_t solar_os_stream_unregister(const char *id)
{
    if (!stream_text_valid(id, SOLAR_OS_STREAM_ID_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = stream_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    stream_lock();
    const int index = stream_find_locked(id);
    if (index < 0) {
        stream_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    stream_entry_t *entry = &streams[index];
    if (entry->active_handles != 0U || entry->observers != 0U) {
        stream_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t generation = entry->generation;
    memset(entry, 0, sizeof(*entry));
    entry->generation = generation;
    stream_unlock();
    return ESP_OK;
}

esp_err_t solar_os_stream_unregister_many(const char *const *ids, size_t count)
{
    if (ids == NULL || count == 0U || count > SOLAR_OS_STREAM_MAX) return ESP_ERR_INVALID_ARG;
    for (size_t i=0; i<count; ++i) {
        if (!stream_text_valid(ids[i], SOLAR_OS_STREAM_ID_MAX, true)) return ESP_ERR_INVALID_ARG;
        for (size_t j=0; j<i; ++j) {
            if (strcmp(ids[i], ids[j]) == 0) return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t err = stream_ensure_init();
    if (err != ESP_OK) return err;
    int indexes[SOLAR_OS_STREAM_MAX];
    stream_lock();
    for (size_t i=0; i<count; ++i) {
        indexes[i] = stream_find_locked(ids[i]);
        if (indexes[i] < 0 || streams[indexes[i]].active_handles != 0U || streams[indexes[i]].observers != 0U) {
            err = indexes[i] < 0 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
            stream_unlock();
            return err;
        }
    }
    for (size_t i=0; i<count; ++i) {
        stream_entry_t *entry = &streams[indexes[i]];
        const uint32_t generation = entry->generation;
        memset(entry, 0, sizeof(*entry));
        entry->generation = generation;
    }
    stream_unlock();
    return ESP_OK;
}

size_t solar_os_stream_count(void)
{
    if (stream_ensure_init() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    stream_lock();
    for (size_t i = 0; i < SOLAR_OS_STREAM_MAX; i++) {
        if (streams[i].registered) {
            count++;
        }
    }
    stream_unlock();
    return count;
}

bool solar_os_stream_get(size_t index, solar_os_stream_info_t *info)
{
    if (info == NULL || stream_ensure_init() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    stream_lock();
    for (size_t i = 0; i < SOLAR_OS_STREAM_MAX; i++) {
        if (!streams[i].registered) {
            continue;
        }
        if (current++ == index) {
            stream_copy_info_locked(&streams[i], info);
            stream_unlock();
            return true;
        }
    }
    stream_unlock();
    return false;
}

esp_err_t solar_os_stream_get_info(const char *id, solar_os_stream_info_t *info)
{
    if (!stream_text_valid(id, SOLAR_OS_STREAM_ID_MAX, true) || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = stream_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    stream_lock();
    const int index = stream_find_locked(id);
    if (index < 0) {
        stream_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    stream_copy_info_locked(&streams[index], info);
    stream_unlock();
    return ESP_OK;
}

esp_err_t solar_os_stream_set_unit(const char *id,const char *unit)
{
    if(!stream_text_valid(id,SOLAR_OS_STREAM_ID_MAX,true) ||
        !stream_text_valid(unit,SOLAR_OS_STREAM_UNIT_MAX,false)) return ESP_ERR_INVALID_ARG;
    esp_err_t err=stream_ensure_init();if(err!=ESP_OK) return err;
    stream_lock();int index=stream_find_locked(id);
    if(index<0) err=ESP_ERR_NOT_FOUND;
    else strlcpy(streams[index].driver.info.unit,unit?unit:"",SOLAR_OS_STREAM_UNIT_MAX);
    stream_unlock();return err;
}

const char *solar_os_stream_type_name(solar_os_stream_type_t type)
{
    switch (type) {
    case SOLAR_OS_STREAM_TYPE_SCALAR: return "scalar";
    case SOLAR_OS_STREAM_TYPE_EVENT: return "event";
    case SOLAR_OS_STREAM_TYPE_BYTES: return "bytes";
    case SOLAR_OS_STREAM_TYPE_AUDIO: return "audio";
    case SOLAR_OS_STREAM_TYPE_VIDEO: return "video";
    default: return "unknown";
    }
}

const char *solar_os_stream_direction_name(solar_os_stream_direction_t direction)
{
    switch (direction) {
    case SOLAR_OS_STREAM_DIRECTION_SOURCE: return "source";
    case SOLAR_OS_STREAM_DIRECTION_SINK: return "sink";
    case SOLAR_OS_STREAM_DIRECTION_DUPLEX: return "duplex";
    default: return "unknown";
    }
}

const char *solar_os_stream_sharing_name(solar_os_stream_sharing_t sharing)
{
    switch (sharing) {
    case SOLAR_OS_STREAM_SHARING_SHARED: return "shared";
    case SOLAR_OS_STREAM_SHARING_EXCLUSIVE: return "exclusive";
    case SOLAR_OS_STREAM_SHARING_FANOUT: return "fanout";
    case SOLAR_OS_STREAM_SHARING_MIXED: return "mixed";
    default: return "unknown";
    }
}

const char *solar_os_stream_audio_sample_format_name(
    solar_os_stream_audio_sample_format_t format)
{
    return format == SOLAR_OS_STREAM_AUDIO_S16_LE ? "s16le" : "unknown";
}

static solar_os_stream_direction_t stream_default_open_direction(
    solar_os_stream_direction_t endpoint)
{
    return endpoint == SOLAR_OS_STREAM_DIRECTION_SINK ?
        SOLAR_OS_STREAM_DIRECTION_SINK : SOLAR_OS_STREAM_DIRECTION_SOURCE;
}

esp_err_t solar_os_stream_open_ex(const char *id,
                                  const char *owner,
                                  const solar_os_stream_open_options_t *options,
                                  solar_os_stream_handle_t *handle)
{
    if (!stream_text_valid(id, SOLAR_OS_STREAM_ID_MAX, true) ||
        !stream_text_valid(owner, SOLAR_OS_STREAM_OWNER_MAX, true) ||
        handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = stream_ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    solar_os_stream_open_fn open = NULL;
    void *user = NULL;
    if (solar_os_stream_handle_valid(handle)) {
        return ESP_ERR_INVALID_STATE;
    }
    *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    stream_lock();
    const int index = stream_find_locked(id);
    if (index < 0) {
        stream_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    stream_entry_t *entry = &streams[index];
    const solar_os_stream_direction_t direction = options != NULL ?
        options->direction : stream_default_open_direction(entry->driver.info.direction);
    if (!stream_direction_compatible(entry->driver.info.direction, direction)) {
        stream_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (entry->driver.info.sharing == SOLAR_OS_STREAM_SHARING_EXCLUSIVE &&
        entry->active_handles != 0U) {
        stream_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(handle->id, entry->driver.info.id, sizeof(handle->id));
    handle->type = entry->driver.info.type;
    handle->direction = direction;
    handle->slot = index;
    handle->generation = entry->generation;
    handle->audio = entry->driver.info.audio;
    entry->active_handles++;
    if (entry->driver.info.sharing == SOLAR_OS_STREAM_SHARING_EXCLUSIVE) {
        strlcpy(entry->owner, owner, sizeof(entry->owner));
    } else {
        strlcpy(entry->owner, "shared", sizeof(entry->owner));
    }
    open = entry->driver.open;
    user = entry->driver.user;
    stream_unlock();

    solar_os_stream_open_options_t defaults = {
        .direction = direction,
        .timeout_ms = 0U,
    };
    if (open != NULL) {
        err = open(user, owner, options != NULL ? options : &defaults, handle);
        if (err != ESP_OK) {
            stream_lock();
            if (index < (int)SOLAR_OS_STREAM_MAX && streams[index].registered &&
                streams[index].generation == handle->generation &&
                streams[index].active_handles > 0U) {
                streams[index].active_handles--;
                if (streams[index].active_handles == 0U) {
                    streams[index].owner[0] = '\0';
                }
            }
            stream_unlock();
            *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_stream_open(const char *id,
                               const char *owner,
                               solar_os_stream_handle_t *handle)
{
    return solar_os_stream_open_ex(id, owner, NULL, handle);
}

bool solar_os_stream_handle_valid(const solar_os_stream_handle_t *handle)
{
    if (handle == NULL || handle->slot < 0 ||
        handle->slot >= (int)SOLAR_OS_STREAM_MAX ||
        stream_ensure_init() != ESP_OK) {
        return false;
    }
    stream_lock();
    const stream_entry_t *entry = &streams[handle->slot];
    const bool valid = entry->registered &&
        entry->generation == handle->generation &&
        strcmp(entry->driver.info.id, handle->id) == 0;
    stream_unlock();
    return valid;
}

esp_err_t solar_os_stream_close_ex(solar_os_stream_handle_t *handle)
{
    if (!solar_os_stream_handle_valid(handle)) {
        if (handle != NULL) {
            *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (handle->leased_frame != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int slot = handle->slot;
    const uint32_t generation = handle->generation;
    solar_os_stream_close_fn close = NULL;
    esp_err_t (*close_checked)(void *, solar_os_stream_handle_t *) = NULL;
    void *user = NULL;
    stream_lock();
    close = streams[slot].driver.close;
    close_checked = streams[slot].driver.close_checked;
    user = streams[slot].driver.user;
    stream_unlock();
    if (close_checked != NULL) {
        const esp_err_t error = close_checked(user, handle);
        if (error != ESP_OK) {
            return error;
        }
    } else if (close != NULL) {
        close(user, handle);
    }
    stream_lock();
    if (streams[slot].registered && streams[slot].generation == generation &&
        streams[slot].active_handles > 0U) {
        streams[slot].active_handles--;
        if (streams[slot].active_handles == 0U) {
            streams[slot].owner[0] = '\0';
        }
    }
    stream_unlock();
    *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    return ESP_OK;
}

void solar_os_stream_close(solar_os_stream_handle_t *handle)
{
    (void)solar_os_stream_close_ex(handle);
}

static esp_err_t stream_get_callbacks(solar_os_stream_handle_t *handle,
                                      stream_entry_t **entry,
                                      void **user)
{
    if (handle == NULL || handle->slot < 0 ||
        handle->slot >= (int)SOLAR_OS_STREAM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    stream_entry_t *candidate = &streams[handle->slot];
    if (!candidate->registered || candidate->generation != handle->generation) {
        return ESP_ERR_INVALID_STATE;
    }
    *entry = candidate;
    *user = candidate->driver.user;
    return ESP_OK;
}

static esp_err_t stream_frame_operation(solar_os_stream_handle_t *handle,
                                        solar_os_stream_video_frame_t *frame,
                                        bool acquire)
{
    if (handle == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stream_ensure_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    stream_entry_t *entry = NULL;
    void *user = NULL;
    esp_err_t (*callback)(void *, solar_os_stream_handle_t *,
                          solar_os_stream_video_frame_t *) = NULL;
    stream_lock();
    esp_err_t error = stream_get_callbacks(handle, &entry, &user);
    if (error == ESP_OK) {
        if (handle->type != SOLAR_OS_STREAM_TYPE_VIDEO ||
            !stream_direction_can_read(handle->direction)) {
            error = ESP_ERR_NOT_SUPPORTED;
        } else if ((acquire && handle->leased_frame != NULL) ||
                   (!acquire && handle->leased_frame != frame)) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            callback = acquire ? entry->driver.acquire_frame : entry->driver.release_frame;
            if (callback == NULL) error = ESP_ERR_NOT_SUPPORTED;
        }
    }
    stream_unlock();
    if (error != ESP_OK) return error;
    error = callback(user, handle, frame);
    if (error == ESP_OK) {
        handle->leased_frame = acquire ? frame : NULL;
        if (acquire) {
            stream_lock();
            entry->read_units++;
            stream_unlock();
        } else {
            memset(frame, 0, sizeof(*frame));
        }
    }
    return error;
}

esp_err_t solar_os_stream_acquire_frame(solar_os_stream_handle_t *handle,
                                       solar_os_stream_video_frame_t *frame)
{
    return stream_frame_operation(handle, frame, true);
}

esp_err_t solar_os_stream_release_frame(solar_os_stream_handle_t *handle,
                                       solar_os_stream_video_frame_t *frame)
{
    return stream_frame_operation(handle, frame, false);
}

static size_t stream_transfer_units(const stream_entry_t *entry,
                                    const solar_os_stream_handle_t *handle,
                                    size_t bytes)
{
    if (entry->driver.info.type != SOLAR_OS_STREAM_TYPE_AUDIO) {
        return bytes;
    }
    const solar_os_stream_audio_format_t *format = handle != NULL ?
        &handle->audio : &entry->driver.info.audio;
    const size_t frame_bytes =
        ((size_t)format->channels * format->bits_per_sample) / 8U;
    return frame_bytes != 0U ? bytes / frame_bytes : 0U;
}

esp_err_t solar_os_stream_read(solar_os_stream_handle_t *handle,
                               void *data,
                               size_t len,
                               uint32_t timeout_ms,
                               size_t *read_len)
{
    if (data == NULL || len == 0U || read_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *read_len = 0U;
    if (stream_ensure_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    stream_entry_t *entry = NULL;
    void *user = NULL;
    solar_os_stream_read_fn read = NULL;
    stream_lock();
    esp_err_t err = stream_get_callbacks(handle, &entry, &user);
    if (err == ESP_OK) {
        if (!stream_direction_can_read(handle->direction)) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            read = entry->driver.read;
            if (read == NULL) {
                err = ESP_ERR_NOT_SUPPORTED;
            }
        }
    }
    stream_unlock();
    if (err != ESP_OK) {
        return err;
    }
    err = read(user, handle, data, len, timeout_ms, read_len);
    stream_lock();
    if (entry->registered && entry->generation == handle->generation) {
        if (err == ESP_OK) {
            entry->read_units += stream_transfer_units(entry, handle, *read_len);
        } else if (err == ESP_ERR_TIMEOUT) {
            entry->underruns++;
        }
    }
    stream_unlock();
    return err;
}

esp_err_t solar_os_stream_write(solar_os_stream_handle_t *handle,
                                const void *data,
                                size_t len,
                                uint32_t timeout_ms,
                                size_t *written)
{
    if (data == NULL || len == 0U || written == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *written = 0U;
    if (stream_ensure_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    stream_entry_t *entry = NULL;
    void *user = NULL;
    solar_os_stream_write_fn write = NULL;
    stream_lock();
    esp_err_t err = stream_get_callbacks(handle, &entry, &user);
    if (err == ESP_OK) {
        if (!stream_direction_can_write(handle->direction)) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            write = entry->driver.write;
            if (write == NULL) {
                err = ESP_ERR_NOT_SUPPORTED;
            } else if (entry->driver.info.has_scalar_range) {
                float value;
                if (len != sizeof(value)) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    memcpy(&value, data, sizeof(value));
                    if (!isfinite(value) ||
                        value < entry->driver.info.scalar_minimum ||
                        value > entry->driver.info.scalar_maximum) {
                        err = ESP_ERR_INVALID_ARG;
                    }
                }
            }
        }
    }
    stream_unlock();
    if (err != ESP_OK) {
        return err;
    }
    err = write(user, handle, data, len, timeout_ms, written);
    stream_lock();
    if (entry->registered && entry->generation == handle->generation) {
        if (err == ESP_OK) {
            entry->written_units += stream_transfer_units(entry, handle, *written);
        } else if (err == ESP_ERR_TIMEOUT) {
            entry->overruns++;
        }
    }
    stream_unlock();
    return err;
}

static void stream_record_timestamp(solar_os_stream_csv_record_t *record)
{
    record->uptime_ms = solar_os_time_uptime_ms();
    record->time_valid = solar_os_time_get_utc_epoch_ms(&record->time_ms) == ESP_OK;
}

static int stream_csv_prefix(const solar_os_stream_csv_record_t *record,
                             const char *id,
                             char *line,
                             size_t line_len)
{
    if (record->time_valid) {
        return snprintf(line, line_len, "%" PRIu64 ",%" PRIu64 ",%s,",
                        record->time_ms, record->uptime_ms, id);
    }
    return snprintf(line, line_len, ",%" PRIu64 ",%s,", record->uptime_ms, id);
}

static void stream_hex_encode(const uint8_t *data, size_t len,
                              char *out, size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    size_t pos = 0U;
    for (size_t i = 0; i < len && pos + 2U < out_len; i++) {
        out[pos++] = hex[(data[i] >> 4) & 0x0fU];
        out[pos++] = hex[data[i] & 0x0fU];
    }
    out[pos] = '\0';
}

static void stream_text_encode(const uint8_t *data, size_t len,
                               char *out, size_t out_len)
{
    size_t pos = 0U;
    out[pos++] = '"';
    for (size_t i = 0; i < len && pos + 2U < out_len; i++) {
        const unsigned char ch = data[i];
        out[pos++] = isprint(ch) && ch != '\r' && ch != '\n' && ch != '"' ?
            (char)ch : '.';
    }
    out[pos++] = '"';
    out[pos < out_len ? pos : out_len - 1U] = '\0';
}

esp_err_t solar_os_stream_csv_header(const solar_os_stream_info_t *info,
                                     char *header,
                                     size_t header_len)
{
    if (info == NULL || header == NULL || header_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stream_ensure_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_stream_csv_header_fn callback = NULL;
    void *user = NULL;
    stream_lock();
    const int index = stream_find_locked(info->id);
    if (index >= 0) {
        callback = streams[index].driver.csv_header;
        user = streams[index].driver.user;
    }
    stream_unlock();
    if (callback != NULL) {
        return callback(user, header, header_len);
    }
    if (info->type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const char *text = info->type == SOLAR_OS_STREAM_TYPE_BYTES ?
        "time_ms,uptime_ms,stream,hex,text" :
        info->type == SOLAR_OS_STREAM_TYPE_EVENT ?
            "time_ms,uptime_ms,stream,value" :
            "time_ms,uptime_ms,stream,value";
    strlcpy(header, text, header_len);
    return strlen(text) + 1U <= header_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_stream_read_scalar(
    solar_os_stream_handle_t *handle,
    const solar_os_stream_read_options_t *options,
    float *value)
{
    if (value == NULL || stream_ensure_init() != ESP_OK) {
        return value == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_NO_MEM;
    }
    stream_entry_t *entry = NULL;
    void *user = NULL;
    solar_os_stream_read_scalar_fn read_scalar = NULL;
    solar_os_stream_read_scalar_sample_fn read_sample = NULL;
    stream_lock();
    esp_err_t err = stream_get_callbacks(handle, &entry, &user);
    if (err == ESP_OK) {
        if (handle->type != SOLAR_OS_STREAM_TYPE_SCALAR ||
            !stream_direction_can_read(handle->direction)) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            read_scalar = entry->driver.read_scalar;
            read_sample = entry->driver.read_scalar_sample;
            if (read_scalar == NULL && read_sample == NULL) {
                err = ESP_ERR_NOT_SUPPORTED;
            }
        }
    }
    stream_unlock();
    if (err != ESP_OK) {
        return err;
    }
    if(read_scalar) err = read_scalar(user, options, value);
    else {
        solar_os_stream_scalar_sample_t sample;
        err=read_sample(user,options,&sample);
        if(err==ESP_OK) *value=sample.value;
    }
    if (err == ESP_OK) {
        stream_lock();
        if (entry->registered && entry->generation == handle->generation) {
            entry->read_units++;
        }
        stream_unlock();
    }
    return err;
}

esp_err_t solar_os_stream_observe_scalar_sample(const char *id,
    const solar_os_stream_read_options_t *options,solar_os_stream_scalar_sample_t *sample)
{
    if(!stream_text_valid(id,SOLAR_OS_STREAM_ID_MAX,true) || !sample) return ESP_ERR_INVALID_ARG;
    esp_err_t err=stream_ensure_init();if(err!=ESP_OK) return err;
    stream_lock();int index=stream_find_locked(id);
    if(index<0) { stream_unlock();return ESP_ERR_NOT_FOUND; }
    stream_entry_t *entry=&streams[index];
    solar_os_stream_read_scalar_sample_fn callback=entry->driver.observe_scalar_sample;
    if(entry->driver.info.type!=SOLAR_OS_STREAM_TYPE_SCALAR ||
        !stream_direction_can_read(entry->driver.info.direction) || !callback) {
        stream_unlock();return ESP_ERR_NOT_SUPPORTED;
    }
    entry->observers++;void *user=entry->driver.user;stream_unlock();
    err=callback(user,options,sample);
    stream_lock();entry->observers--;if(err==ESP_OK) entry->read_units++;stream_unlock();
    return err;
}

esp_err_t solar_os_stream_read_scalar_sample(solar_os_stream_handle_t *handle,
    const solar_os_stream_read_options_t *options, solar_os_stream_scalar_sample_t *sample)
{
    if (sample == NULL || stream_ensure_init() != ESP_OK) return ESP_ERR_INVALID_ARG;
    stream_entry_t *entry = NULL;
    void *user = NULL;
    solar_os_stream_read_scalar_sample_fn callback = NULL;
    stream_lock();
    esp_err_t err = stream_get_callbacks(handle, &entry, &user);
    if (err == ESP_OK) {
        callback = entry->driver.read_scalar_sample;
        if (handle->type != SOLAR_OS_STREAM_TYPE_SCALAR ||
            !stream_direction_can_read(handle->direction) || callback == NULL)
            err = ESP_ERR_NOT_SUPPORTED;
    }
    stream_unlock();
    if (err != ESP_OK) return err;
    err = callback(user, options, sample);
    if (err == ESP_OK) {
        stream_lock();
        if (entry->registered && entry->generation == handle->generation) entry->read_units++;
        stream_unlock();
    }
    return err;
}

esp_err_t solar_os_stream_read_csv(solar_os_stream_handle_t *handle,
                                   const solar_os_stream_read_options_t *options,
                                   solar_os_stream_csv_record_t *record)
{
    if (record == NULL || stream_ensure_init() != ESP_OK) {
        return record == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_NO_MEM;
    }
    stream_entry_t *entry = NULL;
    void *user = NULL;
    solar_os_stream_read_csv_fn callback = NULL;
    stream_lock();
    esp_err_t err = stream_get_callbacks(handle, &entry, &user);
    if (err == ESP_OK) {
        callback = entry->driver.read_csv;
    }
    stream_unlock();
    if (err != ESP_OK) {
        return err;
    }
    if (callback != NULL) {
        err = callback(user, handle, options, record);
        if (err == ESP_OK) {
            stream_lock();
            if (entry->registered && entry->generation == handle->generation) {
                entry->read_units++;
            }
            stream_unlock();
        }
        return err;
    }

    memset(record, 0, sizeof(*record));
    record->has_data = true;
    stream_record_timestamp(record);
    const int offset = stream_csv_prefix(record, handle->id,
                                         record->line, sizeof(record->line));
    if (offset < 0 || (size_t)offset >= sizeof(record->line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (handle->type == SOLAR_OS_STREAM_TYPE_SCALAR) {
        float value = 0.0f;
        err = solar_os_stream_read_scalar(handle, options, &value);
        if (err != ESP_OK) {
            return err;
        }
        snprintf(&record->line[offset], sizeof(record->line) - (size_t)offset,
                 "%.6g", (double)value);
        snprintf(record->change_key, sizeof(record->change_key),
                 "%.6g", (double)value);
        return ESP_OK;
    }
    if (handle->type == SOLAR_OS_STREAM_TYPE_BYTES) {
        uint8_t data[STREAM_BYTE_READ_MAX];
        size_t read_len = 0U;
        err = solar_os_stream_read(handle, data, sizeof(data),
                                   options != NULL ? options->timeout_ms : 0U,
                                   &read_len);
        if (err != ESP_OK) {
            return err;
        }
        char hex[(STREAM_BYTE_READ_MAX * 2U) + 1U];
        char text[STREAM_BYTE_READ_MAX + 3U];
        stream_hex_encode(data, read_len, hex, sizeof(hex));
        stream_text_encode(data, read_len, text, sizeof(text));
        snprintf(&record->line[offset], sizeof(record->line) - (size_t)offset,
                 "%s,%s", hex, text);
        strlcpy(record->change_key, hex, sizeof(record->change_key));
        return ESP_OK;
    }
    if (handle->type == SOLAR_OS_STREAM_TYPE_EVENT) {
        uint8_t value = 0U;
        size_t read_len = 0U;
        err = solar_os_stream_read(handle, &value, sizeof(value),
                                   options != NULL ? options->timeout_ms : 0U,
                                   &read_len);
        if (err != ESP_OK) {
            return err;
        }
        if (read_len != sizeof(value)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        snprintf(&record->line[offset], sizeof(record->line) - (size_t)offset,
                 "%u", (unsigned)value);
        snprintf(record->change_key, sizeof(record->change_key),
                 "%u", (unsigned)value);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
