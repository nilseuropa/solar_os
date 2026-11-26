#include "solar_os_radio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_saved_config.h"

#define SOLAR_OS_RADIO_DEVICE_MAX 4
#define RADIO_RELEASE_WAIT_MS 3500U
#define RADIO_PROFILE_NVS_NAMESPACE "radio_prof"
#define RADIO_PROFILE_NVS_KEY "profiles"
#define RADIO_PROFILE_MAGIC 0x52504631U
#define RADIO_PROFILE_VERSION 3U

/* Version 1 stored this struct verbatim with two tail-padding bytes, and
 * version 2 with one; both read as the driver's low-data-rate choice. */
_Static_assert(sizeof(solar_os_radio_config_t) == 44 &&
                   offsetof(solar_os_radio_config_t, rx_gain) == 42 &&
                   offsetof(solar_os_radio_config_t, low_data_rate) == 43,
               "radio profile v1 layout must remain readable");

typedef struct {
    bool active;
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    const solar_os_radio_ops_t *ops;
    void *ctx;
    bool claimed;
    bool releasing;
    size_t handle_refs;
    size_t named_refs;
    bool unclaimed_write;
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
        /* MeshCore "USA/Canada (Recommended)" preset: 910.525 MHz, SF7,
         * BW 62.5 kHz, CR 4/5 (docs/faq.md, October 2025 "narrow" settings). */
        .name = "meshcore-us915",
        .builtin = true,
        .config = {
            .frequency_hz = 910525000,
            .modulation = SOLAR_OS_RADIO_MODULATION_LORA,
            .rx_bandwidth_hz = 62500,
            .spreading_factor = 7,
            .coding_rate_denominator = 5,
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

static EXT_RAM_BSS_ATTR radio_device_t radio_devices[SOLAR_OS_RADIO_DEVICE_MAX];
static SemaphoreHandle_t radio_mutex;
static SemaphoreHandle_t radio_profile_mutex;
static StaticSemaphore_t radio_mutex_storage;
static StaticSemaphore_t radio_profile_mutex_storage;
static portMUX_TYPE radio_init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t radio_next_token = 1;

static esp_err_t radio_ensure_init(void)
{
    portENTER_CRITICAL(&radio_init_lock);
    if (!radio_mutex) radio_mutex = xSemaphoreCreateMutexStatic(&radio_mutex_storage);
    const bool ready = radio_mutex != NULL;
    portEXIT_CRITICAL(&radio_init_lock);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
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

/* Named observers can coexist with a claim. Pin their registration
 * independently so releasing a claimant cannot retire an active callback. */
typedef struct {
    radio_device_t *device;
    bool claimed;
    bool write;
} radio_operation_t;

static esp_err_t radio_operation_begin(const char *name,
                                       const solar_os_radio_handle_t *handle,
                                       bool write, radio_operation_t *operation)
{
    if (!handle && !radio_name_valid(name)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(radio_ensure_init(), "radio", "init failed");

    esp_err_t error = ESP_OK;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    radio_device_t *device = NULL;
    if (handle) {
        if (radio_handle_valid_locked(handle) && !radio_devices[handle->index].releasing) {
            device = &radio_devices[handle->index];
            device->handle_refs++;
        } else {
            error = ESP_ERR_INVALID_ARG;
        }
    } else {
        const int index = radio_find_index_locked(name);
        if (index < 0) {
            error = ESP_ERR_NOT_FOUND;
        } else if (write && (radio_devices[index].claimed || radio_devices[index].unclaimed_write)) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            device = &radio_devices[index];
            device->named_refs++;
            if (write) device->unclaimed_write = true;
        }
    }
    *operation = (radio_operation_t){.device = device, .claimed = handle != NULL, .write = write};
    xSemaphoreGive(radio_mutex);
    return error;
}

static void radio_operation_release_locked(const radio_operation_t *operation)
{
    radio_device_t *device = operation->device;
    if (operation->claimed) {
        device->handle_refs--;
    } else {
        device->named_refs--;
        if (operation->write) device->unclaimed_write = false;
    }
}

static void radio_operation_end(const radio_operation_t *operation)
{
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    radio_operation_release_locked(operation);
    xSemaphoreGive(radio_mutex);
}

static void radio_fill_info_locked(const radio_device_t *device,
                                   solar_os_radio_info_t *info)
{
    *info = device->info;
    info->claimed = device->claimed;
    if (device->claimed || device->unclaimed_write) {
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
    if (config->rx_gain >= SOLAR_OS_RADIO_RX_GAIN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->low_data_rate > SOLAR_OS_RADIO_LOW_DATA_RATE_ON) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t radio_validate_rx_gain(const solar_os_radio_config_t *config, uint32_t modes)
{
    if (config->rx_gain >= SOLAR_OS_RADIO_RX_GAIN_COUNT) return ESP_ERR_INVALID_ARG;
    return config->rx_gain == SOLAR_OS_RADIO_RX_GAIN_DEFAULT ||
        (modes & SOLAR_OS_RADIO_RX_GAIN_BIT(config->rx_gain)) != 0
        ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
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

static const char *const radio_rx_gain_names[] = {
    "default", "power-saving", "boosted", "auto", "g1", "g2", "g3", "g4", "g5", "g6",
};

const char *solar_os_radio_rx_gain_name(solar_os_radio_rx_gain_t gain)
{
    return (unsigned)gain < SOLAR_OS_RADIO_RX_GAIN_COUNT ? radio_rx_gain_names[gain] : "unknown";
}

bool solar_os_radio_rx_gain_parse(const char *name, uint8_t *gain)
{
    if (name == NULL || gain == NULL) return false;
    for (unsigned i = 0; i < SOLAR_OS_RADIO_RX_GAIN_COUNT; i++) {
        if (strcmp(name, radio_rx_gain_names[i]) == 0) {
            *gain = (uint8_t)i;
            return true;
        }
    }
    return false;
}

void solar_os_radio_rx_gains_format(uint32_t modes, char *buffer, size_t length)
{
    if (buffer == NULL || length == 0) return;
    buffer[0] = '\0';
    size_t used = 0;
    bool any = false;
    for (unsigned i = 0; i < SOLAR_OS_RADIO_RX_GAIN_COUNT; i++) {
        if ((modes & SOLAR_OS_RADIO_RX_GAIN_BIT(i)) != 0) {
            format_append(buffer, length, &used, &any, radio_rx_gain_names[i]);
        }
    }
    if (!any) strlcpy(buffer, "none", length);
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
    ESP_RETURN_ON_ERROR(radio_validate_rx_gain(&registration->default_config,
                                               registration->rx_gain_modes),
                        "radio", "invalid default RX gain");
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
    device->info.rx_gain_modes = registration->rx_gain_modes;
    device->info.max_packet_len = registration->max_packet_len;
    device->status.state = registration->initial_state;
    device->status.config = registration->default_config;
    device->ops = registration->ops;
    device->ctx = registration->ctx;

    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

static esp_err_t radio_unregister(const char *name, const void *ctx, bool require_ctx)
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
    if (require_ctx && radio_devices[index].ctx != ctx) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (radio_devices[index].claimed || radio_devices[index].named_refs) {
        xSemaphoreGive(radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&radio_devices[index], 0, sizeof(radio_devices[index]));
    xSemaphoreGive(radio_mutex);
    return ESP_OK;
}

esp_err_t solar_os_radio_unregister(const char *name)
{
    return radio_unregister(name, NULL, false);
}

esp_err_t solar_os_radio_unregister_owned(const char *name, const void *ctx)
{
    return radio_unregister(name, ctx, true);
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

static esp_err_t radio_get_status(const char *name, const solar_os_radio_handle_t *handle,
                                  solar_os_radio_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    const esp_err_t error = radio_operation_begin(name, handle, false, &operation);
    if (error != ESP_OK) return error;

    radio_device_t *device = operation.device;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    solar_os_radio_status_t current = device->status;
    xSemaphoreGive(radio_mutex);

    esp_err_t result = ESP_OK;
    if (device->ops && device->ops->get_status) {
        result = device->ops->get_status(device->ctx, &current);
    }
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (result == ESP_OK && device->ops && device->ops->get_status) device->status = current;
    radio_operation_release_locked(&operation);
    xSemaphoreGive(radio_mutex);
    if (result == ESP_OK) *status = current;
    return result;
}

esp_err_t solar_os_radio_get_status(const char *name, solar_os_radio_status_t *status)
{
    return radio_get_status(name, NULL, status);
}

esp_err_t solar_os_radio_handle_get_status(const solar_os_radio_handle_t *handle,
                                           solar_os_radio_status_t *status)
{
    return radio_get_status(NULL, handle, status);
}

esp_err_t solar_os_radio_read_rssi(const char *name, int16_t *dbm)
{
    if (!dbm) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    const esp_err_t error = radio_operation_begin(name, NULL, false, &operation);
    if (error != ESP_OK) return error;

    const radio_device_t *device = operation.device;
    const esp_err_t result =
        (device->info.features & SOLAR_OS_RADIO_FEATURE_LIVE_RSSI) && device->ops && device->ops->read_rssi ?
        device->ops->read_rssi(device->ctx, dbm) : ESP_ERR_NOT_SUPPORTED;
    radio_operation_end(&operation);
    return result;
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
    if (device->claimed || device->unclaimed_write) {
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

esp_err_t solar_os_radio_handle_tune_rx(const solar_os_radio_handle_t *handle,
                                       uint32_t frequency_hz, uint32_t *settle_ms)
{
    if (!frequency_hz || !settle_ms) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    const esp_err_t error = radio_operation_begin(NULL, handle, true, &operation);
    if (error != ESP_OK) return error;

    radio_device_t *device = operation.device;
    const esp_err_t result =
        (device->info.features & SOLAR_OS_RADIO_FEATURE_FAST_TUNE) && device->ops && device->ops->tune_rx ?
        device->ops->tune_rx(device->ctx, frequency_hz, settle_ms) : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (result == ESP_OK) {
        device->status.config.frequency_hz = frequency_hz;
        device->status.state = SOLAR_OS_RADIO_STATE_RX;
    }
    radio_operation_release_locked(&operation);
    xSemaphoreGive(radio_mutex);
    return result;
}

static esp_err_t radio_configure(const char *name, const solar_os_radio_handle_t *handle,
                                 const solar_os_radio_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    esp_err_t result = radio_operation_begin(name, handle, true, &operation);
    if (result != ESP_OK) return result;

    radio_device_t *device = operation.device;
    result = radio_validate_config(config, device->info.modulations);
    if (result == ESP_OK) result = radio_validate_rx_gain(config, device->info.rx_gain_modes);
    if (result == ESP_OK) {
        result = device->ops && device->ops->configure ?
            device->ops->configure(device->ctx, config) : ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (result == ESP_OK) device->status.config = *config;
    radio_operation_release_locked(&operation);
    xSemaphoreGive(radio_mutex);
    return result;
}

esp_err_t solar_os_radio_configure(const char *name, const solar_os_radio_config_t *config)
{
    return radio_configure(name, NULL, config);
}

esp_err_t solar_os_radio_handle_configure(const solar_os_radio_handle_t *handle,
                                          const solar_os_radio_config_t *config)
{
    return radio_configure(NULL, handle, config);
}

static esp_err_t radio_set_state(const char *name, const solar_os_radio_handle_t *handle,
                                 solar_os_radio_state_t state)
{
    if (state <= SOLAR_OS_RADIO_STATE_UNKNOWN || state > SOLAR_OS_RADIO_STATE_TX) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    const esp_err_t error = radio_operation_begin(name, handle, true, &operation);
    if (error != ESP_OK) return error;

    radio_device_t *device = operation.device;
    const esp_err_t result = device->ops && device->ops->set_state ?
        device->ops->set_state(device->ctx, state) : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (result == ESP_OK) device->status.state = state;
    radio_operation_release_locked(&operation);
    xSemaphoreGive(radio_mutex);
    return result;
}

esp_err_t solar_os_radio_set_state(const char *name, solar_os_radio_state_t state)
{
    return radio_set_state(name, NULL, state);
}

esp_err_t solar_os_radio_handle_set_state(const solar_os_radio_handle_t *handle,
                                          solar_os_radio_state_t state)
{
    return radio_set_state(NULL, handle, state);
}

static esp_err_t radio_send(const char *name, const solar_os_radio_handle_t *handle,
                            const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    if (!packet || !packet->len || packet->len > SOLAR_OS_RADIO_PACKET_MAX) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    esp_err_t result = radio_operation_begin(name, handle, true, &operation);
    if (result != ESP_OK) return result;

    const radio_device_t *device = operation.device;
    if (packet->len > device->info.max_packet_len) {
        result = ESP_ERR_INVALID_SIZE;
    } else {
        result = device->ops && device->ops->send ?
            device->ops->send(device->ctx, packet, timeout_ms) : ESP_ERR_NOT_SUPPORTED;
    }
    radio_operation_end(&operation);
    return result;
}

esp_err_t solar_os_radio_send(const char *name, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return radio_send(name, NULL, packet, timeout_ms);
}

esp_err_t solar_os_radio_handle_send(const solar_os_radio_handle_t *handle,
                                     const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return radio_send(NULL, handle, packet, timeout_ms);
}

static esp_err_t radio_send_stream(const char *name, const solar_os_radio_handle_t *handle,
                                   const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    const esp_err_t error = radio_operation_begin(name, handle, true, &operation);
    if (error != ESP_OK) return error;

    const radio_device_t *device = operation.device;
    const esp_err_t result =
        (device->info.features & SOLAR_OS_RADIO_FEATURE_CONTINUOUS_TX) && device->ops && device->ops->send_stream ?
        device->ops->send_stream(device->ctx, data, len, timeout_ms) : ESP_ERR_NOT_SUPPORTED;
    radio_operation_end(&operation);
    return result;
}

esp_err_t solar_os_radio_send_stream(const char *name, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    return radio_send_stream(name, NULL, data, len, timeout_ms);
}

esp_err_t solar_os_radio_handle_send_stream(const solar_os_radio_handle_t *handle,
                                            const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    return radio_send_stream(NULL, handle, data, len, timeout_ms);
}

static esp_err_t radio_receive(const char *name, const solar_os_radio_handle_t *handle,
                               solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    if (!packet) return ESP_ERR_INVALID_ARG;
    radio_operation_t operation;
    esp_err_t result = radio_operation_begin(name, handle, true, &operation);
    if (result != ESP_OK) return result;

    const radio_device_t *device = operation.device;
    if (device->ops && device->ops->receive) {
        memset(packet, 0, sizeof(*packet));
        result = device->ops->receive(device->ctx, packet, timeout_ms);
        if (result == ESP_OK && packet->len > device->info.max_packet_len) result = ESP_ERR_INVALID_SIZE;
    } else {
        result = ESP_ERR_NOT_SUPPORTED;
    }
    radio_operation_end(&operation);
    return result;
}

esp_err_t solar_os_radio_receive(const char *name, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return radio_receive(name, NULL, packet, timeout_ms);
}

esp_err_t solar_os_radio_handle_receive(const solar_os_radio_handle_t *handle,
                                        solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return radio_receive(NULL, handle, packet, timeout_ms);
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
    xSemaphoreTake(radio_mutex, portMAX_DELAY);
    if (!radio_profile_mutex) radio_profile_mutex = xSemaphoreCreateMutexStatic(&radio_profile_mutex_storage);
    const bool ready = radio_profile_mutex != NULL;
    xSemaphoreGive(radio_mutex);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
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

static solar_os_saved_config_t radio_profile_file;

static bool radio_profile_store_valid(const void *data, size_t length)
{
    const radio_profile_store_t *store = data;
    if (length != sizeof(*store) ||
        store->magic != RADIO_PROFILE_MAGIC ||
        (store->version < 1U || store->version > RADIO_PROFILE_VERSION) ||
        store->size != sizeof(*store)) {
        return false;
    }

    for (size_t i = 0; i < SOLAR_OS_RADIO_USER_PROFILE_MAX; i++) {
        const radio_user_profile_t *profile = &store->profiles[i];
        if (!profile->active) {
            continue;
        }
        solar_os_radio_config_t config = profile->config;
        if (store->version == 1U) config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_DEFAULT;
        if (store->version < 3U) config.low_data_rate = SOLAR_OS_RADIO_LOW_DATA_RATE_AUTO;
        if (!radio_profile_name_valid(profile->name) ||
            radio_builtin_profile_find(profile->name) != NULL ||
            radio_validate_config(
                &config,
                SOLAR_OS_RADIO_MODULATION_FSK |
                    SOLAR_OS_RADIO_MODULATION_GFSK |
                    SOLAR_OS_RADIO_MODULATION_MSK |
                    SOLAR_OS_RADIO_MODULATION_GMSK |
                    SOLAR_OS_RADIO_MODULATION_OOK |
                    SOLAR_OS_RADIO_MODULATION_LORA) != ESP_OK) {
            return false;
        }
    }
    return true;
}

static esp_err_t radio_profile_store_load(radio_profile_store_t *store)
{
    if (!store) return ESP_ERR_INVALID_ARG;
    radio_profile_store_empty(store);
    size_t length = sizeof(*store);
    esp_err_t error = solar_os_saved_config_load(&radio_profile_file, "radio-profiles",
        RADIO_PROFILE_NVS_NAMESPACE, RADIO_PROFILE_NVS_KEY, store, sizeof(*store), &length, radio_profile_store_valid);
    if (error == ESP_ERR_NVS_NOT_FOUND || error == ESP_ERR_NOT_FOUND) {
        radio_profile_store_empty(store); return ESP_OK;
    }
    if (error == ESP_OK && store->version < RADIO_PROFILE_VERSION) {
        for (size_t i = 0; i < SOLAR_OS_RADIO_USER_PROFILE_MAX; i++) {
            solar_os_radio_config_t *config = &store->profiles[i].config;
            if (store->version == 1U) config->rx_gain = SOLAR_OS_RADIO_RX_GAIN_DEFAULT;
            config->low_data_rate = SOLAR_OS_RADIO_LOW_DATA_RATE_AUTO;
        }
        store->version = RADIO_PROFILE_VERSION;
    }
    return error;
}

static esp_err_t radio_profile_store_save(const radio_profile_store_t *store)
{
    return solar_os_saved_config_save(&radio_profile_file, "radio-profiles", store, sizeof(*store));
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
    case SOLAR_OS_RADIO_FEATURE_LIVE_RSSI:
        return "live_rssi";
    case SOLAR_OS_RADIO_FEATURE_FAST_TUNE:
        return "fast_tune";
    case SOLAR_OS_RADIO_FEATURE_SNR:
        return "snr";
    case SOLAR_OS_RADIO_FEATURE_TX_POWER:
        return "tx_power";
    case SOLAR_OS_RADIO_FEATURE_RX_GAIN:
        return "rx_gain";
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
        SOLAR_OS_RADIO_FEATURE_LIVE_RSSI,
        SOLAR_OS_RADIO_FEATURE_FAST_TUNE,
        SOLAR_OS_RADIO_FEATURE_SNR,
        SOLAR_OS_RADIO_FEATURE_TX_POWER,
        SOLAR_OS_RADIO_FEATURE_RX_GAIN,
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
