#include "nmea.h"

#include <stdint.h>
#include <string.h>

#define NMEA_FIELD_MAX 20U

void nmea_parser_reset(nmea_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
}

void nmea_fix_state_reset(nmea_fix_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void nmea_fix_state_drop_gga(nmea_fix_state_t *state)
{
    state->quality = 0U;
    state->satellites = 0U;
    state->hdop_e2 = 0U;
    state->altitude_valid = false;
    state->altitude_msl_mm = 0;
}

static bool hex_value(char c, uint8_t *value)
{
    if (c >= '0' && c <= '9') {
        *value = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *value = (uint8_t)(c - 'A' + 10);
        return true;
    }
    return false;
}

/* Anything larger is no real GNSS field; the cap keeps every multiply below
 * (at most six more decades) inside int64. */
#define NMEA_SCALED_LIMIT 100000000000LL

/* Parses an unsigned decimal with an optional fraction into value * 10^scale.
 * Returns false on empty, malformed or oversized input. */
static bool parse_scaled(const char *text, unsigned scale, int64_t *out)
{
    int64_t value = 0;
    size_t i = 0;
    bool any_digit = false;

    while (text[i] >= '0' && text[i] <= '9') {
        if (value > NMEA_SCALED_LIMIT) {
            return false;
        }
        value = value * 10 + (text[i] - '0');
        any_digit = true;
        i++;
    }
    if (text[i] == '.') {
        i++;
        while (scale > 0 && text[i] >= '0' && text[i] <= '9') {
            if (value > NMEA_SCALED_LIMIT) {
                return false;
            }
            value = value * 10 + (text[i] - '0');
            scale--;
            any_digit = true;
            i++;
        }
        while (text[i] >= '0' && text[i] <= '9') {
            i++;
        }
    }
    if (!any_digit || text[i] != '\0') {
        return false;
    }
    while (scale > 0) {
        value *= 10;
        scale--;
    }
    *out = value;
    return true;
}

static bool parse_u8_digits(const char *text, size_t offset, uint8_t *out)
{
    if (text[offset] < '0' || text[offset] > '9' ||
        text[offset + 1] < '0' || text[offset + 1] > '9') {
        return false;
    }
    *out = (uint8_t)((text[offset] - '0') * 10 + (text[offset + 1] - '0'));
    return true;
}

/* "hhmmss" or "hhmmss.sss" */
static bool parse_time(const char *text,
                       uint8_t *hour,
                       uint8_t *minute,
                       uint8_t *second)
{
    const size_t length = strlen(text);
    if (length < 6U) {
        return false;
    }
    if (length > 6U) {
        if (text[6] != '.' || length == 7U) {
            return false;
        }
        for (size_t i = 7U; i < length; i++) {
            if (text[i] < '0' || text[i] > '9') {
                return false;
            }
        }
    }
    return parse_u8_digits(text, 0, hour) &&
        parse_u8_digits(text, 2, minute) &&
        parse_u8_digits(text, 4, second) &&
        *hour <= 23U && *minute <= 59U && *second <= 60U;
}

/* "ddmmyy" */
static bool parse_date(const char *text,
                       uint16_t *year,
                       uint8_t *month,
                       uint8_t *day)
{
    uint8_t short_year = 0;
    if (strlen(text) != 6U || !parse_u8_digits(text, 0, day) ||
        !parse_u8_digits(text, 2, month) ||
        !parse_u8_digits(text, 4, &short_year)) {
        return false;
    }
    if (*day < 1U || *day > 31U || *month < 1U || *month > 12U) {
        return false;
    }
    *year = (uint16_t)(2000U + short_year);
    static const uint8_t days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    uint8_t days = days_per_month[*month - 1U];
    if (*month == 2U &&
        ((*year % 4U == 0U && *year % 100U != 0U) || *year % 400U == 0U)) {
        days = 29U;
    }
    return *day <= days;
}

/* "ddmm.mmmm" / "dddmm.mmmm" with a hemisphere letter, into degrees * 1e7.
 * Magnitudes above max_e7 are rejected. */
static bool parse_coordinate(const char *text,
                             const char *hemisphere,
                             char positive,
                             char negative,
                             int64_t max_e7,
                             int32_t *out)
{
    const char *dot = strchr(text, '.');
    const size_t int_len = dot != NULL ? (size_t)(dot - text) : strlen(text);
    if (int_len < 3U || int_len > 5U) {
        return false;
    }
    int32_t degrees = 0;
    for (size_t i = 0; i + 2U < int_len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        degrees = degrees * 10 + (text[i] - '0');
    }
    int64_t minutes_e6 = 0;
    if (!parse_scaled(text + int_len - 2U, 6U, &minutes_e6) ||
        minutes_e6 >= 60000000LL) {
        return false;
    }
    int64_t value = (int64_t)degrees * 10000000LL + (minutes_e6 + 3) / 6;
    if (value > max_e7 ||
        (hemisphere[0] != positive && hemisphere[0] != negative)) {
        return false;
    }
    *out = (int32_t)(hemisphere[0] == negative ? -value : value);
    return true;
}

static void parse_rmc(char fields[][NMEA_FIELD_MAX],
                      size_t field_count,
                      nmea_fix_state_t *state)
{
    if (field_count < 10U) {
        return;
    }
    state->rmc_count++;
    state->valid = fields[2][0] == 'A';
    state->date_valid = false;
    state->latitude_deg_e7 = 0;
    state->longitude_deg_e7 = 0;
    state->ground_speed_mm_s = 0;
    state->course_deg_e5 = 0;
    if (parse_time(fields[1], &state->hour, &state->minute, &state->second) &&
        parse_date(fields[9], &state->year, &state->month, &state->day)) {
        state->date_valid = true;
    }
    if (state->valid) {
        int32_t latitude = 0;
        int32_t longitude = 0;
        if (parse_coordinate(fields[3], fields[4], 'N', 'S', 900000000LL, &latitude) &&
            parse_coordinate(fields[5], fields[6], 'E', 'W', 1800000000LL, &longitude)) {
            state->latitude_deg_e7 = latitude;
            state->longitude_deg_e7 = longitude;
        } else {
            state->valid = false;
        }
        /* One knot is 514.444 mm/s. A speed is bounded before it is
         * scaled, so the product cannot overflow and the result fits. */
        const int64_t max_knots_e3 = (int64_t)INT32_MAX * 1000000LL / 514444LL;
        int64_t knots_e3 = 0;
        if (parse_scaled(fields[7], 3U, &knots_e3) && knots_e3 <= max_knots_e3) {
            state->ground_speed_mm_s =
                (int32_t)((knots_e3 * 514444LL) / 1000000LL);
        }
        int64_t course_e5 = 0;
        if (parse_scaled(fields[8], 5U, &course_e5) &&
            course_e5 <= 36000000LL) {
            state->course_deg_e5 = (int32_t)course_e5;
        }
    }
}

static void parse_gga(char fields[][NMEA_FIELD_MAX],
                      size_t field_count,
                      nmea_fix_state_t *state)
{
    if (field_count < 10U) {
        return;
    }
    state->gga_count++;
    state->quality = 0;
    state->satellites = 0;
    state->hdop_e2 = 0;
    state->altitude_valid = false;
    state->altitude_msl_mm = 0;
    int64_t value = 0;
    if (parse_scaled(fields[6], 0U, &value) && value <= 8) {
        state->quality = (uint8_t)value;
    }
    if (parse_scaled(fields[7], 0U, &value) && value <= 99) {
        state->satellites = (uint8_t)value;
    }
    /* Quality 0 is no fix, and an HDOP or altitude without one is not a
     * reading. */
    if (state->quality > 0U &&
        parse_scaled(fields[8], 2U, &value) && value <= 9999) {
        state->hdop_e2 = (uint16_t)value;
    }
    const bool negative = fields[9][0] == '-';
    if (state->quality > 0U &&
        parse_scaled(negative ? fields[9] + 1 : fields[9], 3U, &value) &&
        value <= 100000000LL) {
        state->altitude_msl_mm = (int32_t)(negative ? -value : value);
        state->altitude_valid = true;
    }
}

static bool parse_sentence(const char *line,
                           size_t length,
                           nmea_fix_state_t *state)
{
    /* "$GPRMC,...*hh": a two-character talker, then the sentence type. */
    if (length < 9U || line[0] != '$' || line[length - 3U] != '*') {
        return false;
    }
    uint8_t checksum = 0;
    for (size_t i = 1; i < length - 3U; i++) {
        checksum ^= (uint8_t)line[i];
    }
    uint8_t high = 0;
    uint8_t low = 0;
    if (!hex_value(line[length - 2U], &high) ||
        !hex_value(line[length - 1U], &low) ||
        checksum != (uint8_t)((high << 4) | low)) {
        return false;
    }

    const char *type = line + 3;
    const bool is_rmc = strncmp(type, "RMC,", 4) == 0;
    const bool is_gga = strncmp(type, "GGA,", 4) == 0;
    if (!is_rmc && !is_gga) {
        return false;
    }

    char fields[15][NMEA_FIELD_MAX] = {{0}};
    size_t field_count = 1;
    size_t field_length = 0;
    for (size_t i = 1; i < length - 3U && field_count < 15U; i++) {
        if (line[i] == ',') {
            field_count++;
            field_length = 0;
        } else if (field_length + 1U < NMEA_FIELD_MAX) {
            fields[field_count - 1U][field_length++] = line[i];
        } else {
            /* Truncation could hide an invalid suffix on a UTC field. */
            return false;
        }
    }

    if (is_rmc) {
        parse_rmc(fields, field_count, state);
    } else {
        parse_gga(fields, field_count, state);
    }
    return true;
}

bool nmea_parser_feed(nmea_parser_t *parser,
                      uint8_t byte,
                      nmea_fix_state_t *state)
{
    if (byte == '$') {
        parser->in_sentence = true;
        parser->length = 0;
        parser->line[parser->length++] = '$';
        return false;
    }
    if (!parser->in_sentence) {
        return false;
    }
    if (byte == '\r' || byte == '\n') {
        parser->in_sentence = false;
        parser->line[parser->length] = '\0';
        return parse_sentence(parser->line, parser->length, state);
    }
    if (parser->length + 1U >= NMEA_LINE_MAX) {
        parser->in_sentence = false;
        return false;
    }
    parser->line[parser->length++] = (char)byte;
    return false;
}
