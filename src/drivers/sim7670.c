#include "sim7670.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM7670_READ_CHUNK 96U
#define SIM7670_READ_SLICE_MS 100U
#define SIM7670_STATUS_TIMEOUT_MS 2000U
#define SIM7670_STATUS_AT_ATTEMPTS 3U
#define SIM7670_CONFIG_TIMEOUT_MS 5000U
#define SIM7670_BAUD_TIMEOUT_MS 5000U
#define SIM7670_ACTIVATION_TIMEOUT_MS 45000U
#define SIM7670_GNSS_POWER_TIMEOUT_MS 10000U
#define SIM7670_CONTEXT_ID 1U

typedef enum {
    TERMINAL_NONE,
    TERMINAL_OK,
    TERMINAL_CONNECT,
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
        if (len >= 7U && memcmp(line, "CONNECT", 7U) == 0 &&
            (len == 7U || line[7] == ' ')) {
            return TERMINAL_CONNECT;
        }
        if ((len == 5U && memcmp(line, "ERROR", 5U) == 0) ||
            (len >= 10U && memcmp(line, "+CME ERROR", 10U) == 0) ||
            (len >= 10U && memcmp(line, "+CMS ERROR", 10U) == 0) ||
            (len == 10U && memcmp(line, "NO CARRIER", 10U) == 0) ||
            (len == 4U && memcmp(line, "BUSY", 4U) == 0) ||
            (len == 9U && memcmp(line, "NO ANSWER", 9U) == 0) ||
            (len == 11U && memcmp(line, "NO DIALTONE", 11U) == 0)) {
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
        if (terminal == TERMINAL_OK || terminal == TERMINAL_CONNECT) {
            return ESP_OK;
        }
        if (terminal == TERMINAL_ERROR) {
            return ESP_FAIL;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static bool uart_baud_rate_supported(uint32_t baud_rate)
{
    switch (baud_rate) {
    case 600U:
    case 1200U:
    case 2400U:
    case 4800U:
    case 9600U:
    case 19200U:
    case 38400U:
    case 57600U:
    case 115200U:
    case 230400U:
    case SIM7670_UART_MAX_BAUD_RATE:
        return true;
    default:
        return false;
    }
}

esp_err_t sim7670_set_uart_baud_rate(sim7670_t *device,
                                     uint32_t baud_rate)
{
    if (device == NULL || !device->initialized ||
        !uart_baud_rate_supported(baud_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    char command[32];
    const int len = snprintf(command,
                             sizeof(command),
                             "AT+IPR=%" PRIu32,
                             baud_rate);
    if (len <= 0 || (size_t)len >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char response[64];
    return sim7670_command(device,
                           command,
                           SIM7670_BAUD_TIMEOUT_MS,
                           response,
                           sizeof(response));
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

bool sim7670_parse_cgact(const char *response,
                         unsigned context_id,
                         bool *active)
{
    if (response == NULL || context_id == 0U || context_id > 15U ||
        active == NULL) {
        return false;
    }
    const char *line = response;
    while ((line = strstr(line, "+CGACT:")) != NULL) {
        line += strlen("+CGACT:");
        unsigned cid = 0U;
        unsigned state = 0U;
        if (sscanf(line, " %u,%u", &cid, &state) == 2 &&
            cid == context_id && state <= 1U) {
            *active = state == 1U;
            return true;
        }
    }
    return false;
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

static bool parse_uint8_field(const char *text, uint8_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > UINT8_MAX) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static void parse_gnss_datetime(const char *date,
                                const char *time,
                                sim7670_gnss_fix_t *fix)
{
    if (date == NULL || time == NULL || strlen(date) < 6U ||
        strlen(time) < 6U) {
        return;
    }
    unsigned day = 0U;
    unsigned month = 0U;
    unsigned year = 0U;
    unsigned hour = 0U;
    unsigned minute = 0U;
    unsigned second = 0U;
    if (parse_unsigned_field(&date[0], 2U, &day) &&
        parse_unsigned_field(&date[2], 2U, &month) &&
        parse_unsigned_field(&date[4], 2U, &year) &&
        parse_unsigned_field(&time[0], 2U, &hour) &&
        parse_unsigned_field(&time[2], 2U, &minute) &&
        parse_unsigned_field(&time[4], 2U, &second) &&
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

static bool parse_double_field(const char *text, double *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    const double parsed = strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static void trim_csv_field(char **field)
{
    char *start = *field;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    *field = start;
}

static size_t coordinate_integer_digits(const char *text)
{
    size_t digits = 0U;
    if (*text == '+' || *text == '-') {
        text++;
    }
    while (isdigit((unsigned char)*text)) {
        digits++;
        text++;
    }
    return digits;
}

static bool parse_decimal_coordinate(const char *text,
                                     char hemisphere,
                                     int32_t *degrees_e7)
{
    double decimal = 0.0;
    if (!parse_double_field(text, &decimal) || decimal < 0.0) {
        return false;
    }
    const double maximum = hemisphere == 'N' || hemisphere == 'S'
        ? 90.0
        : hemisphere == 'E' || hemisphere == 'W'
        ? 180.0
        : -1.0;
    if (maximum < 0.0 || decimal > maximum) {
        return false;
    }
    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal = -decimal;
    }
    *degrees_e7 = (int32_t)(decimal * 10000000.0 +
        (decimal >= 0.0 ? 0.5 : -0.5));
    return true;
}

bool sim7670_parse_cgnssinfo(const char *response,
                             sim7670_gnss_fix_t *fix)
{
    if (response == NULL || fix == NULL) {
        return false;
    }
    memset(fix, 0, sizeof(*fix));
    const char *value = find_line(response, "+CGNSSINFO:");
    if (value == NULL) {
        return false;
    }
    while (*value == ' ') {
        value++;
    }
    const size_t line_len = strcspn(value, "\r\n");
    if (line_len >= 256U) {
        return false;
    }
    char line[256];
    memcpy(line, value, line_len);
    line[line_len] = '\0';
    char *fields[24] = {0};
    const size_t field_count = split_csv(line, fields, 24U);
    if (field_count == 0U) {
        return false;
    }
    for (size_t i = 0U; i < field_count; i++) {
        trim_csv_field(&fields[i]);
    }

    uint8_t mode = 0U;
    if (fields[0][0] == '\0') {
        return true;
    }
    if (!parse_uint8_field(fields[0], &mode)) {
        return false;
    }
    if (mode != 2U && mode != 3U) {
        return true;
    }

    size_t hemisphere_index = SIZE_MAX;
    for (size_t i = 2U; i + 2U < field_count; i++) {
        const bool north_south =
            fields[i][1] == '\0' &&
            (fields[i][0] == 'N' || fields[i][0] == 'S');
        const bool east_west =
            fields[i + 2U][1] == '\0' &&
            (fields[i + 2U][0] == 'E' || fields[i + 2U][0] == 'W');
        if (north_south && east_west) {
            hemisphere_index = i;
            break;
        }
    }
    if (hemisphere_index == SIZE_MAX) {
        return false;
    }

    const size_t latitude_index = hemisphere_index - 1U;
    uint8_t visible = 0U;
    bool visible_valid = false;
    for (size_t i = 1U; i < latitude_index; i++) {
        uint8_t constellation = 0U;
        if (parse_uint8_field(fields[i], &constellation)) {
            visible_valid = true;
            if (UINT8_MAX - visible >= constellation) {
                visible = (uint8_t)(visible + constellation);
            }
        }
    }
    fix->satellites_valid = visible_valid;
    fix->satellites = visible;

    const bool degrees_minutes =
        coordinate_integer_digits(fields[latitude_index]) > 2U ||
        coordinate_integer_digits(fields[hemisphere_index + 1U]) > 3U;
    fix->fix_type = mode;
    const bool latitude_valid = degrees_minutes
        ? parse_coordinate(fields[latitude_index],
                           fields[hemisphere_index][0],
                           &fix->latitude_deg_e7)
        : parse_decimal_coordinate(fields[latitude_index],
                                   fields[hemisphere_index][0],
                                   &fix->latitude_deg_e7);
    const bool longitude_valid = degrees_minutes
        ? parse_coordinate(fields[hemisphere_index + 1U],
                           fields[hemisphere_index + 2U][0],
                           &fix->longitude_deg_e7)
        : parse_decimal_coordinate(fields[hemisphere_index + 1U],
                                   fields[hemisphere_index + 2U][0],
                                   &fix->longitude_deg_e7);
    if (!latitude_valid || !longitude_valid) {
        return false;
    }
    fix->valid = true;

    const size_t tail = hemisphere_index + 3U;
    if (tail + 1U < field_count) {
        parse_gnss_datetime(fields[tail], fields[tail + 1U], fix);
    }

    double parsed = 0.0;
    if (tail + 2U < field_count &&
        parse_double_field(fields[tail + 2U], &parsed)) {
        fix->height_msl_mm = (int32_t)(parsed * 1000.0 +
            (parsed >= 0.0 ? 0.5 : -0.5));
    }
    if (tail + 3U < field_count &&
        parse_double_field(fields[tail + 3U], &parsed) && parsed >= 0.0) {
        fix->ground_speed_mm_s = (int32_t)(parsed * 514.444 + 0.5);
    }
    if (tail + 4U < field_count &&
        parse_double_field(fields[tail + 4U], &parsed) && parsed >= 0.0) {
        fix->heading_deg_e5 = (int32_t)(parsed * 100000.0 + 0.5);
    }
    if (tail + 5U < field_count &&
        parse_double_field(fields[tail + 5U], &parsed) && parsed >= 0.0 &&
        parsed <= 655.35) {
        fix->position_dop_e2 = (uint16_t)(parsed * 100.0 + 0.5);
    }
    uint8_t used = 0U;
    if (tail + 8U < field_count &&
        parse_uint8_field(fields[tail + 8U], &used)) {
        fix->satellites_valid = true;
        fix->satellites = used;
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
    ret = sim7670_command(device,
                          "AT+CGACT?",
                          SIM7670_STATUS_TIMEOUT_MS,
                          response,
                          sizeof(response));
    if (ret == ESP_OK) {
        status->data_status_valid =
            sim7670_parse_cgact(response,
                                SIM7670_CONTEXT_ID,
                                &status->data_active);
    }
    return ESP_OK;
}

static bool quoted_value_valid(const char *value, size_t max_len, bool required)
{
    if (value == NULL) {
        return !required;
    }
    const size_t len = strnlen(value, max_len + 1U);
    if ((required && len == 0U) || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20U || ch > 0x7eU || ch == '"' || ch == '\\' ||
            ch == '\r' || ch == '\n') {
            return false;
        }
    }
    return true;
}

static bool apn_valid(const char *apn)
{
    if (!quoted_value_valid(apn, SIM7670_APN_MAX, true)) {
        return false;
    }
    for (const unsigned char *ch = (const unsigned char *)apn;
         *ch != '\0'; ch++) {
        if (!(isalnum(*ch) || *ch == '-' || *ch == '.')) {
            return false;
        }
    }
    return true;
}

static esp_err_t run_config_command(sim7670_t *device,
                                    const char *command,
                                    uint32_t timeout_ms)
{
    char response[256];
    return sim7670_command(device,
                           command,
                           timeout_ms,
                           response,
                           sizeof(response));
}

esp_err_t sim7670_configure_pdp(sim7670_t *device,
                                const char *apn,
                                sim7670_pdp_type_t pdp_type,
                                sim7670_auth_t auth,
                                const char *username,
                                const char *password)
{
    const bool credentials_required =
        auth == SIM7670_AUTH_PAP || auth == SIM7670_AUTH_CHAP;
    if (device == NULL || !device->initialized || !apn_valid(apn) ||
        pdp_type > SIM7670_PDP_IPV4V6 || auth > SIM7670_AUTH_AUTO ||
        !quoted_value_valid(username,
                            SIM7670_USERNAME_MAX,
                            credentials_required) ||
        !quoted_value_valid(password,
                            SIM7670_PASSWORD_MAX,
                            credentials_required) ||
        (!credentials_required &&
         ((username != NULL && username[0] != '\0') ||
          (password != NULL && password[0] != '\0')))) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *pdp_name = pdp_type == SIM7670_PDP_IPV4
        ? "IP"
        : (pdp_type == SIM7670_PDP_IPV6 ? "IPV6" : "IPV4V6");
    char command[SIM7670_COMMAND_MAX + 1U];
    int length = snprintf(command,
                          sizeof(command),
                          "AT+CGDCONT=%u,\"%s\",\"%s\"",
                          SIM7670_CONTEXT_ID,
                          pdp_name,
                          apn);
    if (length <= 0 || (size_t)length >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = run_config_command(device,
                                       command,
                                       SIM7670_CONFIG_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (auth == SIM7670_AUTH_NONE || auth == SIM7670_AUTH_AUTO) {
        length = snprintf(command,
                          sizeof(command),
                          "AT+CGAUTH=%u,0",
                          SIM7670_CONTEXT_ID);
    } else {
        length = snprintf(command,
                          sizeof(command),
                          "AT+CGAUTH=%u,%u,\"%s\",\"%s\"",
                          SIM7670_CONTEXT_ID,
                          auth == SIM7670_AUTH_PAP ? 1U : 2U,
                          password,
                          username);
    }
    if (length <= 0 || (size_t)length >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return run_config_command(device, command, SIM7670_CONFIG_TIMEOUT_MS);
}

esp_err_t sim7670_clear_pdp(sim7670_t *device)
{
    esp_err_t ret = sim7670_set_pdp_active(device, false);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = run_config_command(device,
                             "AT+CGDCONT=1",
                             SIM7670_CONFIG_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    return run_config_command(device,
                              "AT+CGAUTH=1,0",
                              SIM7670_CONFIG_TIMEOUT_MS);
}

esp_err_t sim7670_set_pdp_active(sim7670_t *device, bool active)
{
    if (device == NULL || !device->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    if (active) {
        esp_err_t ret = sim7670_set_packet_attached(device, true);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    esp_err_t ret = run_config_command(
        device,
        active ? "AT+CGACT=1,1" : "AT+CGACT=0,1",
        SIM7670_ACTIVATION_TIMEOUT_MS);
    if (!active && ret != ESP_OK) {
        /* LTE may reject deactivation of its last PDN. Detaching packet
         * service deactivates the context and gives disconnect its expected
         * modem-level semantics. */
        ret = run_config_command(device,
                                 "AT+CGATT=0",
                                 SIM7670_ACTIVATION_TIMEOUT_MS);
    }
    return ret;
}

esp_err_t sim7670_set_packet_attached(sim7670_t *device, bool attached)
{
    if (device == NULL || !device->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    return run_config_command(device,
                              attached ? "AT+CGATT=1" : "AT+CGATT=0",
                              SIM7670_ACTIVATION_TIMEOUT_MS);
}

esp_err_t sim7670_enter_data_mode(sim7670_t *device)
{
    if (device == NULL || !device->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    char response[128];
    const esp_err_t ret = sim7670_command(device,
                                          "ATD*99#",
                                          SIM7670_ACTIVATION_TIMEOUT_MS + 5000U,
                                          response,
                                          sizeof(response));
    return ret == ESP_OK && strstr(response, "CONNECT") != NULL
        ? ESP_OK
        : (ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret);
}

esp_err_t sim7670_unlock_sim(sim7670_t *device, const char *pin)
{
    if (device == NULL || !device->initialized || pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strnlen(pin, 9U);
    if (len < 4U || len > 8U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)pin[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    char command[24];
    const int length = snprintf(command, sizeof(command), "AT+CPIN=%s", pin);
    if (length <= 0 || (size_t)length >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return run_config_command(device, command, SIM7670_CONFIG_TIMEOUT_MS);
}

esp_err_t sim7670_set_gnss_power(sim7670_t *device, bool enabled)
{
    char response[192];
    return sim7670_command(device,
                           enabled ? "AT+CGNSSPWR=1" : "AT+CGNSSPWR=0",
                           SIM7670_GNSS_POWER_TIMEOUT_MS,
                           response,
                           sizeof(response));
}

esp_err_t sim7670_probe_gnss(sim7670_t *device)
{
    char response[256];
    const esp_err_t ret = sim7670_command(device,
                                          "AT+CGPSINFO",
                                          SIM7670_CONFIG_TIMEOUT_MS,
                                          response,
                                          sizeof(response));
    if (ret != ESP_OK) {
        return ret;
    }
    sim7670_gnss_fix_t fix;
    return sim7670_parse_cgpsinfo(response, &fix)
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t sim7670_configure_gnss(sim7670_t *device)
{
    const esp_err_t ret = run_config_command(device,
                                              "AT+CGNSSMODE=15",
                                              SIM7670_CONFIG_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    return run_config_command(device,
                              "AT+CGNSSTST=1",
                              SIM7670_CONFIG_TIMEOUT_MS);
}

esp_err_t sim7670_read_gnss_fix(sim7670_t *device,
                                uint32_t timeout_ms,
                                sim7670_gnss_fix_t *fix)
{
    if (fix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char response[512];
    const esp_err_t ret = sim7670_command(device,
                                          "AT+CGNSSINFO",
                                          timeout_ms,
                                          response,
                                          sizeof(response));
    if (ret != ESP_OK) {
        return ret;
    }
    return sim7670_parse_cgnssinfo(response, fix)
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
