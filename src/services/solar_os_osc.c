#include "solar_os_osc.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_parameters.h"

#define OSC_PARAMETER_PREFIX "/solaros/parameter/"
#define OSC_PARAMETER_NORMALIZED_SUFFIX "/normalized"

typedef struct {
    bool active;
    solar_os_osc_binding_info_t info;
    float pending_value;
} osc_binding_slot_t;

static EXT_RAM_BSS_ATTR osc_binding_slot_t osc_bindings[SOLAR_OS_OSC_BINDING_MAX];
static SemaphoreHandle_t osc_mutex;
static StaticSemaphore_t osc_mutex_storage;
static uint32_t osc_next_id;

static esp_err_t osc_ensure_mutex(void)
{
    if (osc_mutex == NULL) {
        osc_mutex = xSemaphoreCreateMutexStatic(&osc_mutex_storage);
    }
    return osc_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool osc_name_valid(const char *name, size_t capacity)
{
    if (name == NULL || name[0] == '\0' || strnlen(name, capacity) >= capacity) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_')) {
            return false;
        }
    }
    return true;
}

static bool osc_source_valid(const char *source, size_t capacity)
{
    return source != NULL && source[0] != '\0' &&
           strnlen(source, capacity) < capacity;
}

static bool osc_address_valid(const char *address)
{
    if (address == NULL || address[0] != '/' || address[1] == '\0' ||
        strnlen(address, SOLAR_OS_OSC_ADDRESS_MAX) >= SOLAR_OS_OSC_ADDRESS_MAX) {
        return false;
    }
    bool component = false;
    for (const unsigned char *p = (const unsigned char *)address + 1U;
         *p != '\0'; p++) {
        if (*p == '/') {
            if (!component) {
                return false;
            }
            component = false;
        } else if (*p <= 0x20U || *p >= 0x7fU || strchr("#*,?[]{}", *p) != NULL) {
            return false;
        } else {
            component = true;
        }
    }
    return component;
}

static int osc_binding_find_id_locked(uint32_t id)
{
    for (size_t i = 0; i < SOLAR_OS_OSC_BINDING_MAX; i++) {
        if (osc_bindings[i].active && osc_bindings[i].info.id == id) {
            return (int)i;
        }
    }
    return -1;
}

static int osc_binding_find_name_locked(const char *name)
{
    for (size_t i = 0; i < SOLAR_OS_OSC_BINDING_MAX; i++) {
        if (osc_bindings[i].active &&
            strcmp(osc_bindings[i].info.config.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t solar_os_osc_bind(const solar_os_osc_binding_config_t *config,
                            uint32_t *binding_id)
{
    if (config == NULL ||
        !osc_name_valid(config->name, sizeof(config->name)) ||
        !osc_source_valid(config->source, sizeof(config->source)) ||
        !osc_address_valid(config->address) ||
        config->source_type > SOLAR_OS_OSC_SOURCE_CONTROL ||
        config->value_type > SOLAR_OS_OSC_VALUE_EVENT ||
        config->edge > SOLAR_OS_OSC_EDGE_BOTH || config->interval_ms == 0U ||
        !isfinite(config->delta) || config->delta < 0.0f ||
        (config->source_type == SOLAR_OS_OSC_SOURCE_CONTROL &&
         config->value_type != SOLAR_OS_OSC_VALUE_SCALAR) ||
        osc_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    if (osc_binding_find_name_locked(config->name) >= 0) {
        xSemaphoreGive(osc_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < SOLAR_OS_OSC_BINDING_MAX; i++) {
        if (osc_bindings[i].active) {
            continue;
        }
        memset(&osc_bindings[i], 0, sizeof(osc_bindings[i]));
        osc_next_id++;
        if (osc_next_id == 0U) {
            osc_next_id++;
        }
        osc_bindings[i].active = true;
        osc_bindings[i].info.id = osc_next_id;
        osc_bindings[i].info.config = *config;
        osc_bindings[i].info.last_error = ESP_OK;
        if (binding_id != NULL) {
            *binding_id = osc_next_id;
        }
        xSemaphoreGive(osc_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(osc_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_osc_unbind(const char *name)
{
    if (!osc_name_valid(name, SOLAR_OS_OSC_BINDING_NAME_MAX) ||
        osc_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    const int index = osc_binding_find_name_locked(name);
    if (index >= 0) {
        memset(&osc_bindings[index], 0, sizeof(osc_bindings[index]));
    }
    xSemaphoreGive(osc_mutex);
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void solar_os_osc_clear(void)
{
    if (osc_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    memset(osc_bindings, 0, sizeof(osc_bindings));
    xSemaphoreGive(osc_mutex);
}

size_t solar_os_osc_binding_count(void)
{
    if (osc_ensure_mutex() != ESP_OK) {
        return 0U;
    }
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    size_t count = 0U;
    for (size_t i = 0; i < SOLAR_OS_OSC_BINDING_MAX; i++) {
        count += osc_bindings[i].active ? 1U : 0U;
    }
    xSemaphoreGive(osc_mutex);
    return count;
}

bool solar_os_osc_binding_get(size_t index, solar_os_osc_binding_info_t *info)
{
    if (info == NULL || osc_ensure_mutex() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    size_t seen = 0U;
    for (size_t i = 0; i < SOLAR_OS_OSC_BINDING_MAX; i++) {
        if (!osc_bindings[i].active) {
            continue;
        }
        if (seen++ == index) {
            *info = osc_bindings[i].info;
            xSemaphoreGive(osc_mutex);
            return true;
        }
    }
    xSemaphoreGive(osc_mutex);
    return false;
}

bool solar_os_osc_binding_due(const solar_os_osc_binding_info_t *info,
                              uint64_t now_ms)
{
    return info != NULL &&
        (info->last_sample_ms == 0U ||
         now_ms - info->last_sample_ms >= info->config.interval_ms);
}

esp_err_t solar_os_osc_binding_prepare(uint32_t id,
                                       uint64_t now_ms,
                                       float value,
                                       bool *send,
                                       bool *send_integer)
{
    if (id == 0U || !isfinite(value) || send == NULL || send_integer == NULL ||
        osc_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    *send = false;
    *send_integer = false;
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    const int index = osc_binding_find_id_locked(id);
    if (index < 0) {
        xSemaphoreGive(osc_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    osc_binding_slot_t *slot = &osc_bindings[index];
    solar_os_osc_binding_info_t *info = &slot->info;
    const bool had_value = info->has_value;
    const float previous = info->last_value;
    info->source_available = true;
    info->has_value = true;
    info->last_value = value;
    info->last_sample_ms = now_ms;
    info->last_error = ESP_OK;

    if (info->config.value_type == SOLAR_OS_OSC_VALUE_EVENT) {
        const bool old_level = previous != 0.0f;
        const bool new_level = value != 0.0f;
        const bool rising = had_value && !old_level && new_level;
        const bool falling = had_value && old_level && !new_level;
        *send = (rising && info->config.edge != SOLAR_OS_OSC_EDGE_FALLING) ||
                (falling && info->config.edge != SOLAR_OS_OSC_EDGE_RISING);
        *send_integer = true;
    } else {
        const bool changed = !info->has_sent_value ||
            (info->config.delta > 0.0f ?
                 fabsf(value - info->last_sent_value) >= info->config.delta :
                 value != info->last_sent_value);
        *send = info->config.send_always || changed;
    }
    if (*send) {
        slot->pending_value = value;
    }
    xSemaphoreGive(osc_mutex);
    return ESP_OK;
}

void solar_os_osc_binding_note_error(uint32_t id,
                                     uint64_t now_ms,
                                     esp_err_t error,
                                     bool send_error)
{
    if (id == 0U || osc_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    const int index = osc_binding_find_id_locked(id);
    if (index >= 0) {
        solar_os_osc_binding_info_t *info = &osc_bindings[index].info;
        info->last_sample_ms = now_ms;
        info->last_error = error;
        if (send_error) {
            info->send_errors++;
        } else {
            info->source_available = false;
            info->source_errors++;
        }
    }
    xSemaphoreGive(osc_mutex);
}

void solar_os_osc_binding_note_sent(uint32_t id, uint64_t now_ms)
{
    if (id == 0U || osc_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(osc_mutex, portMAX_DELAY);
    const int index = osc_binding_find_id_locked(id);
    if (index >= 0) {
        osc_binding_slot_t *slot = &osc_bindings[index];
        slot->info.has_sent_value = true;
        slot->info.last_sent_value = slot->pending_value;
        slot->info.last_send_ms = now_ms;
        slot->info.sent++;
        slot->info.last_error = ESP_OK;
    }
    xSemaphoreGive(osc_mutex);
}

static uint32_t osc_read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static void osc_write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static esp_err_t osc_read_string(const uint8_t *packet,
                                 size_t packet_len,
                                 size_t *offset,
                                 const char **text,
                                 size_t *text_len)
{
    if (packet == NULL || offset == NULL || text == NULL || text_len == NULL ||
        *offset >= packet_len) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t start = *offset;
    size_t end = start;
    while (end < packet_len && packet[end] != 0U) {
        end++;
    }
    if (end == packet_len) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t padded = (end + 4U) & ~(size_t)3U;
    if (padded > packet_len) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = end; i < padded; i++) {
        if (packet[i] != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    *text = (const char *)&packet[start];
    *text_len = end - start;
    *offset = padded;
    return ESP_OK;
}

static esp_err_t osc_parameter_path(const char *address,
                                    size_t address_len,
                                    char *path,
                                    size_t path_len,
                                    bool *normalized)
{
    const size_t prefix_len = sizeof(OSC_PARAMETER_PREFIX) - 1U;
    const size_t suffix_len = sizeof(OSC_PARAMETER_NORMALIZED_SUFFIX) - 1U;
    if (normalized == NULL || address_len <= prefix_len ||
        memcmp(address, OSC_PARAMETER_PREFIX, prefix_len) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t effective_len = address_len;
    *normalized = false;
    if (address_len > prefix_len + suffix_len &&
        memcmp(address + address_len - suffix_len,
               OSC_PARAMETER_NORMALIZED_SUFFIX, suffix_len) == 0) {
        effective_len -= suffix_len;
        *normalized = true;
    }
    if (effective_len - prefix_len >= path_len) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t out = 0U;
    bool component = false;
    for (size_t i = prefix_len; i < effective_len; i++) {
        const unsigned char ch = (unsigned char)address[i];
        if (ch == '/') {
            if (!component) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            path[out++] = '.';
            component = false;
        } else if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                     ch == '-' || ch == '_' || ch == '.')) {
            return ESP_ERR_INVALID_RESPONSE;
        } else {
            path[out++] = (char)ch;
            component = true;
        }
    }
    if (!component) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    path[out] = '\0';
    return ESP_OK;
}

static esp_err_t osc_dispatch_message(const uint8_t *packet,
                                      size_t packet_len,
                                      solar_os_osc_dispatch_result_t *result)
{
    size_t offset = 0U;
    const char *address = NULL;
    const char *types = NULL;
    size_t address_len = 0U;
    size_t types_len = 0U;
    esp_err_t err = osc_read_string(packet, packet_len, &offset,
                                    &address, &address_len);
    if (err != ESP_OK || address_len == 0U || address[0] != '/') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = osc_read_string(packet, packet_len, &offset, &types, &types_len);
    if (err != ESP_OK || types_len != 2U || types[0] != ',' ||
        strchr("fiTF", types[1]) == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    float value = 0.0f;
    if (types[1] == 'f' || types[1] == 'i') {
        if (offset + 4U != packet_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t raw = osc_read_u32(&packet[offset]);
        if (types[1] == 'f') {
            memcpy(&value, &raw, sizeof(value));
            if (!isfinite(value)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            value = (float)(int32_t)raw;
        }
    } else {
        if (offset != packet_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        value = types[1] == 'T' ? 1.0f : 0.0f;
    }

    char path[SOLAR_OS_PARAMETER_PATH_MAX];
    bool normalized = false;
    err = osc_parameter_path(address, address_len, path, sizeof(path),
                             &normalized);
    if (err == ESP_ERR_NOT_FOUND) {
        result->unknown_paths++;
        result->messages++;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (normalized) {
        solar_os_parameter_info_t info;
        err = solar_os_parameter_find(path, &info);
        if (err == ESP_OK &&
            (types[1] == 'i' || value < 0.0f || value > 1.0f)) {
            err = ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            const uint16_t normalized_value = (uint16_t)lroundf(
                value * (float)SOLAR_OS_PARAMETER_NORMALIZED_MAX);
            err = solar_os_parameter_set_normalized(path, normalized_value);
        }
    } else {
        err = solar_os_parameter_set(path, value);
    }
    result->messages++;
    if (err == ESP_OK) {
        result->applied++;
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        result->unknown_paths++;
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        result->rejected_values++;
        return ESP_OK;
    }
    return err;
}

static esp_err_t osc_dispatch_element(const uint8_t *packet,
                                      size_t packet_len,
                                      uint32_t depth,
                                      solar_os_osc_dispatch_result_t *result)
{
    if (packet_len == 0U || result->messages >= SOLAR_OS_OSC_PACKET_UPDATE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (packet_len >= 8U && memcmp(packet, "#bundle\0", 8U) == 0) {
        if (depth >= SOLAR_OS_OSC_BUNDLE_DEPTH_MAX || packet_len < 16U ||
            osc_read_u32(&packet[8]) != 0U || osc_read_u32(&packet[12]) != 1U) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        size_t offset = 16U;
        while (offset < packet_len) {
            if (offset + 4U > packet_len) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            const uint32_t element_len = osc_read_u32(&packet[offset]);
            offset += 4U;
            if (element_len == 0U || element_len > packet_len - offset) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            const esp_err_t err = osc_dispatch_element(&packet[offset],
                                                       element_len,
                                                       depth + 1U,
                                                       result);
            if (err != ESP_OK) {
                return err;
            }
            offset += element_len;
        }
        return offset == packet_len ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    return osc_dispatch_message(packet, packet_len, result);
}

esp_err_t solar_os_osc_dispatch_packet(
    const uint8_t *packet,
    size_t packet_len,
    solar_os_osc_dispatch_result_t *result)
{
    if (packet == NULL || result == NULL || packet_len == 0U ||
        packet_len > SOLAR_OS_OSC_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    return osc_dispatch_element(packet, packet_len, 0U, result);
}

static esp_err_t osc_encode(const char *address,
                            char type,
                            uint32_t raw,
                            uint8_t *packet,
                            size_t packet_capacity,
                            size_t *packet_len)
{
    if (!osc_address_valid(address) || packet == NULL || packet_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t address_len = strlen(address) + 1U;
    const size_t address_padded = (address_len + 3U) & ~(size_t)3U;
    const size_t total = address_padded + 4U + 4U;
    if (total > packet_capacity || total > SOLAR_OS_OSC_PACKET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(packet, 0, total);
    memcpy(packet, address, address_len);
    packet[address_padded] = ',';
    packet[address_padded + 1U] = (uint8_t)type;
    osc_write_u32(&packet[address_padded + 4U], raw);
    *packet_len = total;
    return ESP_OK;
}

esp_err_t solar_os_osc_encode_float(const char *address,
                                    float value,
                                    uint8_t *packet,
                                    size_t packet_capacity,
                                    size_t *packet_len)
{
    if (!isfinite(value)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t raw = 0U;
    memcpy(&raw, &value, sizeof(raw));
    return osc_encode(address, 'f', raw, packet, packet_capacity, packet_len);
}

esp_err_t solar_os_osc_encode_int(const char *address,
                                  int32_t value,
                                  uint8_t *packet,
                                  size_t packet_capacity,
                                  size_t *packet_len)
{
    return osc_encode(address, 'i', (uint32_t)value,
                      packet, packet_capacity, packet_len);
}

const char *solar_os_osc_source_name(solar_os_osc_source_t source)
{
    return source == SOLAR_OS_OSC_SOURCE_CONTROL ? "control" : "stream";
}

const char *solar_os_osc_value_name(solar_os_osc_value_t value)
{
    return value == SOLAR_OS_OSC_VALUE_EVENT ? "event" : "scalar";
}

const char *solar_os_osc_edge_name(solar_os_osc_edge_t edge)
{
    if (edge == SOLAR_OS_OSC_EDGE_RISING) {
        return "rising";
    }
    if (edge == SOLAR_OS_OSC_EDGE_FALLING) {
        return "falling";
    }
    return "both";
}
