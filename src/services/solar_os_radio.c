#include "solar_os_radio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define SOLAR_OS_RADIO_DEVICE_MAX 4
#define RADIO_RELEASE_WAIT_MS 3500U
#define RADIO_PROFILE_NVS_NAMESPACE "radio_prof"
#define RADIO_PROFILE_NVS_KEY "profiles"
#define RADIO_PROFILE_MAGIC 0x52504631U
#define RADIO_PROFILE_VERSION 1U

typedef struct {
    bool active;
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    const solar_os_radio_ops_t *ops;
    void *ctx;
    bool claimed;
    bool releasing;
    size_t handle_refs;
    char owner[SOLAR_OS_RADIO_OWNER_MAX];
    uint32_t token;
} radio_device_t;

typedef struct {
    bool active;
    char name[SOLAR_OS_RADIO_PROFILE_NAME_MAX];
    solar_os_radio_config_t config;
} radio_user_profile_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    radio_user_profile_t profiles[SOLAR_OS_RADIO_USER_PROFILE_MAX];
} radio_profile_store_t;

static const solar_os_radio_profile_t radio_builtin_profiles[] = {
    {
        .name = "meshcore-eu868",
        .builtin = true,
        .config = {
            .frequency_hz = 869618000,
            .modulation = SOLAR_OS_RADIO_MODULATION_LORA,
            .rx_bandwidth_hz = 62500,
            .spreading_factor = 8,
            .coding_rate_denominator = 8,
            .preamble_len = 32,
            .sync_word_len = 1,
            .sync_word = {0x12},
            .tx_power_dbm = 14,
            .crc_enabled = true,
            .variable_length = true,
            .payload_length = 255,
        },
    },
    {
        .name = "lora-eu868",
        .builtin = true,
        .config = {
            .frequency_hz = 868000000,
            .modulation = SOLAR_OS_RADIO_MODULATION_LORA,
            .rx_bandwidth_hz = 125000,
            .spreading_factor = 7,
            .coding_rate_denominator = 5,
            .preamble_len = 8,
            .sync_word_len = 1,
            .sync_word = {0x12},
            .tx_power_dbm = 13,
            .crc_enabled = true,
            .variable_length = true,
            .payload_length = 255,
        },
    },
    {
        .name = "gfsk-eu868",
        .builtin = true,
        .config = {
            .frequency_hz = 868000000,
            .modulation = SOLAR_OS_RADIO_MODULATION_GFSK,
            .bitrate_bps = 4800,
            .deviation_hz = 5000,
            .rx_bandwidth_hz = 12500,
            .spreading_factor = 7,
            .coding_rate_denominator = 5,
            .preamble_len = 3,
            .sync_word_len = 2,
            .sync_word = {0x2d, 0xd4},
            .tx_power_dbm = 13,
            .crc_enabled = true,
            .variable_length = true,
            .payload_length = 64,
        },
    },
    {
        .name = "ook-eu868",
        .builtin = true,
        .config = {
            .frequency_hz = 868000000,
            .modulation = SOLAR_OS_RADIO_MODULATION_OOK,
            .bitrate_bps = 4800,
            .rx_bandwidth_hz = 12500,
            .spreading_factor = 7,
            .coding_rate_denominator = 5,
            .preamble_len = 3,
            .sync_word_len = 2,
            .sync_word = {0x2d, 0xd4},
            .tx_power_dbm = 13,
            .crc_enabled = true,
            .variable_length = true,
            .payload_length = 64,
        },
    },
};

_Static_assert(sizeof(radio_builtin_profiles) / sizeof(radio_builtin_profiles[0]) ==
                   SOLAR_OS_RADIO_BUILTIN_PROFILE_COUNT,
               "built-in radio profile count mismatch");

static radio_device_t radio_devices[SOLAR_OS_RADIO_DEVICE_MAX];
static SemaphoreHandle_t radio_mutex;
static SemaphoreHandle_t radio_profile_mutex;
static uint32_t radio_next_token = 1;

static esp_err_t radio_ensure_init(void)
{
    if (radio_mutex != NULL) {
        return ESP_OK;
    }

    radio_mutex = xSemaphoreCreateMutex();
    if (radio_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool radio_name_valid(const char *name)
{
    return name != NULL &&
        name[0] != '\0' &&
        strnlen(name, SOLAR_OS_RADIO_NAME_MAX) < SOLAR_OS_RADIO_NAME_MAX;
}

static int radio_find_index_locked(const char *name)
{
    if (name == NULL) {
        return -1;
    }

    for (size_t i = 0; i < SOLAR_OS_RADIO_DEVICE_MAX; i++) {
        if (radio_devices[i].active && strcmp(radio_devices[i].info.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static radio_device_t *radio_alloc_locked(void)
{
    for (size_t i = 0; i < SOLAR_OS_RADIO_DEVICE_MAX; i++) {
        if (!radio_devices[i].active) {
            return &radio_devices[i];
        }
    }
    return NULL;
}

static bool radio_handle_valid_locked(const solar_os_radio_handle_t *handle)
{
    if (handle == NULL ||
        handle->index < 0 ||
        handle->index >= (int)SOLAR_OS_RADIO_DEVICE_MAX) {
        return false;
    }
    const radio_device_t *device = &radio_devices[handle->index];
    return device->active && device->claimed && device->token == handle->token;
}

static bool radio_handle_operation_acquire_locked(const solar_os_radio_handle_t *handle)
{
    if (!radio_handle_valid_locked(handle)) {
        return false;
    }
    radio_device_t *device = &radio_devices[handle->index];
    if (device->releasing) {
        return false;
    }
    device->handle_refs++;
    return true;
}

static void radio_handle_operation_release_locked(int index, uint32_t token)
{
    if (index < 0 || index >= (int)SOLAR_OS_RADIO_DEVICE_MAX) {
        return;
    }
    radio_device_t *device = &radio_devices[index];
    if (device->active && device->claimed && device->token == token &&
        device->handle_refs > 0) {
        device->handle_refs--;
    }
}

static void radio_fill_info_locked(const radio_device_t *device,
                                   solar_os_radio_info_t *info)
{
    *info = device->info;
    info->claimed = device->claimed;
    if (device->claimed) {
        strlcpy(info->owner, device->owner, sizeof(info->owner));
    } else {
        info->owner[0] = '\0';
    }
}

static bool radio_modulation_supported(solar_os_radio_modulation_t modulation,
                                       solar_os_radio_modulations_t supported)
{
    return modulation != SOLAR_OS_RADIO_MODULATION_NONE &&
        ((supported & (solar_os_radio_modulations_t)modulation) != 0);
}

static esp_err_t radio_validate_config(const solar_os_radio_config_t *config,
                                       solar_os_radio_modulations_t supported)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!radio_modulation_supported(config->modulation, supported)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->sync_word_len > SOLAR_OS_RADIO_SYNC_WORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void format_append(char *buffer, size_t buffer_len, size_t *used, bool *any, const char *token)
{
    if (buffer == NULL || buffer_len == 0 || used == NULL || any == NULL || token == NULL) {
        return;
    }
    if (*used >= buffer_len) {
        return;
    }

    const int written = snprintf(buffer + *used,
                                 buffer_len - *used,
                                 "%s%s",
                                 *any ? " " : "",
                                 token);
    if (written < 0) {
        buffer[*used] = '\0';
        return;
    }
    if ((size_t)written >= buffer_len - *used) {
        buffer[buffer_len - 1] = '\0';
        *used = buffer_len - 1;
        return;
    }
    *used += (size_t)written;
    *any = true;
}

esp_err_t solar_os_radio_init(void)
{
    return radio_ensure_init();
}

esp_err_t solar_os_radio_register(const solar_os_radio_registration_t *registration)
{
    if (registration == NULL ||
        !radio_name_valid(registration->name) ||
        registration->modulations == 0 ||
        registration->max_packet_len == 0 ||
        registration->max_packet_len > SOLAR_OS_RADIO_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_validate_config(&registration->default_config,
                                              registration->modulations),
                        "radio",
                        "invalid default config");
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (radio_find_index_locked(registration->name) >= 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    radio_device_t *device = radio_alloc_locked();
    if (device == NULL) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NO_MEM;
    }

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->info.name, registration->name, sizeof(device->info.name));
    strlcpy(device->info.driver,
            registration->driver != NULL ? registration->driver : "",
            sizeof(device->info.driver));
    strlcpy(device->info.summary,
            registration->summary != NULL ? registration->summary : "",
            sizeof(device->info.summary));
    device->info.modulations = registration->modulations;
    device->info.features = registration->features;
    device->info.max_packet_len = registration->max_packet_len;
    device->status.state = registration->initial_state;
    device->status.config = registration->default_config;
    device->ops = registration->ops;
    device->ctx = registration->ctx;

    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

esp_err_t solar_os_radio_unregister(const char *name)
{
    if (!radio_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (radio_devices[index].claimed) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&radio_devices[index], 0, sizeof(radio_devices[index]));
    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

size_t solar_os_radio_count(void)
{
    size_t count = 0;

    if (radio_ensure_init() != ESP_OK) {
        return 0;
    }

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_RADIO_DEVICE_MAX; i++) {
        if (radio_devices[i].active) {
            count++;
        }
    }
    xSemaphoreGive(radio_mutex);
    return count;
}

bool solar_os_radio_get(size_t index, solar_os_radio_info_t *info)
{
    size_t current = 0;

    if (info == NULL || radio_ensure_init() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_RADIO_DEVICE_MAX; i++) {
        if (!radio_devices[i].active) {
            continue;
        }
        if (current++ == index) {
            radio_fill_info_locked(&radio_devices[i], info);
            xSemaphoreGive(radio_mutex);
            return true;
        }
    }
    xSemaphoreGive(radio_mutex);
    return false;
}

esp_err_t solar_os_radio_get_info(const char *name, solar_os_radio_info_t *info)
{
    if (!radio_name_valid(name) || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    radio_fill_info_locked(&radio_devices[index], info);
    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

esp_err_t solar_os_radio_get_status(const char *name, solar_os_radio_status_t *status)
{
    if (!radio_name_valid(name) || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    solar_os_radio_status_t current = {0};

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    current = radio_devices[index].status;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if (ops != NULL && ops->get_status != NULL) {
        const esp_err_t ret = ops->get_status(ctx, &current);
        if (ret != ESP_OK) {
            return ret;
        }

        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        const int update_index = radio_find_index_locked(name);
        if (update_index >= 0) {
            radio_devices[update_index].status = current;
        }
        xSemaphoreGive(radio_mutex);
    }

    *status = current;
    return ESP_OK;
}

esp_err_t solar_os_radio_claim(const char *name,
                               const char *owner,
                               solar_os_radio_handle_t *handle)
{
    if (!radio_name_valid(name) ||
        owner == NULL ||
        owner[0] == '\0' ||
        strnlen(owner, SOLAR_OS_RADIO_OWNER_MAX) >= SOLAR_OS_RADIO_OWNER_MAX ||
        handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    radio_device_t *device = &radio_devices[index];
    if (device->claimed) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    device->claimed = true;
    device->releasing = false;
    device->handle_refs = 0;
    strlcpy(device->owner, owner, sizeof(device->owner));
    device->token = radio_next_token++;
    if (radio_next_token == 0) {
        radio_next_token = 1;
    }
    handle->index = index;
    handle->token = device->token;
    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

esp_err_t solar_os_radio_release(solar_os_radio_handle_t *handle)
{
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    int index = -1;
    uint32_t token = 0;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (!radio_handle_valid_locked(handle)) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    index = handle->index;
    token = handle->token;
    radio_devices[index].releasing = true;
    xSemaphoreGive(radio_mutex);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RADIO_RELEASE_WAIT_MS);
    for (;;) {
        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        radio_device_t *device = &radio_devices[index];
        if (!device->active || !device->claimed || device->token != token) {
            xSemaphoreGive(radio_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (device->handle_refs == 0) {
            device->claimed = false;
            device->releasing = false;
            device->owner[0] = '\0';
            device->token = 0;
            handle->index = -1;
            handle->token = 0;
            xSemaphoreGive(radio_mutex);
            return ESP_OK;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            device->releasing = false;
            xSemaphoreGive(radio_mutex);
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreGive(radio_mutex);
        vTaskDelay(1);
    }
}

bool solar_os_radio_handle_valid(const solar_os_radio_handle_t *handle)
{
    if (radio_ensure_init() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const bool valid =
        radio_handle_valid_locked(handle) && !radio_devices[handle->index].releasing;
    xSemaphoreGive(radio_mutex);
    return valid;
}

esp_err_t solar_os_radio_handle_get_status(const solar_os_radio_handle_t *handle,
                                           solar_os_radio_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    solar_os_radio_status_t current = {0};
    int index = -1;
    uint32_t token = 0;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (!radio_handle_operation_acquire_locked(handle)) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    index = handle->index;
    token = handle->token;
    current = radio_devices[index].status;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if (ops != NULL && ops->get_status != NULL) {
        const esp_err_t ret = ops->get_status(ctx, &current);
        if (ret != ESP_OK) {
            xSemaphoreTake(radio_mutex, portMAX_DELAY);
            radio_handle_operation_release_locked(index, token);
            xSemaphoreGive(radio_mutex);
            return ret;
        }
        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        if (index >= 0 &&
            index < (int)SOLAR_OS_RADIO_DEVICE_MAX &&
            radio_devices[index].active &&
            radio_devices[index].claimed &&
            radio_devices[index].token == token) {
            radio_devices[index].status = current;
        }
        radio_handle_operation_release_locked(index, token);
        xSemaphoreGive(radio_mutex);
    } else {
        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        radio_handle_operation_release_locked(index, token);
        xSemaphoreGive(radio_mutex);
    }
    *status = current;
    return ESP_OK;
}

esp_err_t solar_os_radio_configure(const char *name, const solar_os_radio_config_t *config)
{
    if (!radio_name_valid(name) || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    solar_os_radio_modulations_t supported = 0;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (radio_devices[index].claimed) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    supported = radio_devices[index].info.modulations;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    ESP_RETURN_ON_ERROR(radio_validate_config(config, supported), "radio", "invalid config");
    if (ops == NULL || ops->configure == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t ret = ops->configure(ctx, config);
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int update_index = radio_find_index_locked(name);
    if (update_index >= 0) {
        radio_devices[update_index].status.config = *config;
    }
    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

esp_err_t solar_os_radio_handle_configure(const solar_os_radio_handle_t *handle,
                                          const solar_os_radio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    solar_os_radio_modulations_t supported = 0;
    int index = -1;
    uint32_t token = 0;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (!radio_handle_operation_acquire_locked(handle)) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    index = handle->index;
    token = handle->token;
    supported = radio_devices[index].info.modulations;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    esp_err_t ret = radio_validate_config(config, supported);
    if (ret != ESP_OK) {
        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        radio_handle_operation_release_locked(index, token);
        xSemaphoreGive(radio_mutex);
        return ret;
    }
    if (ops == NULL || ops->configure == NULL) {
        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        radio_handle_operation_release_locked(index, token);
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = ops->configure(ctx, config);

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (ret == ESP_OK &&
        index >= 0 &&
        index < (int)SOLAR_OS_RADIO_DEVICE_MAX &&
        radio_devices[index].active &&
        radio_devices[index].claimed &&
        radio_devices[index].token == token) {
        radio_devices[index].status.config = *config;
    }
    radio_handle_operation_release_locked(index, token);
    xSemaphoreGive(radio_mutex);
    return ret;
}

esp_err_t solar_os_radio_set_state(const char *name, solar_os_radio_state_t state)
{
    if (!radio_name_valid(name) ||
        state == SOLAR_OS_RADIO_STATE_UNKNOWN ||
        state > SOLAR_OS_RADIO_STATE_TX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (radio_devices[index].claimed) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if (ops == NULL || ops->set_state == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t ret = ops->set_state(ctx, state);
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int update_index = radio_find_index_locked(name);
    if (update_index >= 0) {
        radio_devices[update_index].status.state = state;
    }
    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

esp_err_t solar_os_radio_handle_set_state(const solar_os_radio_handle_t *handle,
                                          solar_os_radio_state_t state)
{
    if (state == SOLAR_OS_RADIO_STATE_UNKNOWN ||
        state > SOLAR_OS_RADIO_STATE_TX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    int index = -1;
    uint32_t token = 0;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (!radio_handle_operation_acquire_locked(handle)) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    index = handle->index;
    token = handle->token;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if (ops == NULL || ops->set_state == NULL) {
        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        radio_handle_operation_release_locked(index, token);
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t ret = ops->set_state(ctx, state);

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (ret == ESP_OK &&
        index >= 0 &&
        index < (int)SOLAR_OS_RADIO_DEVICE_MAX &&
        radio_devices[index].active &&
        radio_devices[index].claimed &&
        radio_devices[index].token == token) {
        radio_devices[index].status.state = state;
    }
    radio_handle_operation_release_locked(index, token);
    xSemaphoreGive(radio_mutex);
    return ret;
}

esp_err_t solar_os_radio_send(const char *name,
                              const solar_os_radio_packet_t *packet,
                              uint32_t timeout_ms)
{
    if (!radio_name_valid(name) ||
        packet == NULL ||
        packet->len == 0 ||
        packet->len > SOLAR_OS_RADIO_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    size_t max_packet_len = 0;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (radio_devices[index].claimed) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    max_packet_len = radio_devices[index].info.max_packet_len;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if (packet->len > max_packet_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (ops == NULL || ops->send == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ops->send(ctx, packet, timeout_ms);
}

esp_err_t solar_os_radio_handle_send(const solar_os_radio_handle_t *handle,
                                     const solar_os_radio_packet_t *packet,
                                     uint32_t timeout_ms)
{
    if (packet == NULL ||
        packet->len == 0 ||
        packet->len > SOLAR_OS_RADIO_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    size_t max_packet_len = 0;
    int index = -1;
    uint32_t token = 0;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (!radio_handle_operation_acquire_locked(handle)) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    index = handle->index;
    token = handle->token;
    max_packet_len = radio_devices[index].info.max_packet_len;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    esp_err_t ret = ESP_OK;
    if (packet->len > max_packet_len) {
        ret = ESP_ERR_INVALID_SIZE;
    } else if (ops == NULL || ops->send == NULL) {
        ret = ESP_ERR_NOT_SUPPORTED;
    } else {
        ret = ops->send(ctx, packet, timeout_ms);
    }
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    radio_handle_operation_release_locked(index, token);
    xSemaphoreGive(radio_mutex);
    return ret;
}

esp_err_t solar_os_radio_send_stream(const char *name,
                                     const uint8_t *data,
                                     size_t len,
                                     uint32_t timeout_ms)
{
    if (!radio_name_valid(name) || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    solar_os_radio_features_t features = 0;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (radio_devices[index].claimed) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    features = radio_devices[index].info.features;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if ((features & SOLAR_OS_RADIO_FEATURE_CONTINUOUS_TX) == 0 ||
        ops == NULL || ops->send_stream == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ops->send_stream(ctx, data, len, timeout_ms);
}

esp_err_t solar_os_radio_receive(const char *name,
                                 solar_os_radio_packet_t *packet,
                                 uint32_t timeout_ms)
{
    if (!radio_name_valid(name) || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    size_t max_packet_len = 0;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    const int index = radio_find_index_locked(name);
    if (index < 0) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (radio_devices[index].claimed) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    max_packet_len = radio_devices[index].info.max_packet_len;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if (ops == NULL || ops->receive == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(packet, 0, sizeof(*packet));
    const esp_err_t ret = ops->receive(ctx, packet, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (packet->len > max_packet_len || packet->len > SOLAR_OS_RADIO_PACKET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t solar_os_radio_handle_receive(const solar_os_radio_handle_t *handle,
                                        solar_os_radio_packet_t *packet,
                                        uint32_t timeout_ms)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    const solar_os_radio_ops_t *ops = NULL;
    void *ctx = NULL;
    size_t max_packet_len = 0;
    int index = -1;
    uint32_t token = 0;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (!radio_handle_operation_acquire_locked(handle)) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    index = handle->index;
    token = handle->token;
    max_packet_len = radio_devices[index].info.max_packet_len;
    ops = radio_devices[index].ops;
    ctx = radio_devices[index].ctx;
    xSemaphoreGive(radio_mutex);

    if (ops == NULL || ops->receive == NULL) {
        xSemaphoreTake(radio_mutex, portMAX_DELAY);
        radio_handle_operation_release_locked(index, token);
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(packet, 0, sizeof(*packet));
    esp_err_t ret = ops->receive(ctx, packet, timeout_ms);
    if (ret == ESP_OK &&
        (packet->len > max_packet_len || packet->len > SOLAR_OS_RADIO_PACKET_MAX)) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    radio_handle_operation_release_locked(index, token);
    xSemaphoreGive(radio_mutex);
    return ret;
}

static bool radio_profile_name_valid(const char *name)
{
    if (name == NULL ||
        name[0] == '\0' ||
        strnlen(name, SOLAR_OS_RADIO_PROFILE_NAME_MAX) >=
            SOLAR_OS_RADIO_PROFILE_NAME_MAX ||
        !isalnum((unsigned char)name[0])) {
        return false;
    }

    for (const char *p = name + 1; *p != '\0'; p++) {
        if (!isalnum((unsigned char)*p) &&
            *p != '-' &&
            *p != '_' &&
            *p != '.') {
            return false;
        }
    }
    return true;
}

static esp_err_t radio_profile_ensure_init(void)
{
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");
    if (radio_profile_mutex != NULL) {
        return ESP_OK;
    }

    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (radio_profile_mutex == NULL) {
        radio_profile_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreGive(radio_mutex);
    return radio_profile_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static const solar_os_radio_profile_t *radio_builtin_profile_find(const char *name)
{
    for (size_t i = 0;
         i < sizeof(radio_builtin_profiles) / sizeof(radio_builtin_profiles[0]);
         i++) {
        if (strcmp(radio_builtin_profiles[i].name, name) == 0) {
            return &radio_builtin_profiles[i];
        }
    }
    return NULL;
}

static void radio_profile_store_empty(radio_profile_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = RADIO_PROFILE_MAGIC;
    store->version = RADIO_PROFILE_VERSION;
    store->size = sizeof(*store);
}

static esp_err_t radio_profile_store_load(radio_profile_store_t *store)
{
    if (store == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    radio_profile_store_empty(store);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(RADIO_PROFILE_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = sizeof(*store);
    ret = nvs_get_blob(nvs, RADIO_PROFILE_NVS_KEY, store, &len);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        radio_profile_store_empty(store);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (len != sizeof(*store) ||
        store->magic != RADIO_PROFILE_MAGIC ||
        store->version != RADIO_PROFILE_VERSION ||
        store->size != sizeof(*store)) {
        return ESP_ERR_INVALID_VERSION;
    }

    for (size_t i = 0; i < SOLAR_OS_RADIO_USER_PROFILE_MAX; i++) {
        const radio_user_profile_t *profile = &store->profiles[i];
        if (!profile->active) {
            continue;
        }
        if (!radio_profile_name_valid(profile->name) ||
            radio_builtin_profile_find(profile->name) != NULL ||
            radio_validate_config(
                &profile->config,
                SOLAR_OS_RADIO_MODULATION_FSK |
                    SOLAR_OS_RADIO_MODULATION_GFSK |
                    SOLAR_OS_RADIO_MODULATION_MSK |
                    SOLAR_OS_RADIO_MODULATION_GMSK |
                    SOLAR_OS_RADIO_MODULATION_OOK |
                    SOLAR_OS_RADIO_MODULATION_LORA) != ESP_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

static esp_err_t radio_profile_store_save(const radio_profile_store_t *store)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(RADIO_PROFILE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(nvs, RADIO_PROFILE_NVS_KEY, store, sizeof(*store));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t solar_os_radio_profile_list(solar_os_radio_profile_t *profiles,
                                      size_t capacity,
                                      size_t *count)
{
    if (count == NULL || (profiles == NULL && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    radio_profile_store_t store;
    ESP_RETURN_ON_ERROR(radio_profile_ensure_init(), "radio", "init failed");
    xSemaphoreTake(radio_profile_mutex, portMAX_DELAY);
    const esp_err_t load_ret = radio_profile_store_load(&store);
    xSemaphoreGive(radio_profile_mutex);
    ESP_RETURN_ON_ERROR(load_ret, "radio", "load profiles failed");

    size_t total = 0;
    for (size_t i = 0;
         i < sizeof(radio_builtin_profiles) / sizeof(radio_builtin_profiles[0]);
         i++) {
        if (total < capacity) {
            profiles[total] = radio_builtin_profiles[i];
        }
        total++;
    }
    for (size_t i = 0; i < SOLAR_OS_RADIO_USER_PROFILE_MAX; i++) {
        if (!store.profiles[i].active) {
            continue;
        }
        if (total < capacity) {
            memset(&profiles[total], 0, sizeof(profiles[total]));
            strlcpy(profiles[total].name,
                    store.profiles[i].name,
                    sizeof(profiles[total].name));
            profiles[total].config = store.profiles[i].config;
        }
        total++;
    }

    *count = total;
    return total <= capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_radio_profile_get(const char *profile_name,
                                     solar_os_radio_profile_t *profile)
{
    if (!radio_profile_name_valid(profile_name) || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_radio_profile_t *builtin =
        radio_builtin_profile_find(profile_name);
    if (builtin != NULL) {
        *profile = *builtin;
        return ESP_OK;
    }

    radio_profile_store_t store;
    ESP_RETURN_ON_ERROR(radio_profile_ensure_init(), "radio", "init failed");
    xSemaphoreTake(radio_profile_mutex, portMAX_DELAY);
    const esp_err_t load_ret = radio_profile_store_load(&store);
    xSemaphoreGive(radio_profile_mutex);
    ESP_RETURN_ON_ERROR(load_ret, "radio", "load profiles failed");
    for (size_t i = 0; i < SOLAR_OS_RADIO_USER_PROFILE_MAX; i++) {
        if (store.profiles[i].active &&
            strcmp(store.profiles[i].name, profile_name) == 0) {
            memset(profile, 0, sizeof(*profile));
            strlcpy(profile->name, store.profiles[i].name, sizeof(profile->name));
            profile->config = store.profiles[i].config;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_radio_profile_apply(const char *radio_name,
                                       const char *profile_name)
{
    solar_os_radio_profile_t profile;
    ESP_RETURN_ON_ERROR(solar_os_radio_profile_get(profile_name, &profile),
                        "radio",
                        "profile not found");

    solar_os_radio_status_t previous;
    ESP_RETURN_ON_ERROR(solar_os_radio_get_status(radio_name, &previous),
                        "radio",
                        "radio not found");

    const esp_err_t ret = solar_os_radio_configure(radio_name, &profile.config);
    if (ret != ESP_OK) {
        (void)solar_os_radio_configure(radio_name, &previous.config);
        if (previous.state != SOLAR_OS_RADIO_STATE_UNKNOWN &&
            previous.state != SOLAR_OS_RADIO_STATE_STANDBY) {
            (void)solar_os_radio_set_state(radio_name, previous.state);
        }
    }
    return ret;
}

esp_err_t solar_os_radio_handle_profile_apply(
    const solar_os_radio_handle_t *handle,
    const char *profile_name)
{
    solar_os_radio_profile_t profile;
    ESP_RETURN_ON_ERROR(solar_os_radio_profile_get(profile_name, &profile),
                        "radio",
                        "profile not found");

    solar_os_radio_status_t previous;
    ESP_RETURN_ON_ERROR(solar_os_radio_handle_get_status(handle, &previous),
                        "radio",
                        "invalid radio handle");

    const esp_err_t ret =
        solar_os_radio_handle_configure(handle, &profile.config);
    if (ret != ESP_OK) {
        (void)solar_os_radio_handle_configure(handle, &previous.config);
        if (previous.state != SOLAR_OS_RADIO_STATE_UNKNOWN &&
            previous.state != SOLAR_OS_RADIO_STATE_STANDBY) {
            (void)solar_os_radio_handle_set_state(handle, previous.state);
        }
    }
    return ret;
}

esp_err_t solar_os_radio_profile_save(const char *radio_name,
                                      const char *profile_name)
{
    if (!radio_profile_name_valid(profile_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (radio_builtin_profile_find(profile_name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_radio_status_t status;
    ESP_RETURN_ON_ERROR(solar_os_radio_get_status(radio_name, &status),
                        "radio",
                        "radio not found");

    ESP_RETURN_ON_ERROR(radio_profile_ensure_init(), "radio", "init failed");

    radio_profile_store_t store;
    xSemaphoreTake(radio_profile_mutex, portMAX_DELAY);
    esp_err_t ret = radio_profile_store_load(&store);
    if (ret != ESP_OK) {
        xSemaphoreGive(radio_profile_mutex);
        return ret;
    }
    radio_user_profile_t *target = NULL;
    for (size_t i = 0; i < SOLAR_OS_RADIO_USER_PROFILE_MAX; i++) {
        if (store.profiles[i].active &&
            strcmp(store.profiles[i].name, profile_name) == 0) {
            target = &store.profiles[i];
            break;
        }
        if (!store.profiles[i].active && target == NULL) {
            target = &store.profiles[i];
        }
    }
    if (target == NULL) {
        xSemaphoreGive(radio_profile_mutex);
        return ESP_ERR_NO_MEM;
    }

    memset(target, 0, sizeof(*target));
    target->active = true;
    strlcpy(target->name, profile_name, sizeof(target->name));
    target->config = status.config;
    ret = radio_profile_store_save(&store);
    xSemaphoreGive(radio_profile_mutex);
    return ret;
}

esp_err_t solar_os_radio_profile_remove(const char *profile_name)
{
    if (!radio_profile_name_valid(profile_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (radio_builtin_profile_find(profile_name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(radio_profile_ensure_init(), "radio", "init failed");

    radio_profile_store_t store;
    xSemaphoreTake(radio_profile_mutex, portMAX_DELAY);
    esp_err_t ret = radio_profile_store_load(&store);
    if (ret != ESP_OK) {
        xSemaphoreGive(radio_profile_mutex);
        return ret;
    }
    for (size_t i = 0; i < SOLAR_OS_RADIO_USER_PROFILE_MAX; i++) {
        if (store.profiles[i].active &&
            strcmp(store.profiles[i].name, profile_name) == 0) {
            memset(&store.profiles[i], 0, sizeof(store.profiles[i]));
            ret = radio_profile_store_save(&store);
            xSemaphoreGive(radio_profile_mutex);
            return ret;
        }
    }
    xSemaphoreGive(radio_profile_mutex);
    return ESP_ERR_NOT_FOUND;
}

const char *solar_os_radio_modulation_name(solar_os_radio_modulation_t modulation)
{
    switch (modulation) {
    case SOLAR_OS_RADIO_MODULATION_FSK:
        return "fsk";
    case SOLAR_OS_RADIO_MODULATION_GFSK:
        return "gfsk";
    case SOLAR_OS_RADIO_MODULATION_MSK:
        return "msk";
    case SOLAR_OS_RADIO_MODULATION_GMSK:
        return "gmsk";
    case SOLAR_OS_RADIO_MODULATION_OOK:
        return "ook";
    case SOLAR_OS_RADIO_MODULATION_LORA:
        return "lora";
    case SOLAR_OS_RADIO_MODULATION_NONE:
    default:
        return "none";
    }
}

solar_os_radio_modulation_t solar_os_radio_modulation_from_name(const char *name)
{
    if (name == NULL) {
        return SOLAR_OS_RADIO_MODULATION_NONE;
    }
    if (strcmp(name, "fsk") == 0) {
        return SOLAR_OS_RADIO_MODULATION_FSK;
    }
    if (strcmp(name, "gfsk") == 0) {
        return SOLAR_OS_RADIO_MODULATION_GFSK;
    }
    if (strcmp(name, "msk") == 0) {
        return SOLAR_OS_RADIO_MODULATION_MSK;
    }
    if (strcmp(name, "gmsk") == 0) {
        return SOLAR_OS_RADIO_MODULATION_GMSK;
    }
    if (strcmp(name, "ook") == 0) {
        return SOLAR_OS_RADIO_MODULATION_OOK;
    }
    if (strcmp(name, "lora") == 0) {
        return SOLAR_OS_RADIO_MODULATION_LORA;
    }
    return SOLAR_OS_RADIO_MODULATION_NONE;
}

const char *solar_os_radio_feature_name(solar_os_radio_feature_t feature)
{
    switch (feature) {
    case SOLAR_OS_RADIO_FEATURE_PACKET:
        return "packet";
    case SOLAR_OS_RADIO_FEATURE_RSSI:
        return "rssi";
    case SOLAR_OS_RADIO_FEATURE_SNR:
        return "snr";
    case SOLAR_OS_RADIO_FEATURE_TX_POWER:
        return "tx_power";
    case SOLAR_OS_RADIO_FEATURE_CRC:
        return "crc";
    case SOLAR_OS_RADIO_FEATURE_SYNC_WORD:
        return "sync_word";
    case SOLAR_OS_RADIO_FEATURE_PREAMBLE:
        return "preamble";
    case SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH:
        return "variable_length";
    case SOLAR_OS_RADIO_FEATURE_ADDRESSING:
        return "addressing";
    case SOLAR_OS_RADIO_FEATURE_AES:
        return "aes";
    case SOLAR_OS_RADIO_FEATURE_PROMISCUOUS:
        return "promiscuous";
    case SOLAR_OS_RADIO_FEATURE_CONTINUOUS_RX:
        return "continuous_rx";
    case SOLAR_OS_RADIO_FEATURE_CONTINUOUS_TX:
        return "continuous_tx";
    default:
        return "unknown";
    }
}

const char *solar_os_radio_state_name(solar_os_radio_state_t state)
{
    switch (state) {
    case SOLAR_OS_RADIO_STATE_SLEEP:
        return "sleep";
    case SOLAR_OS_RADIO_STATE_STANDBY:
        return "standby";
    case SOLAR_OS_RADIO_STATE_RX:
        return "rx";
    case SOLAR_OS_RADIO_STATE_TX:
        return "tx";
    case SOLAR_OS_RADIO_STATE_UNKNOWN:
    default:
        return "unknown";
    }
}

solar_os_radio_state_t solar_os_radio_state_from_name(const char *name)
{
    if (name == NULL) {
        return SOLAR_OS_RADIO_STATE_UNKNOWN;
    }
    if (strcmp(name, "sleep") == 0) {
        return SOLAR_OS_RADIO_STATE_SLEEP;
    }
    if (strcmp(name, "standby") == 0) {
        return SOLAR_OS_RADIO_STATE_STANDBY;
    }
    if (strcmp(name, "rx") == 0) {
        return SOLAR_OS_RADIO_STATE_RX;
    }
    if (strcmp(name, "tx") == 0) {
        return SOLAR_OS_RADIO_STATE_TX;
    }
    return SOLAR_OS_RADIO_STATE_UNKNOWN;
}

void solar_os_radio_modulations_format(solar_os_radio_modulations_t modulations,
                                       char *buffer,
                                       size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    buffer[0] = '\0';

    size_t used = 0;
    bool any = false;
    const solar_os_radio_modulation_t values[] = {
        SOLAR_OS_RADIO_MODULATION_FSK,
        SOLAR_OS_RADIO_MODULATION_GFSK,
        SOLAR_OS_RADIO_MODULATION_MSK,
        SOLAR_OS_RADIO_MODULATION_GMSK,
        SOLAR_OS_RADIO_MODULATION_OOK,
        SOLAR_OS_RADIO_MODULATION_LORA,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if ((modulations & (solar_os_radio_modulations_t)values[i]) != 0) {
            format_append(buffer, buffer_len, &used, &any, solar_os_radio_modulation_name(values[i]));
        }
    }
    if (!any) {
        strlcpy(buffer, "none", buffer_len);
    }
}

void solar_os_radio_features_format(solar_os_radio_features_t features,
                                    char *buffer,
                                    size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    buffer[0] = '\0';

    size_t used = 0;
    bool any = false;
    const solar_os_radio_feature_t values[] = {
        SOLAR_OS_RADIO_FEATURE_PACKET,
        SOLAR_OS_RADIO_FEATURE_RSSI,
        SOLAR_OS_RADIO_FEATURE_SNR,
        SOLAR_OS_RADIO_FEATURE_TX_POWER,
        SOLAR_OS_RADIO_FEATURE_CRC,
        SOLAR_OS_RADIO_FEATURE_SYNC_WORD,
        SOLAR_OS_RADIO_FEATURE_PREAMBLE,
        SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH,
        SOLAR_OS_RADIO_FEATURE_ADDRESSING,
        SOLAR_OS_RADIO_FEATURE_AES,
        SOLAR_OS_RADIO_FEATURE_PROMISCUOUS,
        SOLAR_OS_RADIO_FEATURE_CONTINUOUS_RX,
        SOLAR_OS_RADIO_FEATURE_CONTINUOUS_TX,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if ((features & (solar_os_radio_features_t)values[i]) != 0) {
            format_append(buffer, buffer_len, &used, &any, solar_os_radio_feature_name(values[i]));
        }
    }
    if (!any) {
        strlcpy(buffer, "none", buffer_len);
    }
}
