#include "solar_os_stream.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_ADC
#include "solar_os_adc.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif
#include "solar_os_board_caps.h"
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
#include "solar_os_battery.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_gpio.h"
#endif
#include "solar_os_port.h"
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
#include "solar_os_sensors.h"
#endif
#include "solar_os_time.h"

#define STREAM_BYTE_READ_MAX 64U
#define STREAM_MIC_DEFAULT_WINDOW_MS 100U
#define STREAM_MIC_MIN_WINDOW_MS 10U
#define STREAM_MIC_MAX_WINDOW_MS 1000U

typedef enum {
    STREAM_KIND_TEMPERATURE,
    STREAM_KIND_HUMIDITY,
    STREAM_KIND_BATTERY,
    STREAM_KIND_MIC,
    STREAM_KIND_ADC,
    STREAM_KIND_GPIO,
    STREAM_KIND_PORT,
} stream_kind_t;

typedef struct {
    const char *id;
    solar_os_stream_type_t type;
    const char *unit;
    const char *format;
    const char *summary;
    stream_kind_t kind;
    solar_os_board_capability_t required_capability;
    int index;
} stream_source_t;

static const stream_source_t singleton_streams[] = {
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    {
        .id = "temperature",
        .type = SOLAR_OS_STREAM_TYPE_SCALAR,
        .unit = "C",
        .format = "csv",
        .summary = "ambient temperature",
        .kind = STREAM_KIND_TEMPERATURE,
        .required_capability = SOLAR_OS_BOARD_CAP_TEMPERATURE,
    },
    {
        .id = "humidity",
        .type = SOLAR_OS_STREAM_TYPE_SCALAR,
        .unit = "percent",
        .format = "csv",
        .summary = "relative humidity",
        .kind = STREAM_KIND_HUMIDITY,
        .required_capability = SOLAR_OS_BOARD_CAP_HUMIDITY,
    },
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    {
        .id = "battery",
        .type = SOLAR_OS_STREAM_TYPE_SCALAR,
        .unit = "V",
        .format = "csv",
        .summary = "battery voltage and state",
        .kind = STREAM_KIND_BATTERY,
        .required_capability = SOLAR_OS_BOARD_CAP_BATTERY,
    },
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    {
        .id = "mic0",
        .type = SOLAR_OS_STREAM_TYPE_SCALAR,
        .unit = "percent",
        .format = "csv",
        .summary = "left microphone level",
        .kind = STREAM_KIND_MIC,
        .required_capability = SOLAR_OS_BOARD_CAP_AUDIO_INPUT,
        .index = 0,
    },
    {
        .id = "mic1",
        .type = SOLAR_OS_STREAM_TYPE_SCALAR,
        .unit = "percent",
        .format = "csv",
        .summary = "right microphone level",
        .kind = STREAM_KIND_MIC,
        .required_capability = SOLAR_OS_BOARD_CAP_AUDIO_INPUT,
        .index = 1,
    },
#endif
};

static bool stream_parse_pin_id(const char *id, const char *prefix, int *pin)
{
    const size_t prefix_len = strlen(prefix);

    if (id == NULL || strncmp(id, prefix, prefix_len) != 0 || id[prefix_len] == '\0') {
        return false;
    }

    for (const char *p = &id[prefix_len]; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    char *end = NULL;
    const long parsed = strtol(&id[prefix_len], &end, 10);
    if (end == &id[prefix_len] || *end != '\0' || parsed < 0 || parsed > 48) {
        return false;
    }

    if (pin != NULL) {
        *pin = (int)parsed;
    }
    return true;
}

static void stream_fill_info(solar_os_stream_info_t *info,
                             const char *id,
                             solar_os_stream_type_t type,
                             const char *unit,
                             const char *format,
                             const char *summary)
{
    memset(info, 0, sizeof(*info));
    strlcpy(info->id, id != NULL ? id : "", sizeof(info->id));
    info->type = type;
    strlcpy(info->unit, unit != NULL ? unit : "", sizeof(info->unit));
    strlcpy(info->format, format != NULL ? format : "csv", sizeof(info->format));
    strlcpy(info->summary, summary != NULL ? summary : "", sizeof(info->summary));
}

static bool stream_singleton_available(const stream_source_t *source)
{
    return source != NULL &&
        (source->required_capability == 0 ||
         solar_os_board_has(source->required_capability));
}

static size_t stream_singleton_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(singleton_streams) / sizeof(singleton_streams[0]); i++) {
        if (stream_singleton_available(&singleton_streams[i])) {
            count++;
        }
    }
    return count;
}

static bool stream_info_for_singleton(size_t index, solar_os_stream_info_t *info)
{
    size_t seen = 0;
    for (size_t i = 0; i < sizeof(singleton_streams) / sizeof(singleton_streams[0]); i++) {
        const stream_source_t *source = &singleton_streams[i];
        if (!stream_singleton_available(source)) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }

        stream_fill_info(info,
                         source->id,
                         source->type,
                         source->unit,
                         source->format,
                         source->summary);
        return true;
    }

    return false;
}

static bool stream_find_singleton(const char *id, const stream_source_t **source)
{
    if (id == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(singleton_streams) / sizeof(singleton_streams[0]); i++) {
        if (strcmp(id, singleton_streams[i].id) == 0 &&
            stream_singleton_available(&singleton_streams[i])) {
            if (source != NULL) {
                *source = &singleton_streams[i];
            }
            return true;
        }
    }

    return false;
}

static size_t stream_runtime_adc_count(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_ADC
    size_t count = 0;
    for (size_t i = 0; i < solar_os_adc_pin_count(); i++) {
        solar_os_adc_pin_info_t info;
        if (solar_os_adc_get_pin_info(i, &info) &&
            info.runtime_allowed &&
            info.adc_capable) {
            count++;
        }
    }
    return count;
#else
    return 0;
#endif
}

static bool stream_adc_info_by_runtime_index(size_t target, solar_os_stream_info_t *out)
{
#if SOLAR_OS_PACKAGE_SERVICE_ADC
    size_t seen = 0;

    for (size_t i = 0; i < solar_os_adc_pin_count(); i++) {
        solar_os_adc_pin_info_t info;
        if (!solar_os_adc_get_pin_info(i, &info) ||
            !info.runtime_allowed ||
            !info.adc_capable) {
            continue;
        }
        if (seen == target) {
            char id[SOLAR_OS_STREAM_ID_MAX];
            snprintf(id, sizeof(id), "adc%d", info.pin);
            stream_fill_info(out,
                             id,
                             SOLAR_OS_STREAM_TYPE_SCALAR,
                             "mV",
                             "csv",
                             "expansion ADC sample");
            return true;
        }
        seen++;
    }

    return false;
#else
    (void)target;
    (void)out;
    return false;
#endif
}

static size_t stream_runtime_gpio_count(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    size_t count = 0;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (solar_os_gpio_get_pin_info(i, &info) && info.runtime_allowed) {
            count++;
        }
    }
    return count;
#else
    return 0;
#endif
}

static bool stream_gpio_info_by_runtime_index(size_t target, solar_os_stream_info_t *out)
{
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    size_t seen = 0;

    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (!solar_os_gpio_get_pin_info(i, &info) || !info.runtime_allowed) {
            continue;
        }
        if (seen == target) {
            char id[SOLAR_OS_STREAM_ID_MAX];
            snprintf(id, sizeof(id), "gpio%d", info.pin);
            stream_fill_info(out,
                             id,
                             SOLAR_OS_STREAM_TYPE_EVENT,
                             "",
                             "csv",
                             "expansion GPIO state");
            return true;
        }
        seen++;
    }

    return false;
#else
    (void)target;
    (void)out;
    return false;
#endif
}

static size_t stream_readable_port_count(void)
{
    solar_os_port_info_t ports[SOLAR_OS_PORT_MAX];
    size_t count = 0;
    const size_t port_count = solar_os_port_list(ports, sizeof(ports) / sizeof(ports[0]));

    for (size_t i = 0; i < port_count && i < sizeof(ports) / sizeof(ports[0]); i++) {
        if ((ports[i].capabilities & SOLAR_OS_PORT_CAP_READ) != 0) {
            count++;
        }
    }
    return count;
}

static bool stream_port_info_by_readable_index(size_t target, solar_os_stream_info_t *out)
{
    solar_os_port_info_t ports[SOLAR_OS_PORT_MAX];
    size_t seen = 0;
    const size_t port_count = solar_os_port_list(ports, sizeof(ports) / sizeof(ports[0]));

    for (size_t i = 0; i < port_count && i < sizeof(ports) / sizeof(ports[0]); i++) {
        if ((ports[i].capabilities & SOLAR_OS_PORT_CAP_READ) == 0) {
            continue;
        }
        if (seen == target) {
            stream_fill_info(out,
                             ports[i].name,
                             SOLAR_OS_STREAM_TYPE_BYTES,
                             "bytes",
                             "csv",
                             ports[i].label);
            return true;
        }
        seen++;
    }
    return false;
}

size_t solar_os_stream_count(void)
{
    return stream_singleton_count() +
        stream_runtime_adc_count() +
        stream_runtime_gpio_count() +
        stream_readable_port_count();
}

bool solar_os_stream_get(size_t index, solar_os_stream_info_t *info)
{
    if (info == NULL) {
        return false;
    }

    const size_t singleton_count = stream_singleton_count();
    if (index < singleton_count) {
        return stream_info_for_singleton(index, info);
    }
    index -= singleton_count;

    const size_t adc_count = stream_runtime_adc_count();
    if (index < adc_count) {
        return stream_adc_info_by_runtime_index(index, info);
    }
    index -= adc_count;

    const size_t gpio_count = stream_runtime_gpio_count();
    if (index < gpio_count) {
        return stream_gpio_info_by_runtime_index(index, info);
    }
    index -= gpio_count;

    return stream_port_info_by_readable_index(index, info);
}

esp_err_t solar_os_stream_get_info(const char *id, solar_os_stream_info_t *info)
{
    if (id == NULL || id[0] == '\0' || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const stream_source_t *singleton = NULL;
    if (stream_find_singleton(id, &singleton)) {
        stream_fill_info(info,
                         singleton->id,
                         singleton->type,
                         singleton->unit,
                         singleton->format,
                         singleton->summary);
        return ESP_OK;
    }

    int pin = -1;
#if SOLAR_OS_PACKAGE_SERVICE_ADC
    if (stream_parse_pin_id(id, "adc", &pin)) {
        solar_os_adc_pin_info_t adc_info;
        for (size_t i = 0; i < solar_os_adc_pin_count(); i++) {
            if (solar_os_adc_get_pin_info(i, &adc_info) &&
                adc_info.pin == pin &&
                adc_info.runtime_allowed &&
                adc_info.adc_capable) {
                stream_fill_info(info,
                                 id,
                                 SOLAR_OS_STREAM_TYPE_SCALAR,
                                 "mV",
                                 "csv",
                                 "expansion ADC sample");
                return ESP_OK;
            }
        }
        return ESP_ERR_NOT_FOUND;
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    if (stream_parse_pin_id(id, "gpio", &pin)) {
        solar_os_gpio_pin_info_t gpio_info;
        if (solar_os_gpio_get_pin_info_by_pin(pin, &gpio_info) &&
            gpio_info.runtime_allowed) {
            stream_fill_info(info,
                             id,
                             SOLAR_OS_STREAM_TYPE_EVENT,
                             "",
                             "csv",
                             "expansion GPIO state");
            return ESP_OK;
        }
        return ESP_ERR_NOT_FOUND;
    }
#endif

    solar_os_port_info_t port_info;
    if (solar_os_port_get_info(id, &port_info) == ESP_OK &&
        (port_info.capabilities & SOLAR_OS_PORT_CAP_READ) != 0) {
        stream_fill_info(info,
                         id,
                         SOLAR_OS_STREAM_TYPE_BYTES,
                         "bytes",
                         "csv",
                         port_info.label);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

const char *solar_os_stream_type_name(solar_os_stream_type_t type)
{
    switch (type) {
    case SOLAR_OS_STREAM_TYPE_SCALAR:
        return "scalar";
    case SOLAR_OS_STREAM_TYPE_EVENT:
        return "event";
    case SOLAR_OS_STREAM_TYPE_BYTES:
        return "bytes";
    default:
        return "unknown";
    }
}

esp_err_t solar_os_stream_open(const char *id,
                               const char *owner,
                               solar_os_stream_handle_t *handle)
{
    solar_os_stream_info_t info;
    esp_err_t err = solar_os_stream_get_info(id, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (owner == NULL || owner[0] == '\0' || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    strlcpy(handle->id, info.id, sizeof(handle->id));
    handle->type = info.type;

    if (info.type == SOLAR_OS_STREAM_TYPE_BYTES) {
        err = solar_os_port_claim(info.id, owner, &handle->port);
        if (err != ESP_OK) {
            *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
            return err;
        }
    }

    return ESP_OK;
}

void solar_os_stream_close(solar_os_stream_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }
    if (solar_os_port_handle_valid(&handle->port)) {
        (void)solar_os_port_release(&handle->port);
    }
    *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
}

esp_err_t solar_os_stream_csv_header(const solar_os_stream_info_t *info,
                                     char *header,
                                     size_t header_len)
{
    if (info == NULL || header == NULL || header_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (info->type) {
    case SOLAR_OS_STREAM_TYPE_BYTES:
        strlcpy(header, "time_ms,uptime_ms,stream,hex,text", header_len);
        break;
    case SOLAR_OS_STREAM_TYPE_EVENT:
        strlcpy(header, "time_ms,uptime_ms,stream,value", header_len);
        break;
    case SOLAR_OS_STREAM_TYPE_SCALAR:
    default:
        if (strncmp(info->id, "adc", 3) == 0) {
            strlcpy(header, "time_ms,uptime_ms,stream,raw,mv", header_len);
        } else if (strncmp(info->id, "mic", 3) == 0) {
            strlcpy(header, "time_ms,uptime_ms,stream,peak_percent,avg_percent", header_len);
        } else if (strcmp(info->id, "battery") == 0) {
            strlcpy(header, "time_ms,uptime_ms,stream,voltage_v,percent", header_len);
        } else if (strcmp(info->id, "temperature") == 0) {
            strlcpy(header, "time_ms,uptime_ms,stream,temperature_c", header_len);
        } else if (strcmp(info->id, "humidity") == 0) {
            strlcpy(header, "time_ms,uptime_ms,stream,humidity_percent", header_len);
        } else {
            strlcpy(header, "time_ms,uptime_ms,stream,value", header_len);
        }
        break;
    }

    return strlen(header) + 1 < header_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
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
        return snprintf(line,
                        line_len,
                        "%" PRIu64 ",%" PRIu64 ",%s,",
                        record->time_ms,
                        record->uptime_ms,
                        id);
    }

    return snprintf(line, line_len, ",%" PRIu64 ",%s,", record->uptime_ms, id);
}

static bool stream_csv_append(char *line, size_t line_len, int offset, const char *suffix)
{
    if (offset < 0 || (size_t)offset >= line_len) {
        return false;
    }

    const int written = snprintf(&line[offset], line_len - (size_t)offset, "%s", suffix);
    return written >= 0 && (size_t)written < line_len - (size_t)offset;
}

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
static esp_err_t stream_record_temperature(solar_os_stream_csv_record_t *record)
{
    solar_os_environment_t environment;
    const esp_err_t err = solar_os_sensors_read_environment(&environment);
    if (err != ESP_OK) {
        return err;
    }

    const int offset = stream_csv_prefix(record, "temperature", record->line, sizeof(record->line));
    char suffix[48];
    snprintf(suffix, sizeof(suffix), "%.2f", environment.temperature_c);
    snprintf(record->change_key, sizeof(record->change_key), "%.2f", environment.temperature_c);
    return stream_csv_append(record->line, sizeof(record->line), offset, suffix) ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}

static esp_err_t stream_record_humidity(solar_os_stream_csv_record_t *record)
{
    solar_os_environment_t environment;
    const esp_err_t err = solar_os_sensors_read_environment(&environment);
    if (err != ESP_OK) {
        return err;
    }

    const int offset = stream_csv_prefix(record, "humidity", record->line, sizeof(record->line));
    char suffix[48];
    snprintf(suffix, sizeof(suffix), "%.2f", environment.humidity_percent);
    snprintf(record->change_key, sizeof(record->change_key), "%.2f", environment.humidity_percent);
    return stream_csv_append(record->line, sizeof(record->line), offset, suffix) ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
static esp_err_t stream_record_battery(solar_os_stream_csv_record_t *record)
{
    solar_os_battery_status_t status;
    const esp_err_t err = solar_os_battery_get_status(&status);
    if (err != ESP_OK) {
        return err;
    }

    const int offset = stream_csv_prefix(record, "battery", record->line, sizeof(record->line));
    char suffix[48];
    snprintf(suffix,
             sizeof(suffix),
             "%u.%03u,%u",
             (unsigned)(status.voltage_mv / 1000U),
             (unsigned)(status.voltage_mv % 1000U),
             (unsigned)status.percent);
    snprintf(record->change_key,
             sizeof(record->change_key),
             "%u:%u",
             (unsigned)status.voltage_mv,
             (unsigned)status.percent);
    return stream_csv_append(record->line, sizeof(record->line), offset, suffix) ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
static esp_err_t stream_record_adc(const char *id, solar_os_stream_csv_record_t *record)
{
    int pin = -1;
    if (!stream_parse_pin_id(id, "adc", &pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_adc_sample_t sample;
    const esp_err_t err = solar_os_adc_read(pin, &sample);
    if (err != ESP_OK) {
        return err;
    }

    const int offset = stream_csv_prefix(record, id, record->line, sizeof(record->line));
    char suffix[48];
    snprintf(suffix,
             sizeof(suffix),
             "%d,%u",
             sample.raw,
             (unsigned)sample.voltage_mv);
    snprintf(record->change_key,
             sizeof(record->change_key),
             "%d:%u",
             sample.raw,
             (unsigned)sample.voltage_mv);
    return stream_csv_append(record->line, sizeof(record->line), offset, suffix) ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
static esp_err_t stream_record_gpio(const char *id, solar_os_stream_csv_record_t *record)
{
    int pin = -1;
    if (!stream_parse_pin_id(id, "gpio", &pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool level = false;
    const esp_err_t err = solar_os_gpio_read(pin, &level);
    if (err != ESP_OK) {
        return err;
    }

    const int offset = stream_csv_prefix(record, id, record->line, sizeof(record->line));
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%u", level ? 1U : 0U);
    snprintf(record->change_key, sizeof(record->change_key), "%u", level ? 1U : 0U);
    return stream_csv_append(record->line, sizeof(record->line), offset, suffix) ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
static esp_err_t stream_record_mic(const char *id,
                                   const solar_os_stream_read_options_t *options,
                                   solar_os_stream_csv_record_t *record)
{
    const uint8_t channel = strcmp(id, "mic1") == 0 ? 1U : 0U;
    uint32_t window_ms = STREAM_MIC_DEFAULT_WINDOW_MS;
    if (options != NULL && options->window_ms != 0) {
        window_ms = options->window_ms;
    }
    if (window_ms < STREAM_MIC_MIN_WINDOW_MS || window_ms > STREAM_MIC_MAX_WINDOW_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_audio_level_t level;
    const esp_err_t err = solar_os_audio_measure_channel_level(channel, window_ms, &level);
    if (err != ESP_OK) {
        return err;
    }

    const int offset = stream_csv_prefix(record, id, record->line, sizeof(record->line));
    char suffix[48];
    snprintf(suffix,
             sizeof(suffix),
             "%u,%u",
             (unsigned)level.peak_percent,
             (unsigned)level.average_percent);
    snprintf(record->change_key,
             sizeof(record->change_key),
             "%u:%u",
             (unsigned)level.peak_percent,
             (unsigned)level.average_percent);
    return stream_csv_append(record->line, sizeof(record->line), offset, suffix) ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}
#endif

static void stream_hex_encode(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    size_t pos = 0;

    if (out == NULL || out_len == 0) {
        return;
    }

    for (size_t i = 0; i < len && pos + 2 < out_len; i++) {
        out[pos++] = hex[(data[i] >> 4) & 0x0fU];
        out[pos++] = hex[data[i] & 0x0fU];
    }
    out[pos] = '\0';
}

static void stream_text_encode(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t pos = 0;

    if (out == NULL || out_len == 0) {
        return;
    }

    out[pos++] = '"';
    for (size_t i = 0; i < len && pos + 2 < out_len; i++) {
        const unsigned char ch = data[i];
        if (ch == '"') {
            if (pos + 3 >= out_len) {
                break;
            }
            out[pos++] = '"';
            out[pos++] = '"';
        } else {
            out[pos++] = isprint(ch) && ch != '\r' && ch != '\n' ? (char)ch : '.';
        }
    }
    if (pos < out_len) {
        out[pos++] = '"';
    }
    out[pos < out_len ? pos : out_len - 1] = '\0';
}

static esp_err_t stream_record_bytes(solar_os_stream_handle_t *handle,
                                     const solar_os_stream_read_options_t *options,
                                     solar_os_stream_csv_record_t *record)
{
    if (!solar_os_port_handle_valid(&handle->port)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[STREAM_BYTE_READ_MAX];
    size_t read_len = 0;
    const uint32_t timeout_ms = options != NULL ? options->timeout_ms : 0;
    const esp_err_t err = solar_os_port_read(&handle->port,
                                             data,
                                             sizeof(data),
                                             timeout_ms,
                                             &read_len);
    if (err != ESP_OK) {
        return err;
    }
    if (read_len == 0) {
        record->has_data = false;
        return ESP_ERR_TIMEOUT;
    }

    char hex[(STREAM_BYTE_READ_MAX * 2U) + 1U];
    char text[STREAM_BYTE_READ_MAX + 3U];
    stream_hex_encode(data, read_len, hex, sizeof(hex));
    stream_text_encode(data, read_len, text, sizeof(text));

    const int offset = stream_csv_prefix(record, handle->id, record->line, sizeof(record->line));
    char suffix[SOLAR_OS_STREAM_CSV_LINE_MAX];
    snprintf(suffix, sizeof(suffix), "%s,%s", hex, text);
    strlcpy(record->change_key, hex, sizeof(record->change_key));
    return stream_csv_append(record->line, sizeof(record->line), offset, suffix) ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_stream_read_csv(solar_os_stream_handle_t *handle,
                                   const solar_os_stream_read_options_t *options,
                                   solar_os_stream_csv_record_t *record)
{
    if (handle == NULL || handle->id[0] == '\0' || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(record, 0, sizeof(*record));
    record->has_data = true;
    stream_record_timestamp(record);

    if (handle->type == SOLAR_OS_STREAM_TYPE_BYTES) {
        return stream_record_bytes(handle, options, record);
    }

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    if (strcmp(handle->id, "temperature") == 0) {
        return stream_record_temperature(record);
    }
    if (strcmp(handle->id, "humidity") == 0) {
        return stream_record_humidity(record);
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    if (strcmp(handle->id, "battery") == 0) {
        return stream_record_battery(record);
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (strcmp(handle->id, "mic0") == 0 || strcmp(handle->id, "mic1") == 0) {
        return stream_record_mic(handle->id, options, record);
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ADC
    if (strncmp(handle->id, "adc", 3) == 0) {
        return stream_record_adc(handle->id, record);
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    if (strncmp(handle->id, "gpio", 4) == 0) {
        return stream_record_gpio(handle->id, record);
    }
#endif

    return ESP_ERR_NOT_FOUND;
}
