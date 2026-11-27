#include "solar_os_port.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"
#include "solar_os_stream.h"

typedef struct {
    bool registered;
    bool registering;
    bool retiring;
    char name[SOLAR_OS_PORT_NAME_MAX];
    char label[SOLAR_OS_PORT_LABEL_MAX];
    uint32_t capabilities;
    solar_os_port_read_fn read;
    solar_os_port_write_fn write;
    solar_os_port_open_fn open;
    solar_os_port_close_fn close;
    solar_os_port_control_fn control;
    void *user;
    unsigned inflight;
    bool claimed;
    char owner[SOLAR_OS_PORT_OWNER_MAX];
    uint32_t token;
} solar_os_port_entry_t;

static SemaphoreHandle_t port_mutex;
/* Stable entries are shared by task-level stream callbacks only. */
static EXT_RAM_BSS_ATTR solar_os_port_entry_t ports[SOLAR_OS_PORT_MAX];
static uint32_t next_token = 1;

static esp_err_t port_ensure_init(void)
{
    if (port_mutex != NULL) {
        return ESP_OK;
    }

    port_mutex = xSemaphoreCreateMutex();
    return port_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void port_lock(void)
{
    if (port_mutex != NULL) {
        xSemaphoreTake(port_mutex, portMAX_DELAY);
    }
}

static void port_unlock(void)
{
    if (port_mutex != NULL) {
        xSemaphoreGive(port_mutex);
    }
}

static bool port_valid_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= SOLAR_OS_PORT_NAME_MAX) {
        return false;
    }

    for (const char *p = name; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        if (!isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

static int port_find_locked(const char *name)
{
    if (name == NULL) {
        return -1;
    }

    for (size_t i = 0; i < SOLAR_OS_PORT_MAX; i++) {
        if (ports[i].registered && strcmp(ports[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void port_fill_info_locked(const solar_os_port_entry_t *entry, solar_os_port_info_t *info)
{
    memset(info, 0, sizeof(*info));
    strlcpy(info->name, entry->name, sizeof(info->name));
    strlcpy(info->label, entry->label, sizeof(info->label));
    info->capabilities = entry->capabilities;
    info->claimed = entry->claimed;
    info->operations = entry->inflight;
    if (entry->claimed) {
        strlcpy(info->owner, entry->owner, sizeof(info->owner));
    }
}

static bool port_handle_valid_locked(const solar_os_port_handle_t *handle)
{
    if (handle == NULL || handle->index < 0 || handle->index >= (int)SOLAR_OS_PORT_MAX) {
        return false;
    }

    const solar_os_port_entry_t *entry = &ports[handle->index];
    return entry->registered && entry->claimed && entry->token == handle->token;
}

static esp_err_t port_stream_open(void *user,
                                  const char *owner,
                                  const solar_os_stream_open_options_t *options,
                                  solar_os_stream_handle_t *handle)
{
    (void)options;
    solar_os_port_entry_t *entry = user;
    if (entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_port_handle_t port = SOLAR_OS_PORT_HANDLE_INIT;
    const esp_err_t err = solar_os_port_claim(entry->name, owner, &port);
    if (err == ESP_OK) {
        handle->private_data[0] = (uintptr_t)(intptr_t)port.index;
        handle->private_data[1] = port.token;
    }
    return err;
}

static solar_os_port_handle_t port_stream_handle(
    const solar_os_stream_handle_t *handle)
{
    return (solar_os_port_handle_t){
        .index = (int)(intptr_t)handle->private_data[0],
        .token = (uint32_t)handle->private_data[1],
    };
}

static esp_err_t port_stream_close(void *user, solar_os_stream_handle_t *handle)
{
    (void)user;
    solar_os_port_handle_t port = port_stream_handle(handle);
    if (solar_os_port_handle_valid(&port)) {
        const esp_err_t error = solar_os_port_release(&port);
        if (error != ESP_OK) return error;
    }
    handle->private_data[0] = 0U;
    handle->private_data[1] = 0U;
    return ESP_OK;
}

static esp_err_t port_stream_read(void *user,
                                  solar_os_stream_handle_t *handle,
                                  void *data,
                                  size_t len,
                                  uint32_t timeout_ms,
                                  size_t *read_len)
{
    (void)user;
    const solar_os_port_handle_t port = port_stream_handle(handle);
    return solar_os_port_read(&port, data, len, timeout_ms, read_len);
}

static esp_err_t port_stream_write(void *user,
                                   solar_os_stream_handle_t *handle,
                                   const void *data,
                                   size_t len,
                                   uint32_t timeout_ms,
                                   size_t *written)
{
    (void)user;
    (void)timeout_ms;
    const solar_os_port_handle_t port = port_stream_handle(handle);
    return solar_os_port_write(&port, data, len, written);
}

esp_err_t solar_os_port_init(void)
{
    return port_ensure_init();
}

static bool port_valid_driver(const solar_os_port_driver_t *driver)
{
    return port_valid_name(driver->name) && driver->capabilities != 0 &&
        (driver->label == NULL || strlen(driver->label) < SOLAR_OS_PORT_LABEL_MAX);
}

static void port_reserve(solar_os_port_entry_t *slot, const solar_os_port_driver_t *driver)
{
    memset(slot, 0, sizeof(*slot));
    slot->registered = true;
    slot->registering = true;
    strlcpy(slot->name, driver->name, sizeof(slot->name));
    strlcpy(slot->label, driver->label != NULL ? driver->label : driver->name, sizeof(slot->label));
    slot->capabilities = driver->capabilities;
    slot->read = driver->read;
    slot->write = driver->write;
    slot->open = driver->open;
    slot->close = driver->close;
    slot->user = driver->user;
    slot->control = driver->control;
}

static esp_err_t port_reserve_many(const solar_os_port_driver_t *drivers,
                                  solar_os_port_entry_t **slots, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (port_find_locked(drivers[i].name) >= 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    size_t available = 0;
    for (size_t i = 0; i < SOLAR_OS_PORT_MAX && available < count; i++) {
        if (!ports[i].registered) {
            slots[available++] = &ports[i];
        }
    }
    if (available != count) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        port_reserve(slots[i], &drivers[i]);
    }
    return ESP_OK;
}

static void port_prepare_stream(solar_os_port_entry_t *slot, solar_os_stream_driver_t *stream)
{
    const uint32_t capabilities = slot->capabilities;
    solar_os_stream_direction_t direction = SOLAR_OS_STREAM_DIRECTION_SOURCE;
    if ((capabilities & (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) ==
        (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) {
        direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX;
    } else if ((capabilities & SOLAR_OS_PORT_CAP_WRITE) != 0U) {
        direction = SOLAR_OS_STREAM_DIRECTION_SINK;
    }
    *stream = (solar_os_stream_driver_t){
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_BYTES,
            .direction = direction,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
        },
        .open = port_stream_open,
        .close_checked = port_stream_close,
        .read = (capabilities & SOLAR_OS_PORT_CAP_READ) != 0U ? port_stream_read : NULL,
        .write = (capabilities & SOLAR_OS_PORT_CAP_WRITE) != 0U ? port_stream_write : NULL,
        .user = slot,
    };
    strlcpy(stream->info.id, slot->name, sizeof(stream->info.id));
    strlcpy(stream->info.provider, "port", sizeof(stream->info.provider));
    strlcpy(stream->info.device, slot->name, sizeof(stream->info.device));
    strlcpy(stream->info.unit, "bytes", sizeof(stream->info.unit));
    strlcpy(stream->info.format, "bytes", sizeof(stream->info.format));
    strlcpy(stream->info.summary, slot->label, sizeof(stream->info.summary));
}

static esp_err_t port_register_group(const solar_os_port_driver_t *drivers,
                                     solar_os_stream_driver_t *streams, size_t count)
{
    solar_os_port_entry_t *slots[SOLAR_OS_PORT_MAX];
    port_lock();
    esp_err_t result = port_reserve_many(drivers, slots, count);
    port_unlock();
    if (result != ESP_OK) {
        return result;
    }

    for (size_t i = 0; i < count; i++) {
        port_prepare_stream(slots[i], &streams[i]);
    }
    result = solar_os_stream_register_many(streams, count);

    port_lock();
    for (size_t i = 0; i < count; i++) {
        if (result == ESP_OK) {
            slots[i]->registering = false;
        } else {
            memset(slots[i], 0, sizeof(*slots[i]));
        }
    }
    port_unlock();
    return result;
}

esp_err_t solar_os_port_register(const solar_os_port_driver_t *driver)
{
    if (driver == NULL || !port_valid_driver(driver)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = port_ensure_init();
    if (result != ESP_OK) {
        return result;
    }
    solar_os_stream_driver_t stream;
    return port_register_group(driver, &stream, 1);
}

esp_err_t solar_os_port_register_many(const solar_os_port_driver_t *drivers, size_t count)
{
    if (drivers == NULL || count == 0 || count > SOLAR_OS_PORT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 1) {
        return solar_os_port_register(drivers);
    }
    for (size_t i = 0; i < count; i++) {
        if (!port_valid_driver(&drivers[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(drivers[i].name, drivers[j].name) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    const esp_err_t ready = port_ensure_init();
    if (ready != ESP_OK) {
        return ready;
    }
    solar_os_stream_driver_t *streams = solar_os_memory_calloc(count, sizeof(*streams),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "port.registration");
    if (streams == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t result = port_register_group(drivers, streams, count);
    solar_os_memory_free(streams);
    return result;
}

esp_err_t solar_os_port_unregister(const char *name)
{
    return solar_os_port_unregister_many(&name, 1);
}

static esp_err_t port_unregister_many(const char *const *names, const void *const *users, size_t count)
{
    if (!names || !count || count > SOLAR_OS_PORT_MAX) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        if (!port_valid_name(names[i])) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(names[i], names[j])) return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = port_ensure_init();
    if (ret != ESP_OK) return ret;
    solar_os_port_entry_t *entries[SOLAR_OS_PORT_MAX];
    const char *stream_ids[SOLAR_OS_PORT_MAX];
    const void *stream_users[SOLAR_OS_PORT_MAX];
    size_t selected = 0;
    port_lock();
    for (size_t i = 0; i < count; i++) {
        const int index = port_find_locked(names[i]);
        if (users && (index < 0 || ports[index].user != users[i])) {
            continue;
        }
        if (index < 0 || ports[index].registering || ports[index].claimed ||
            ports[index].retiring || ports[index].inflight) {
            port_unlock();
            return index < 0 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
        }
        entries[selected] = &ports[index];
        stream_ids[selected] = names[i];
        stream_users[selected++] = &ports[index];
    }
    for (size_t i = 0; i < selected; i++) {
        entries[i]->retiring = true;
    }
    port_unlock();

    if (selected == 0) {
        return ESP_OK;
    }
    if (users) {
        ret = solar_os_stream_unregister_many_owned(stream_ids, stream_users, selected);
    } else {
        ret = count == 1 ? solar_os_stream_unregister(names[0]) : solar_os_stream_unregister_many(names, count);
        if (count == 1 && ret == ESP_ERR_NOT_FOUND) {
            ret = ESP_OK;
        }
    }

    port_lock();
    for (size_t i = 0; i < selected; i++) {
        if (ret == ESP_OK) {
            memset(entries[i], 0, sizeof(*entries[i]));
        } else {
            entries[i]->retiring = false;
        }
    }
    port_unlock();
    return ret;
}

esp_err_t solar_os_port_unregister_many(const char *const *names, size_t count)
{
    return port_unregister_many(names, NULL, count);
}

esp_err_t solar_os_port_unregister_many_owned(const char *const *names,
                                               const void *const *users, size_t count)
{
    return users != NULL ? port_unregister_many(names, users, count) : ESP_ERR_INVALID_ARG;
}

static void port_io_done(int index)
{
    port_lock();
    ports[index].inflight--;
    port_unlock();
}

esp_err_t solar_os_port_claim(const char *name,
                              const char *owner,
                              solar_os_port_handle_t *handle)
{
    if (!port_valid_name(name) ||
        owner == NULL ||
        owner[0] == '\0' ||
        strlen(owner) >= SOLAR_OS_PORT_OWNER_MAX ||
        handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = port_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    port_lock();
    const int index = port_find_locked(name);
    if (index < 0) {
        port_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    solar_os_port_entry_t *entry = &ports[index];
    if (entry->registering || entry->claimed || entry->retiring || entry->inflight) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    entry->claimed = true;
    strlcpy(entry->owner, owner, sizeof(entry->owner));
    entry->token = next_token++;
    if (next_token == 0) {
        next_token = 1;
    }
    handle->index = index;
    handle->token = entry->token;
    const solar_os_port_open_fn open_fn = entry->open;
    void *user = entry->user;
    entry->retiring = open_fn != NULL;
    port_unlock();

    if (open_fn != NULL) {
        ret = open_fn(user);
        port_lock();
        entry->retiring = false;
        if (ret != ESP_OK) {
            entry->claimed = false;
            entry->owner[0] = '\0';
            entry->token = 0;
            *handle = (solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;
        }
        port_unlock();
    }
    return ret;
}

esp_err_t solar_os_port_release(solar_os_port_handle_t *handle)
{
    esp_err_t ret = port_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    port_lock();
    if (!port_handle_valid_locked(handle)) {
        port_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_port_entry_t *entry = &ports[handle->index];
    if (entry->retiring) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    entry->retiring = true;
    const solar_os_port_close_fn close_fn = entry->close;
    void *user = entry->user;
    const int index = handle->index;
    const uint32_t token = handle->token;
    port_unlock();

    if (close_fn != NULL) {
        ret = close_fn(user);
    }

    port_lock();
    entry = &ports[index];
    if (!entry->registered || !entry->claimed || entry->token != token) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    entry->retiring = false;
    if (ret != ESP_OK) {
        port_unlock();
        return ret;
    }
    entry->claimed = false;
    entry->owner[0] = '\0';
    entry->token = 0;
    handle->index = -1;
    handle->token = 0;
    port_unlock();
    return ESP_OK;
}

bool solar_os_port_handle_valid(const solar_os_port_handle_t *handle)
{
    if (port_ensure_init() != ESP_OK) {
        return false;
    }

    port_lock();
    const bool valid = port_handle_valid_locked(handle);
    port_unlock();
    return valid;
}

esp_err_t solar_os_port_read(const solar_os_port_handle_t *handle,
                             uint8_t *data,
                             size_t len,
                             uint32_t timeout_ms,
                             size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = port_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_port_read_fn read_fn = NULL;
    void *user = NULL;
    port_lock();
    if (!port_handle_valid_locked(handle)) {
        port_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_port_entry_t *entry = &ports[handle->index];
    if (entry->retiring) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if ((entry->capabilities & SOLAR_OS_PORT_CAP_READ) == 0 || entry->read == NULL) {
        port_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    read_fn = entry->read;
    user = entry->user;
    const int index = handle->index;
    entry->inflight++;
    port_unlock();

    ret = read_fn(user, data, len, timeout_ms, read_len);
    port_io_done(index);
    return ret;
}

esp_err_t solar_os_port_write(const solar_os_port_handle_t *handle,
                              const uint8_t *data,
                              size_t len,
                              size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = port_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_port_write_fn write_fn = NULL;
    void *user = NULL;
    port_lock();
    if (!port_handle_valid_locked(handle)) {
        port_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_port_entry_t *entry = &ports[handle->index];
    if (entry->retiring) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if ((entry->capabilities & SOLAR_OS_PORT_CAP_WRITE) == 0 || entry->write == NULL) {
        port_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    write_fn = entry->write;
    user = entry->user;
    const int index = handle->index;
    entry->inflight++;
    port_unlock();

    ret = write_fn(user, data, len, written);
    port_io_done(index);
    return ret;
}

static esp_err_t port_serial_control(const solar_os_port_handle_t *handle,
    solar_os_port_control_t request, void *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    esp_err_t err = port_ensure_init();
    if (err != ESP_OK) return err;
    port_lock();
    if (!port_handle_valid_locked(handle)) { port_unlock(); return ESP_ERR_INVALID_ARG; }
    solar_os_port_entry_t *entry = &ports[handle->index];
    if (entry->retiring) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!(entry->capabilities & SOLAR_OS_PORT_CAP_SERIAL) || !entry->control) {
        port_unlock(); return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_port_control_fn control = entry->control;
    void *user = entry->user;
    const int index = handle->index;
    entry->inflight++;
    port_unlock();
    err = control(user, request, data);
    port_io_done(index);
    return err;
}

esp_err_t solar_os_port_serial_get(const solar_os_port_handle_t *handle, solar_os_port_serial_config_t *config)
{
    return port_serial_control(handle, SOLAR_OS_PORT_SERIAL_GET, config);
}

esp_err_t solar_os_port_serial_configure(const solar_os_port_handle_t *handle,
    const solar_os_port_serial_config_t *config, uint32_t mask)
{
    if (!config || !mask || (mask & ~SOLAR_OS_SERIAL_ALL)) return ESP_ERR_INVALID_ARG;
    solar_os_port_serial_update_t update = {.config = *config, .mask = mask};
    return port_serial_control(handle, SOLAR_OS_PORT_SERIAL_UPDATE, &update);
}

esp_err_t solar_os_port_serial_break(const solar_os_port_handle_t *handle, uint16_t duration_ms)
{
    return port_serial_control(handle, SOLAR_OS_PORT_SERIAL_BREAK, &duration_ms);
}

esp_err_t solar_os_port_get_info(const char *name, solar_os_port_info_t *info)
{
    if (!port_valid_name(name) || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = port_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    port_lock();
    const int index = port_find_locked(name);
    if (index < 0 || ports[index].registering) {
        port_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    port_fill_info_locked(&ports[index], info);
    port_unlock();
    return ESP_OK;
}

size_t solar_os_port_list(solar_os_port_info_t *out_ports, size_t max_ports)
{
    if (port_ensure_init() != ESP_OK) {
        return 0;
    }

    size_t count = 0;
    port_lock();
    for (size_t i = 0; i < SOLAR_OS_PORT_MAX; i++) {
        if (!ports[i].registered || ports[i].registering) {
            continue;
        }
        if (out_ports != NULL && count < max_ports) {
            port_fill_info_locked(&ports[i], &out_ports[count]);
        }
        count++;
    }
    port_unlock();
    return count;
}

const char *solar_os_port_capabilities_text(uint32_t capabilities,
                                            char *buffer,
                                            size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return "";
    }

    char text[4] = {'-', '-', '-', '\0'};
    if ((capabilities & SOLAR_OS_PORT_CAP_READ) != 0) {
        text[0] = 'r';
    }
    if ((capabilities & SOLAR_OS_PORT_CAP_WRITE) != 0) {
        text[1] = 'w';
    }
    if ((capabilities & SOLAR_OS_PORT_CAP_CONFIG) != 0) {
        text[2] = 'c';
    }

    snprintf(buffer, buffer_len, "%s", text);
    return buffer;
}
