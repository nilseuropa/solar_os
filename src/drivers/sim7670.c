#include "sim7670.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM7670_READ_CHUNK 96U
#define SIM7670_READ_SLICE_MS 100U
#define SIM7670_STATUS_TIMEOUT_MS 2000U
#define SIM7670_STATUS_AT_ATTEMPTS 3U

typedef enum {
    TERMINAL_NONE,
    TERMINAL_OK,
    TERMINAL_ERROR,
} terminal_result_t;

static bool command_valid(const char *command)
{
    if (command == NULL || strncmp(command, "AT", 2U) != 0) {
        return false;
    }
    const size_t len = strnlen(command, SIM7670_COMMAND_MAX + 1U);
    if (len < 2U || len > SIM7670_COMMAND_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (command[i] == '\r' || command[i] == '\n') {
            return false;
        }
    }
    return true;
}

static terminal_result_t terminal_result(const char *response)
{
    const char *line = response;
    while (line != NULL && *line != '\0') {
        while (*line == '\r' || *line == '\n') {
            line++;
        }
        const char *end = line;
        while (*end != '\0' && *end != '\r' && *end != '\n') {
            end++;
        }
        const size_t len = (size_t)(end - line);
        if (len == 2U && memcmp(line, "OK", 2U) == 0) {
            return TERMINAL_OK;
        }
        if ((len == 5U && memcmp(line, "ERROR", 5U) == 0) ||
            (len >= 10U && memcmp(line, "+CME ERROR", 10U) == 0) ||
            (len >= 10U && memcmp(line, "+CMS ERROR", 10U) == 0)) {
            return TERMINAL_ERROR;
        }
        line = end;
    }
    return TERMINAL_NONE;
}

static void drain_input(sim7670_t *device)
{
    uint8_t discarded[SIM7670_READ_CHUNK];
    for (size_t i = 0; i < 8U; i++) {
        size_t read_len = 0U;
        const esp_err_t ret = device->io.read(device->io.user,
                                               discarded,
                                               sizeof(discarded),
                                               0U,
                                               &read_len);
        if ((ret != ESP_OK && ret != ESP_ERR_TIMEOUT) || read_len == 0U) {
            break;
        }
    }
}

esp_err_t sim7670_init(sim7670_t *device, const sim7670_io_t *io)
{
    if (device == NULL || io == NULL || io->write == NULL || io->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *device = (sim7670_t) {
        .io = *io,
        .initialized = true,
    };
    return ESP_OK;
}

esp_err_t sim7670_command(sim7670_t *device,
                          const char *command,
                          uint32_t timeout_ms,
                          char *response,
                          size_t response_size)
{
    if (device == NULL || !device->initialized || !command_valid(command) ||
        timeout_ms == 0U || response == NULL || response_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    response[0] = '\0';
    drain_input(device);

    char request[SIM7670_COMMAND_MAX + 3U];
    const int request_len = snprintf(request, sizeof(request), "%s\r\n", command);
    if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t written = 0U;
    esp_err_t ret = device->io.write(device->io.user,
                                     (const uint8_t *)request,
                                     (size_t)request_len,
                                     &written);
    if (ret != ESP_OK) {
        return ret;
    }
    if (written != (size_t)request_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t used = 0U;
    uint32_t remaining_ms = timeout_ms;
    while (remaining_ms > 0U) {
        uint8_t chunk[SIM7670_READ_CHUNK];
        size_t read_len = 0U;
        const uint32_t wait_ms = remaining_ms > SIM7670_READ_SLICE_MS
            ? SIM7670_READ_SLICE_MS
            : remaining_ms;
        ret = device->io.read(device->io.user,
                              chunk,
                              sizeof(chunk),
                              wait_ms,
                              &read_len);
        remaining_ms -= wait_ms;
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            return ret;
        }
        if (read_len == 0U) {
            continue;
        }
        if (read_len >= response_size - used) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(response + used, chunk, read_len);
        used += read_len;
        response[used] = '\0';
        const terminal_result_t terminal = terminal_result(response);
        if (terminal == TERMINAL_OK) {
            return ESP_OK;
        }
        if (terminal == TERMINAL_ERROR) {
            return ESP_FAIL;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static const char *find_line(const char *response, const char *prefix)
{
    const size_t prefix_len = strlen(prefix);
    const char *line = response;
    while (line != NULL && *line != '\0') {
        while (*line == '\r' || *line == '\n') {
            line++;
        }
        if (strncmp(line, prefix, prefix_len) == 0) {
            return line + prefix_len;
        }
        line = strpbrk(line, "\r\n");
    }
    return NULL;
}

bool sim7670_parse_csq(const char *response,
                       bool *rssi_valid,
                       int16_t *rssi_dbm,
                       uint8_t *bit_error_rate)
{
    if (response == NULL || rssi_valid == NULL || rssi_dbm == NULL ||
        bit_error_rate == NULL) {
        return false;
    }
    const char *value = find_line(response, "+CSQ:");
    int rssi = 0;
    int ber = 0;
    if (value == NULL || sscanf(value, " %d,%d", &rssi, &ber) != 2 ||
        rssi < 0 || rssi > 99 || ber < 0 || ber > 99) {
        return false;
    }
    *rssi_valid = rssi != 99;
    *rssi_dbm = rssi == 99 ? 0 : (int16_t)(-113 + 2 * rssi);
    *bit_error_rate = (uint8_t)ber;
    return true;
}

bool sim7670_parse_cereg(const char *response,
                         sim7670_registration_t *registration)
{
    if (response == NULL || registration == NULL) {
        return false;
    }
    const char *value = find_line(response, "+CEREG:");
    if (value == NULL) {
        return false;
    }
    while (*value == ' ') {
        value++;
    }
    char *end = NULL;
    long first = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }
    long stat = first;
    if (*end == ',') {
        const char *second = end + 1;
        stat = strtol(second, &end, 10);
        if (end == second) {
            return false;
        }
    }
    switch (stat) {
    case 0:
        *registration = SIM7670_REGISTRATION_NOT_REGISTERED;
        break;
    case 1:
        *registration = SIM7670_REGISTRATION_HOME;
        break;
    case 2:
        *registration = SIM7670_REGISTRATION_SEARCHING;
        break;
    case 3:
        *registration = SIM7670_REGISTRATION_DENIED;
        break;
    case 5:
        *registration = SIM7670_REGISTRATION_ROAMING;
        break;
    default:
        *registration = SIM7670_REGISTRATION_UNKNOWN;
        break;
    }
    return true;
}

static bool parse_unsigned_field(const char *text,
                                 size_t digits,
                                 unsigned *value)
{
    if (text == NULL || value == NULL) {
        return false;
    }
    unsigned parsed = 0U;
    for (size_t i = 0; i < digits; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
        parsed = parsed * 10U + (unsigned)(text[i] - '0');
    }
    *value = parsed;
    return true;
}

static bool parse_coordinate(const char *text,
                             char hemisphere,
                             int32_t *degrees_e7)
{
    if (text == NULL || text[0] == '\0' || degrees_e7 == NULL) {
        return false;
    }
    char *end = NULL;
    const double raw = strtod(text, &end);
    if (end == text || *end != '\0' || raw < 0.0) {
        return false;
    }
    const int degrees = (int)(raw / 100.0);
    const double minutes = raw - (double)degrees * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) {
        return false;
    }
    double decimal = (double)degrees + minutes / 60.0;
    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal = -decimal;
    } else if (hemisphere != 'N' && hemisphere != 'E') {
        return false;
    }
    *degrees_e7 = (int32_t)(decimal * 10000000.0 +
        (decimal >= 0.0 ? 0.5 : -0.5));
    return true;
}

static size_t split_csv(char *line, char **fields, size_t field_count)
{
    size_t count = 0U;
    char *cursor = line;
    while (count < field_count) {
        fields[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    return count;
}

bool sim7670_parse_cgpsinfo(const char *response,
                            sim7670_gnss_fix_t *fix)
{
    if (response == NULL || fix == NULL) {
        return false;
    }
    memset(fix, 0, sizeof(*fix));
    const char *value = find_line(response, "+CGPSINFO:");
    if (value == NULL) {
        return false;
    }
    while (*value == ' ') {
        value++;
    }
    const size_t line_len = strcspn(value, "\r\n");
    if (line_len >= 160U) {
        return false;
    }
    char line[160];
    memcpy(line, value, line_len);
    line[line_len] = '\0';
    char *fields[9] = {0};
    if (split_csv(line, fields, 9U) < 6U) {
        return false;
    }
    if (fields[0][0] == '\0' || fields[1][0] == '\0' ||
        fields[2][0] == '\0' || fields[3][0] == '\0') {
        return true;
    }
    if (!parse_coordinate(fields[0], fields[1][0], &fix->latitude_deg_e7) ||
        !parse_coordinate(fields[2], fields[3][0], &fix->longitude_deg_e7)) {
        return false;
    }
    fix->valid = true;

    if (fields[4] != NULL && fields[5] != NULL &&
        strlen(fields[4]) >= 6U && strlen(fields[5]) >= 6U) {
        unsigned day = 0U;
        unsigned month = 0U;
        unsigned year = 0U;
        unsigned hour = 0U;
        unsigned minute = 0U;
        unsigned second = 0U;
        if (parse_unsigned_field(&fields[4][0], 2U, &day) &&
            parse_unsigned_field(&fields[4][2], 2U, &month) &&
            parse_unsigned_field(&fields[4][4], 2U, &year) &&
            parse_unsigned_field(&fields[5][0], 2U, &hour) &&
            parse_unsigned_field(&fields[5][2], 2U, &minute) &&
            parse_unsigned_field(&fields[5][4], 2U, &second) &&
            day >= 1U && day <= 31U && month >= 1U && month <= 12U &&
            hour <= 23U && minute <= 59U && second <= 60U) {
            fix->year = (uint16_t)(2000U + year);
            fix->month = (uint8_t)month;
            fix->day = (uint8_t)day;
            fix->hour = (uint8_t)hour;
            fix->minute = (uint8_t)minute;
            fix->second = (uint8_t)second;
            fix->time_valid = true;
        }
    }
    if (fields[6] != NULL && fields[6][0] != '\0') {
        char *end = NULL;
        const double altitude_m = strtod(fields[6], &end);
        if (end != fields[6] && *end == '\0') {
            fix->height_msl_mm = (int32_t)(altitude_m * 1000.0 +
                (altitude_m >= 0.0 ? 0.5 : -0.5));
        }
    }
    return true;
}

esp_err_t sim7670_read_status(sim7670_t *device,
                              sim7670_status_t *status)
{
    if (device == NULL || !device->initialized || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->registration = SIM7670_REGISTRATION_UNKNOWN;
    char response[256];
    esp_err_t ret = ESP_FAIL;
    for (size_t attempt = 0U;
         attempt < SIM7670_STATUS_AT_ATTEMPTS && ret != ESP_OK;
         attempt++) {
        ret = sim7670_command(device,
                              "AT",
                              SIM7670_STATUS_TIMEOUT_MS,
                              response,
                              sizeof(response));
    }
    if (ret != ESP_OK) {
        return ret;
    }
    status->online = true;

    ret = sim7670_command(device,
                          "AT+CPIN?",
                          SIM7670_STATUS_TIMEOUT_MS,
                          response,
                          sizeof(response));
    if (ret == ESP_OK) {
        status->sim_status_valid = find_line(response, "+CPIN:") != NULL;
        status->sim_ready = find_line(response, "+CPIN: READY") != NULL;
    }

    ret = sim7670_command(device,
                          "AT+CEREG?",
                          SIM7670_STATUS_TIMEOUT_MS,
                          response,
                          sizeof(response));
    if (ret == ESP_OK) {
        status->registration_status_valid =
            sim7670_parse_cereg(response, &status->registration);
    }

    ret = sim7670_command(device,
                          "AT+CSQ",
                          SIM7670_STATUS_TIMEOUT_MS,
                          response,
                          sizeof(response));
    if (ret == ESP_OK) {
        status->signal_status_valid =
            sim7670_parse_csq(response,
                              &status->rssi_valid,
                              &status->rssi_dbm,
                              &status->bit_error_rate);
    }
    return ESP_OK;
}

esp_err_t sim7670_set_gnss_power(sim7670_t *device, bool enabled)
{
    char response[192];
    return sim7670_command(device,
                           enabled ? "AT+CGNSSPWR=1" : "AT+CGNSSPWR=0",
                           5000U,
                           response,
                           sizeof(response));
}

esp_err_t sim7670_read_gnss_fix(sim7670_t *device,
                                uint32_t timeout_ms,
                                sim7670_gnss_fix_t *fix)
{
    if (fix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char response[384];
    const esp_err_t ret = sim7670_command(device,
                                          "AT+CGPSINFO",
                                          timeout_ms,
                                          response,
                                          sizeof(response));
    if (ret != ESP_OK) {
        return ret;
    }
    return sim7670_parse_cgpsinfo(response, fix)
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}

const char *sim7670_registration_name(sim7670_registration_t registration)
{
    switch (registration) {
    case SIM7670_REGISTRATION_NOT_REGISTERED:
        return "not-registered";
    case SIM7670_REGISTRATION_SEARCHING:
        return "searching";
    case SIM7670_REGISTRATION_DENIED:
        return "denied";
    case SIM7670_REGISTRATION_HOME:
        return "home";
    case SIM7670_REGISTRATION_ROAMING:
        return "roaming";
    case SIM7670_REGISTRATION_UNKNOWN:
    default:
        return "unknown";
    }
}
