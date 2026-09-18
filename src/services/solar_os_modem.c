#include "solar_os_modem.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define MODEM_DEVICE_MAX 3U
#define MODEM_NVS_NAMESPACE "cellular"
#define MODEM_PROFILE_VERSION 1U

typedef struct {
    bool active;
    solar_os_modem_info_t info;
    const solar_os_modem_ops_t *ops;
    void *ctx;
    size_t refs;
    uint32_t generation;
} modem_device_t;

typedef struct {
    uint32_t version;
    char name[SOLAR_OS_MODEM_NAME_MAX];
    solar_os_modem_profile_t profile;
} modem_profile_record_t;

typedef struct {
    const solar_os_modem_ops_t *ops;
    void *ctx;
    size_t index;
    uint32_t generation;
} modem_ref_t;

static SemaphoreHandle_t modem_mutex;
static StaticSemaphore_t modem_mutex_storage;
static modem_device_t modem_devices[MODEM_DEVICE_MAX];
static uint32_t modem_next_generation = 1U;

static esp_err_t ensure_mutex(void)
{
    if (modem_mutex == NULL) {
        modem_mutex = xSemaphoreCreateMutexStatic(&modem_mutex_storage);
    }
    return modem_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool bounded_name_valid(const char *name, size_t capacity)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, capacity) < capacity;
}

static bool modem_name_valid(const char *name)
{
    return bounded_name_valid(name, SOLAR_OS_MODEM_NAME_MAX);
}

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static bool apn_valid(const char *apn)
{
    const size_t len = strnlen(apn, SOLAR_OS_MODEM_APN_MAX + 1U);
    if (len == 0U || len > SOLAR_OS_MODEM_APN_MAX ||
        apn[0] == '.' || apn[len - 1U] == '.') {
        return false;
    }
    bool previous_dot = false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)apn[i];
        if (!(isalnum(ch) || ch == '-' || ch == '.')) {
            return false;
        }
        if (ch == '.' && previous_dot) {
            return false;
        }
        previous_dot = ch == '.';
    }
    return true;
}

static bool credential_valid(const char *value, size_t max_len)
{
    const size_t len = strnlen(value, max_len + 1U);
    if (len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20U || ch > 0x7eU || ch == '"' || ch == '\\') {
            return false;
        }
    }
    return true;
}

esp_err_t solar_os_modem_profile_validate(
    const solar_os_modem_profile_t *profile)
{
    if (profile == NULL || !apn_valid(profile->apn) ||
        profile->ip_type > SOLAR_OS_MODEM_IP_IPV4V6 ||
        profile->auth > SOLAR_OS_MODEM_AUTH_AUTO ||
        !credential_valid(profile->username, SOLAR_OS_MODEM_USERNAME_MAX) ||
        !credential_valid(profile->password, SOLAR_OS_MODEM_PASSWORD_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool have_username = profile->username[0] != '\0';
    const bool have_password = profile->password[0] != '\0';
    if (profile->auth == SOLAR_OS_MODEM_AUTH_PAP ||
        profile->auth == SOLAR_OS_MODEM_AUTH_CHAP) {
        return have_username && have_password ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    return !have_username && !have_password ? ESP_OK : ESP_ERR_INVALID_ARG;
}

bool solar_os_modem_ip_type_parse(const char *name,
                                  solar_os_modem_ip_type_t *type)
{
    if (name == NULL || type == NULL) {
        return false;
    }
    if (strcmp(name, "ipv4") == 0) {
        *type = SOLAR_OS_MODEM_IP_IPV4;
    } else if (strcmp(name, "ipv6") == 0) {
        *type = SOLAR_OS_MODEM_IP_IPV6;
    } else if (strcmp(name, "ipv4v6") == 0) {
        *type = SOLAR_OS_MODEM_IP_IPV4V6;
    } else {
        return false;
    }
    return true;
}

const char *solar_os_modem_ip_type_name(solar_os_modem_ip_type_t type)
{
    switch (type) {
    case SOLAR_OS_MODEM_IP_IPV4:
        return "ipv4";
    case SOLAR_OS_MODEM_IP_IPV6:
        return "ipv6";
    case SOLAR_OS_MODEM_IP_IPV4V6:
        return "ipv4v6";
    default:
        return "unknown";
    }
}

bool solar_os_modem_auth_parse(const char *name, solar_os_modem_auth_t *auth)
{
    if (name == NULL || auth == NULL) {
        return false;
    }
    if (strcmp(name, "none") == 0) {
        *auth = SOLAR_OS_MODEM_AUTH_NONE;
    } else if (strcmp(name, "pap") == 0) {
        *auth = SOLAR_OS_MODEM_AUTH_PAP;
    } else if (strcmp(name, "chap") == 0) {
        *auth = SOLAR_OS_MODEM_AUTH_CHAP;
    } else if (strcmp(name, "auto") == 0) {
        *auth = SOLAR_OS_MODEM_AUTH_AUTO;
    } else {
        return false;
    }
    return true;
}

const char *solar_os_modem_auth_name(solar_os_modem_auth_t auth)
{
    switch (auth) {
    case SOLAR_OS_MODEM_AUTH_NONE:
        return "none";
    case SOLAR_OS_MODEM_AUTH_PAP:
        return "pap";
    case SOLAR_OS_MODEM_AUTH_CHAP:
        return "chap";
    case SOLAR_OS_MODEM_AUTH_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

const char *solar_os_modem_registration_name(
    solar_os_modem_network_registration_t registration)
{
    switch (registration) {
    case SOLAR_OS_MODEM_REGISTRATION_NOT_REGISTERED:
        return "not-registered";
    case SOLAR_OS_MODEM_REGISTRATION_SEARCHING:
        return "searching";
    case SOLAR_OS_MODEM_REGISTRATION_DENIED:
        return "denied";
    case SOLAR_OS_MODEM_REGISTRATION_HOME:
        return "home";
    case SOLAR_OS_MODEM_REGISTRATION_ROAMING:
        return "roaming";
    case SOLAR_OS_MODEM_REGISTRATION_UNKNOWN:
    default:
        return "unknown";
    }
}

static void profile_key(const char *name, char key[10])
{
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *ch = (const unsigned char *)name;
         *ch != '\0'; ch++) {
        hash ^= *ch;
        hash *= UINT32_C(16777619);
    }
    (void)snprintf(key, 10U, "p%08" PRIx32, hash);
}

static esp_err_t profile_load(const char *name,
                              solar_os_modem_profile_t *profile)
{
    if (!modem_name_valid(name) || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MODEM_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    char key[10];
    profile_key(name, key);
    modem_profile_record_t record = {0};
    size_t size = sizeof(record);
    ret = nvs_get_blob(nvs, key, &record, &size);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (size != sizeof(record) || record.version != MODEM_PROFILE_VERSION ||
        strncmp(record.name, name, sizeof(record.name)) != 0 ||
        solar_os_modem_profile_validate(&record.profile) != ESP_OK) {
        secure_zero(&record, sizeof(record));
        return ESP_ERR_INVALID_RESPONSE;
    }
    *profile = record.profile;
    secure_zero(&record, sizeof(record));
    return ESP_OK;
}

static esp_err_t profile_save(const char *name,
                              const solar_os_modem_profile_t *profile)
{
    modem_profile_record_t record = {
        .version = MODEM_PROFILE_VERSION,
        .profile = *profile,
    };
    strlcpy(record.name, name, sizeof(record.name));
    char key[10];
    profile_key(name, key);
    nvs_handle_t nvs;
    bool opened = false;
    esp_err_t ret = nvs_open(MODEM_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        opened = true;
        ret = nvs_set_blob(nvs, key, &record, sizeof(record));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    if (opened) {
        nvs_close(nvs);
    }
    secure_zero(&record, sizeof(record));
    return ret;
}

static esp_err_t profile_remove(const char *name)
{
    char key[10];
    profile_key(name, key);
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MODEM_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_erase_key(nvs, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t acquire_device(const char *name, modem_ref_t *ref)
{
    if (!modem_name_valid(name) || ref == NULL || ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(modem_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MODEM_DEVICE_MAX; i++) {
        if (modem_devices[i].active &&
            strcmp(modem_devices[i].info.name, name) == 0) {
            modem_devices[i].refs++;
            *ref = (modem_ref_t) {
                .ops = modem_devices[i].ops,
                .ctx = modem_devices[i].ctx,
                .index = i,
                .generation = modem_devices[i].generation,
            };
            xSemaphoreGive(modem_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(modem_mutex);
    return ESP_ERR_NOT_FOUND;
}

static void release_device(const modem_ref_t *ref)
{
    xSemaphoreTake(modem_mutex, portMAX_DELAY);
    if (ref->index < MODEM_DEVICE_MAX &&
        modem_devices[ref->index].active &&
        modem_devices[ref->index].generation == ref->generation &&
        modem_devices[ref->index].refs > 0U) {
        modem_devices[ref->index].refs--;
    }
    xSemaphoreGive(modem_mutex);
}

esp_err_t solar_os_modem_register(
    const solar_os_modem_registration_t *registration)
{
    if (registration == NULL || !modem_name_valid(registration->name) ||
        !bounded_name_valid(registration->driver, SOLAR_OS_MODEM_DRIVER_MAX) ||
        !bounded_name_valid(registration->transport,
                            SOLAR_OS_MODEM_TRANSPORT_MAX) ||
        registration->ops == NULL || registration->ops->get_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_mutex() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(modem_mutex, portMAX_DELAY);
    modem_device_t *free_device = NULL;
    for (size_t i = 0; i < MODEM_DEVICE_MAX; i++) {
        if (modem_devices[i].active &&
            strcmp(modem_devices[i].info.name, registration->name) == 0) {
            xSemaphoreGive(modem_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!modem_devices[i].active && free_device == NULL) {
            free_device = &modem_devices[i];
        }
    }
    if (free_device == NULL) {
        xSemaphoreGive(modem_mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(free_device, 0, sizeof(*free_device));
    free_device->active = true;
    strlcpy(free_device->info.name,
            registration->name,
            sizeof(free_device->info.name));
    strlcpy(free_device->info.driver,
            registration->driver,
            sizeof(free_device->info.driver));
    strlcpy(free_device->info.transport,
            registration->transport,
            sizeof(free_device->info.transport));
    free_device->info.profile_support =
        registration->ops->apply_profile != NULL &&
        registration->ops->clear_profile != NULL;
    free_device->info.data_control =
        registration->ops->set_data_active != NULL;
    free_device->info.sim_unlock = registration->ops->unlock_sim != NULL;
    free_device->info.raw_command = registration->ops->command != NULL;
    free_device->ops = registration->ops;
    free_device->ctx = registration->ctx;
    free_device->generation = modem_next_generation++;
    if (free_device->generation == 0U) {
        free_device->generation = modem_next_generation++;
    }
    xSemaphoreGive(modem_mutex);
    return ESP_OK;
}

esp_err_t solar_os_modem_unregister(const char *name)
{
    if (!modem_name_valid(name) || ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(modem_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MODEM_DEVICE_MAX; i++) {
        if (modem_devices[i].active &&
            strcmp(modem_devices[i].info.name, name) == 0) {
            if (modem_devices[i].refs > 0U) {
                xSemaphoreGive(modem_mutex);
                return ESP_ERR_INVALID_STATE;
            }
            memset(&modem_devices[i], 0, sizeof(modem_devices[i]));
            xSemaphoreGive(modem_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(modem_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_modem_count(void)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(modem_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MODEM_DEVICE_MAX; i++) {
        count += modem_devices[i].active ? 1U : 0U;
    }
    xSemaphoreGive(modem_mutex);
    return count;
}

bool solar_os_modem_get(size_t index, solar_os_modem_info_t *info)
{
    if (info == NULL || ensure_mutex() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    xSemaphoreTake(modem_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MODEM_DEVICE_MAX; i++) {
        if (!modem_devices[i].active) {
            continue;
        }
        if (current++ == index) {
            *info = modem_devices[i].info;
            xSemaphoreGive(modem_mutex);
            return true;
        }
    }
    xSemaphoreGive(modem_mutex);
    return false;
}

esp_err_t solar_os_modem_get_status(const char *name,
                                    solar_os_modem_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    modem_ref_t ref = {0};
    esp_err_t ret = acquire_device(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ref.ops->get_status(ref.ctx, status);
    release_device(&ref);
    return ret;
}

esp_err_t solar_os_modem_profile_set(const char *name,
                                     const solar_os_modem_profile_t *profile)
{
    esp_err_t ret = solar_os_modem_profile_validate(profile);
    if (ret != ESP_OK) {
        return ret;
    }
    modem_ref_t ref;
    ret = acquire_device(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ref.ops->apply_profile == NULL) {
        release_device(&ref);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = ref.ops->apply_profile(ref.ctx, profile);
    release_device(&ref);
    if (ret == ESP_OK) {
        ret = profile_save(name, profile);
    }
    return ret;
}

esp_err_t solar_os_modem_profile_get(const char *name,
                                     solar_os_modem_profile_t *profile)
{
    return profile_load(name, profile);
}

esp_err_t solar_os_modem_profile_clear(const char *name)
{
    if (!modem_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    modem_ref_t ref = {0};
    esp_err_t ret = acquire_device(name, &ref);
    if (ret == ESP_OK) {
        if (ref.ops->clear_profile == NULL) {
            release_device(&ref);
            return ESP_ERR_NOT_SUPPORTED;
        }
        ret = ref.ops->clear_profile(ref.ctx);
        release_device(&ref);
    }
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }
    return profile_remove(name);
}

esp_err_t solar_os_modem_set_data_active(const char *name, bool active)
{
    solar_os_modem_profile_t profile = {0};
    if (active) {
        const esp_err_t load_ret = profile_load(name, &profile);
        if (load_ret != ESP_OK) {
            return load_ret;
        }
    }
    modem_ref_t ref = {0};
    esp_err_t ret = acquire_device(name, &ref);
    if (ret == ESP_OK && ref.ops->set_data_active == NULL) {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
    if (ret == ESP_OK && active) {
        if (ref.ops->apply_profile == NULL) {
            ret = ESP_ERR_NOT_SUPPORTED;
        } else {
            ret = ref.ops->apply_profile(ref.ctx, &profile);
        }
    }
    if (ret == ESP_OK) {
        ret = ref.ops->set_data_active(ref.ctx, active);
    }
    if (ref.ops != NULL) {
        release_device(&ref);
    }
    secure_zero(&profile, sizeof(profile));
    return ret;
}

esp_err_t solar_os_modem_unlock_sim(const char *name, const char *pin)
{
    if (pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    modem_ref_t ref;
    esp_err_t ret = acquire_device(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ref.ops->unlock_sim != NULL
        ? ref.ops->unlock_sim(ref.ctx, pin)
        : ESP_ERR_NOT_SUPPORTED;
    release_device(&ref);
    return ret;
}

esp_err_t solar_os_modem_command(const char *name,
                                 const char *command,
                                 uint32_t timeout_ms,
                                 char *response,
                                 size_t response_size)
{
    modem_ref_t ref;
    esp_err_t ret = acquire_device(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ref.ops->command != NULL
        ? ref.ops->command(ref.ctx,
                           command,
                           timeout_ms,
                           response,
                           response_size)
        : ESP_ERR_NOT_SUPPORTED;
    release_device(&ref);
    return ret;
}
