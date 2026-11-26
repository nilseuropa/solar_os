#include "solar_os_identity.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"

#define SOLAR_OS_IDENTITY_NVS_NAMESPACE "identity"
#define SOLAR_OS_IDENTITY_USER_KEY "user"
#define SOLAR_OS_IDENTITY_HOSTNAME_KEY "hostname"
#define SOLAR_OS_IDENTITY_DIR ".solar"
#define SOLAR_OS_IDENTITY_USER_FILE "user"
#define SOLAR_OS_IDENTITY_HOSTNAME_FILE "hostname"

static const char *TAG = "identity";
static portMUX_TYPE identity_lock = portMUX_INITIALIZER_UNLOCKED;
static bool identity_initialized;
static SemaphoreHandle_t identity_mutex;
static StaticSemaphore_t identity_mutex_storage;
static char identity_user[SOLAR_OS_IDENTITY_USER_MAX] =
    SOLAR_OS_IDENTITY_DEFAULT_USER;
static char identity_hostname[SOLAR_OS_IDENTITY_HOSTNAME_MAX] =
    SOLAR_OS_IDENTITY_DEFAULT_HOSTNAME;

static bool identity_char_is_valid(char ch)
{
    const unsigned char value = (unsigned char)ch;
    return isalnum(value) || ch == '-' || ch == '_' || ch == '.';
}

static bool identity_value_is_valid(const char *value, size_t max_len)
{
    if (value == NULL || value[0] == '\0' || strnlen(value, max_len) >= max_len) {
        return false;
    }

    for (const char *p = value; *p != '\0'; p++) {
        if (!identity_char_is_valid(*p)) {
            return false;
        }
    }
    return true;
}

static void identity_trim(char *value)
{
    if (value == NULL) {
        return;
    }

    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) {
        value[--len] = '\0';
    }

    char *start = value;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != value) {
        memmove(value, start, strlen(start) + 1);
    }
}

static bool identity_read_legacy_file(const char *name,
                                      char *buffer,
                                      size_t len)
{
    if (buffer == NULL || len == 0) {
        return false;
    }

    if (!solar_os_storage_is_mounted()) {
        return false;
    }

    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(SOLAR_OS_IDENTITY_DIR, dir, sizeof(dir)) != ESP_OK ||
        solar_os_storage_join_path(dir, name, path, sizeof(path)) != ESP_OK) {
        return false;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }

    bool loaded = false;
    char value[64];
    if (fgets(value, sizeof(value), file) != NULL) {
        identity_trim(value);
        if (identity_value_is_valid(value, len)) {
            strlcpy(buffer, value, len);
            loaded = true;
        }
    }
    fclose(file);
    return loaded;
}

static bool identity_read_nvs(nvs_handle_t nvs,
                              const char *key,
                              char *buffer,
                              size_t len)
{
    char value[SOLAR_OS_IDENTITY_USER_MAX > SOLAR_OS_IDENTITY_HOSTNAME_MAX ?
               SOLAR_OS_IDENTITY_USER_MAX : SOLAR_OS_IDENTITY_HOSTNAME_MAX];
    size_t stored_len = sizeof(value);
    if (nvs_get_str(nvs, key, value, &stored_len) != ESP_OK ||
        !identity_value_is_valid(value, len)) return false;
    strlcpy(buffer, value, len);
    return true;
}

static esp_err_t identity_write_nvs(nvs_handle_t nvs,
                                    const char *key,
                                    const char *value)
{
    esp_err_t ret = nvs_set_str(nvs, key, value);
    return ret == ESP_OK ? nvs_commit(nvs) : ret;
}

static esp_err_t identity_load(void)
{
    if (identity_initialized) return ESP_OK;
    char user[SOLAR_OS_IDENTITY_USER_MAX] = SOLAR_OS_IDENTITY_DEFAULT_USER;
    char hostname[SOLAR_OS_IDENTITY_HOSTNAME_MAX] =
        SOLAR_OS_IDENTITY_DEFAULT_HOSTNAME;
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(SOLAR_OS_IDENTITY_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    const bool user_in_nvs =
        identity_read_nvs(nvs, SOLAR_OS_IDENTITY_USER_KEY, user, sizeof(user));
    const bool hostname_in_nvs =
        identity_read_nvs(nvs,
                          SOLAR_OS_IDENTITY_HOSTNAME_KEY,
                          hostname,
                          sizeof(hostname));
    bool migrated = false;
    if (!user_in_nvs &&
        identity_read_legacy_file(SOLAR_OS_IDENTITY_USER_FILE,
                                  user,
                                  sizeof(user))) {
        ret = nvs_set_str(nvs, SOLAR_OS_IDENTITY_USER_KEY, user);
        migrated = ret == ESP_OK;
    }
    if (ret == ESP_OK &&
        !hostname_in_nvs &&
        identity_read_legacy_file(SOLAR_OS_IDENTITY_HOSTNAME_FILE,
                                  hostname,
                                  sizeof(hostname))) {
        ret = nvs_set_str(nvs, SOLAR_OS_IDENTITY_HOSTNAME_KEY, hostname);
        migrated = ret == ESP_OK || migrated;
    }
    if (ret == ESP_OK && migrated) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&identity_lock);
    strlcpy(identity_user, user, sizeof(identity_user));
    strlcpy(identity_hostname, hostname, sizeof(identity_hostname));
    identity_initialized = true;
    portEXIT_CRITICAL(&identity_lock);

    if (migrated) {
        SOLAR_OS_LOGI(TAG, "migrated legacy .solar identity to NVS");
    }
    return ESP_OK;
}

static SemaphoreHandle_t identity_update_mutex(void)
{
    portENTER_CRITICAL(&identity_lock);
    if (!identity_mutex) identity_mutex = xSemaphoreCreateMutexStatic(&identity_mutex_storage);
    const SemaphoreHandle_t mutex = identity_mutex;
    portEXIT_CRITICAL(&identity_lock);
    return mutex;
}

esp_err_t solar_os_identity_init(void)
{
    portENTER_CRITICAL(&identity_lock);
    const bool already_initialized = identity_initialized;
    portEXIT_CRITICAL(&identity_lock);
    if (already_initialized) return ESP_OK;

    const SemaphoreHandle_t mutex = identity_update_mutex();
    if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;
    const esp_err_t error = identity_load();
    xSemaphoreGive(mutex);
    return error;
}

void solar_os_identity_get(solar_os_identity_v1_t *identity)
{
    if (!identity) return;
    (void)solar_os_identity_init();
    portENTER_CRITICAL(&identity_lock);
    memcpy(identity->user, identity_user, sizeof(identity->user));
    memcpy(identity->hostname, identity_hostname, sizeof(identity->hostname));
    portEXIT_CRITICAL(&identity_lock);
}

void solar_os_identity_get_user(char *buffer, size_t len)
{
    if (!buffer || !len) return;
    solar_os_identity_v1_t identity;
    solar_os_identity_get(&identity);
    strlcpy(buffer, identity.user, len);
}

void solar_os_identity_get_hostname(char *buffer, size_t len)
{
    if (!buffer || !len) return;
    solar_os_identity_v1_t identity;
    solar_os_identity_get(&identity);
    strlcpy(buffer, identity.hostname, len);
}

esp_err_t solar_os_identity_set(solar_os_identity_field_t field, const char *value)
{
    const char *key;
    char *cached;
    size_t capacity;
    switch (field) {
    case SOLAR_OS_IDENTITY_USER:
        key = SOLAR_OS_IDENTITY_USER_KEY;
        cached = identity_user;
        capacity = sizeof(identity_user);
        break;
    case SOLAR_OS_IDENTITY_HOSTNAME:
        key = SOLAR_OS_IDENTITY_HOSTNAME_KEY;
        cached = identity_hostname;
        capacity = sizeof(identity_hostname);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = value ? strnlen(value, capacity) : 0;
    if (!length || length == capacity) return ESP_ERR_INVALID_ARG;
    /* Use one validated copy for both persistence and the live value. */
    char text[SOLAR_OS_IDENTITY_USER_MAX > SOLAR_OS_IDENTITY_HOSTNAME_MAX ?
              SOLAR_OS_IDENTITY_USER_MAX : SOLAR_OS_IDENTITY_HOSTNAME_MAX] = {0};
    memcpy(text, value, length);
    if (!identity_value_is_valid(text, capacity)) return ESP_ERR_INVALID_ARG;

    esp_err_t error = solar_os_identity_init();
    if (error != ESP_OK) return error;
    const SemaphoreHandle_t mutex = identity_update_mutex();
    if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;
    nvs_handle_t nvs;
    error = nvs_open(SOLAR_OS_IDENTITY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error == ESP_OK) {
        error = identity_write_nvs(nvs, key, text);
        nvs_close(nvs);
    }
    if (error == ESP_OK) {
        portENTER_CRITICAL(&identity_lock);
        memcpy(cached, text, capacity);
        portEXIT_CRITICAL(&identity_lock);
    }
    xSemaphoreGive(mutex);
    return error;
}

esp_err_t solar_os_identity_set_user(const char *user)
{
    return solar_os_identity_set(SOLAR_OS_IDENTITY_USER, user);
}

esp_err_t solar_os_identity_set_hostname(const char *hostname)
{
    return solar_os_identity_set(SOLAR_OS_IDENTITY_HOSTNAME, hostname);
}

void solar_os_identity_format(char *buffer, size_t len)
{
    if (!buffer || !len) return;
    solar_os_identity_v1_t identity;
    solar_os_identity_get(&identity);
    snprintf(buffer, len, "%s@%s", identity.user, identity.hostname);
}
