#include "solar_os_port.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool registered;
    char name[SOLAR_OS_PORT_NAME_MAX];
    char label[SOLAR_OS_PORT_LABEL_MAX];
    uint32_t capabilities;
    solar_os_port_read_fn read;
    solar_os_port_write_fn write;
    solar_os_port_open_fn open;
    solar_os_port_close_fn close;
    void *user;
    bool claimed;
    char owner[SOLAR_OS_PORT_OWNER_MAX];
    uint32_t token;
} solar_os_port_entry_t;

static SemaphoreHandle_t port_mutex;
static solar_os_port_entry_t ports[SOLAR_OS_PORT_MAX];
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

esp_err_t solar_os_port_init(void)
{
    return port_ensure_init();
}

esp_err_t solar_os_port_register(const solar_os_port_driver_t *driver)
{
    if (driver == NULL ||
        !port_valid_name(driver->name) ||
        driver->capabilities == 0 ||
        (driver->label != NULL && strlen(driver->label) >= SOLAR_OS_PORT_LABEL_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = port_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    port_lock();
    if (port_find_locked(driver->name) >= 0) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_port_entry_t *slot = NULL;
    for (size_t i = 0; i < SOLAR_OS_PORT_MAX; i++) {
        if (!ports[i].registered) {
            slot = &ports[i];
            break;
        }
    }
    if (slot == NULL) {
        port_unlock();
        return ESP_ERR_NO_MEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->registered = true;
    strlcpy(slot->name, driver->name, sizeof(slot->name));
    strlcpy(slot->label, driver->label != NULL ? driver->label : driver->name, sizeof(slot->label));
    slot->capabilities = driver->capabilities;
    slot->read = driver->read;
    slot->write = driver->write;
    slot->open = driver->open;
    slot->close = driver->close;
    slot->user = driver->user;
    port_unlock();
    return ESP_OK;
}

esp_err_t solar_os_port_unregister(const char *name)
{
    if (!port_valid_name(name)) {
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
    if (ports[index].claimed) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&ports[index], 0, sizeof(ports[index]));
    port_unlock();
    return ESP_OK;
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
    if (entry->claimed) {
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
    const uint32_t token = entry->token;
    port_unlock();

    if (open_fn != NULL) {
        ret = open_fn(user);
        if (ret != ESP_OK) {
            port_lock();
            if (entry->registered && entry->claimed && entry->token == token) {
                entry->claimed = false;
                entry->owner[0] = '\0';
                entry->token = 0;
            }
            port_unlock();
            handle->index = -1;
            handle->token = 0;
            return ret;
        }
    }
    return ESP_OK;
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
    const solar_os_port_close_fn close_fn = entry->close;
    void *user = entry->user;
    const int index = handle->index;
    const uint32_t token = handle->token;
    port_unlock();

    if (close_fn != NULL) {
        ret = close_fn(user);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    port_lock();
    entry = &ports[index];
    if (!entry->registered || !entry->claimed || entry->token != token) {
        port_unlock();
        return ESP_ERR_INVALID_STATE;
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
    const solar_os_port_entry_t *entry = &ports[handle->index];
    if ((entry->capabilities & SOLAR_OS_PORT_CAP_READ) == 0 || entry->read == NULL) {
        port_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    read_fn = entry->read;
    user = entry->user;
    port_unlock();

    return read_fn(user, data, len, timeout_ms, read_len);
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
    const solar_os_port_entry_t *entry = &ports[handle->index];
    if ((entry->capabilities & SOLAR_OS_PORT_CAP_WRITE) == 0 || entry->write == NULL) {
        port_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    write_fn = entry->write;
    user = entry->user;
    port_unlock();

    return write_fn(user, data, len, written);
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
    if (index < 0) {
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
        if (!ports[i].registered) {
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
