#include "solar_os_radio.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SOLAR_OS_RADIO_DEVICE_MAX 4

typedef struct {
    bool active;
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    const solar_os_radio_ops_t *ops;
    void *ctx;
} radio_device_t;

static radio_device_t radio_devices[SOLAR_OS_RADIO_DEVICE_MAX];
static SemaphoreHandle_t radio_mutex;

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
            *info = radio_devices[i].info;
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
    *info = radio_devices[index].info;
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
