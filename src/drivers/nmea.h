#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal NMEA 0183 sentence parser for GNSS receivers that stream RMC and
 * GGA. Bytes are fed one at a time; a complete, checksum-valid RMC or GGA
 * sentence updates the accumulated fix state. The parser is freestanding:
 * no allocation, no floating point, no dependencies beyond <stdint.h>. */

#define NMEA_LINE_MAX 120U

typedef struct {
    bool valid;     /* RMC status field is 'A' */
    bool date_valid; /* RMC carried a date */
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    int32_t latitude_deg_e7;
    int32_t longitude_deg_e7;
    int32_t ground_speed_mm_s;
    int32_t course_deg_e5;
    bool altitude_valid;
    int32_t altitude_msl_mm;
    uint8_t quality; /* GGA fix quality */
    uint8_t satellites;
    uint16_t hdop_e2;
    uint32_t rmc_count; /* accepted RMC sentences since reset */
    uint32_t gga_count; /* accepted GGA sentences since reset */
} nmea_fix_state_t;

typedef struct {
    char line[NMEA_LINE_MAX];
    size_t length;
    bool in_sentence;
} nmea_parser_t;

void nmea_parser_reset(nmea_parser_t *parser);
void nmea_fix_state_reset(nmea_fix_state_t *state);

/* Forgets what only GGA supplies (quality, satellites, HDOP, altitude), for
 * a state whose last GGA is too old to stand behind. RMC fields stay. */
void nmea_fix_state_drop_gga(nmea_fix_state_t *state);

/* Feeds one byte. Returns true when the byte completed a checksum-valid RMC
 * or GGA sentence and the fix state was updated. */
bool nmea_parser_feed(nmea_parser_t *parser,
                      uint8_t byte,
                      nmea_fix_state_t *state);

#ifdef __cplusplus
}
#endif
