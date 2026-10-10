#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nmea.h"

static nmea_parser_t parser;
static nmea_fix_state_t state;

static void reset(void)
{
    nmea_parser_reset(&parser);
    nmea_fix_state_reset(&state);
}

/* Returns whether any byte of the line completed an accepted sentence. */
static bool feed(const char *line)
{
    bool accepted = false;
    for (const char *c = line; *c != '\0'; c++) {
        accepted |= nmea_parser_feed(&parser, (uint8_t)*c, &state);
    }
    return accepted;
}

static bool feed_body(const char *body)
{
    uint8_t checksum = 0;
    for (const char *p = body; *p != '\0'; p++) {
        checksum ^= (uint8_t)*p;
    }
    char line[120];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, checksum);
    return feed(line);
}

static void feed_rmc_time_date(const char *time, const char *date)
{
    char body[100];
    snprintf(body, sizeof(body),
             "GPRMC,%s,A,4500.00,N,01000.00,E,0,0,%s,,,A", time, date);
    assert(feed_body(body));
    assert(state.valid); /* Invalid UTC must not discard a valid position. */
}

static void test_utc_calendar_and_time_syntax(void)
{
    reset();
    const char *valid_times[] = {"000000", "235959", "120000.0", "120000.123456", "235960"};
    for (size_t i = 0; i < sizeof(valid_times) / sizeof(valid_times[0]); i++) {
        feed_rmc_time_date(valid_times[i], "290224");
        assert(state.date_valid && state.year == 2024 && state.month == 2 && state.day == 29);
    }
    const char *invalid_times[] = {
        "", "12000", "120000junk", "120000.", "120000.12x", "1200000",
        "240000", "126000", "120061",
    };
    for (size_t i = 0; i < sizeof(invalid_times) / sizeof(invalid_times[0]); i++) {
        feed_rmc_time_date(invalid_times[i], "010126");
        assert(!state.date_valid);
    }
    const char *invalid_dates[] = {"310226", "290226", "310426", "000126", "011326", "01012", "010126x"};
    for (size_t i = 0; i < sizeof(invalid_dates) / sizeof(invalid_dates[0]); i++) {
        feed_rmc_time_date("120000", invalid_dates[i]);
        assert(!state.date_valid);
    }
    feed_rmc_time_date("120000", "290200");
    assert(state.date_valid && state.year == 2000);
    feed_rmc_time_date("120000", "300426");
    assert(state.date_valid && state.month == 4 && state.day == 30);
    const uint32_t before = state.rmc_count;
    assert(!feed_body("GPRMC,120000.1234567890123junk,A,4500.00,N,01000.00,E,0,0,061026,,,A"));
    assert(state.rmc_count == before); /* No truncated prefix becomes a fresh reading. */
}

static void test_rmc_position_time_and_motion(void)
{
    /* The canonical example: 49 deg 16.45 min N, 123 deg 11.12 min W. */
    reset();
    assert(feed("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68\r\n"));
    assert(state.rmc_count > 0);
    assert(state.valid);
    assert(state.date_valid);
    assert(state.year == 2094 && state.month == 11 && state.day == 19);
    assert(state.hour == 22 && state.minute == 54 && state.second == 46);
    assert(state.latitude_deg_e7 == 492741667);
    assert(state.longitude_deg_e7 == -1231853333);
    /* 0.5 knots is 257 mm/s. */
    assert(state.ground_speed_mm_s == 257);
    assert(state.course_deg_e5 == 5470000);
}

static void test_gga_quality_satellites_and_altitude(void)
{
    reset();
    assert(feed("$GNGGA,225446.00,4916.45,N,12311.12,W,1,08,0.94,545.4,M,46.9,M,,*55\r\n"));
    assert(state.gga_count > 0);
    assert(state.quality == 1);
    assert(state.satellites == 8);
    assert(state.hdop_e2 == 94);
    assert(state.altitude_valid);
    assert(state.altitude_msl_mm == 545400);
}

/* A GGA too old to trust takes its altitude, satellites and HDOP with it;
 * the position from RMC stays. */
static void test_stale_gga_is_dropped(void)
{
    reset();
    assert(feed("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68\r\n"));
    assert(feed("$GNGGA,225446.00,4916.45,N,12311.12,W,1,08,0.94,545.4,M,46.9,M,,*55\r\n"));
    nmea_fix_state_drop_gga(&state);
    assert(!state.altitude_valid && state.altitude_msl_mm == 0);
    assert(state.satellites == 0 && state.hdop_e2 == 0 && state.quality == 0);
    assert(state.valid);
    assert(state.latitude_deg_e7 == 492741667);
}

/* With fix quality 0 there is no fix, so its HDOP and altitude fields are
 * not readings. */
static void test_gga_without_a_fix_has_no_altitude(void)
{
    reset();
    assert(feed("$GNGGA,225446.00,4916.45,N,12311.12,W,0,00,99.99,545.4,M,46.9,M,,*61\r\n"));
    assert(state.quality == 0);
    assert(state.hdop_e2 == 0);
    assert(!state.altitude_valid && state.altitude_msl_mm == 0);
}

static void test_bad_checksum_is_rejected(void)
{
    reset();
    assert(!feed("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*69\r\n"));
    assert(state.rmc_count == 0);
}

static void test_void_fix_keeps_time(void)
{
    reset();
    assert(feed("$GNRMC,120000.00,V,,,,,,,010126,,,N*64\r\n"));
    assert(state.rmc_count > 0);
    assert(!state.valid);
    assert(state.date_valid);
    assert(state.year == 2026 && state.month == 1 && state.day == 1);
}

static void test_noise_and_other_sentences_are_ignored(void)
{
    reset();
    assert(!feed("\xff\x01garbage$GPGSV,1,1,00*79\r\n"));
    assert(state.rmc_count == 0 && state.gga_count == 0);

    /* A truncated sentence does not spoil the next one. */
    assert(!feed("$GPRMC,2254"));
    assert(feed("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68\r\n"));
    assert(state.valid);
}

static void test_southern_and_eastern_hemispheres(void)
{
    reset();
    assert(feed("$GNRMC,120000.00,A,3348.70,S,15112.55,E,0.0,0.0,010126,,,A*57\r\n"));
    assert(state.valid);
    assert(state.latitude_deg_e7 == -338116667);
    assert(state.longitude_deg_e7 == 1512091667);
}

static void test_out_of_range_fields_are_rejected(void)
{
    reset();
    assert(feed("$GPRMC,120000,A,9500.00,N,01000.00,E,0.0,0.0,010126,,,A*7A\r\n"));
    assert(!state.valid);
    assert(feed("$GPRMC,120000,A,4500.00,N,18100.00,E,0.0,0.0,010126,,,A*7E\r\n"));
    assert(!state.valid);
    /* An oversized speed is dropped; the position still stands. */
    assert(feed("$GPRMC,120000,A,4500.00,N,01000.00,E,9999999999999999999,0.0,010126,,,A*60\r\n"));
    assert(state.valid);
    assert(state.ground_speed_mm_s == 0);
    /* Large enough to overflow once scaled: left at zero, never wrapped. */
    assert(feed("$GPRMC,120000,A,4500.00,N,01000.00,E,100000000000,0.0,010126,,,A*58\r\n"));
    assert(state.valid && state.ground_speed_mm_s == 0);
    assert(feed("$GPRMC,120000,A,4500.00,N,01000.00,E,4200000,0.0,010126,,,A*6F\r\n"));
    assert(state.valid && state.ground_speed_mm_s == 0);
    /* Just under the bound still converts. */
    assert(feed("$GPRMC,120000,A,4500.00,N,01000.00,E,4100000,0.0,010126,,,A*6C\r\n"));
    assert(state.ground_speed_mm_s == 2109220400);
}

int main(void)
{
    test_utc_calendar_and_time_syntax();
    test_rmc_position_time_and_motion();
    test_gga_quality_satellites_and_altitude();
    test_stale_gga_is_dropped();
    test_gga_without_a_fix_has_no_altitude();
    test_bad_checksum_is_rejected();
    test_void_fix_keeps_time();
    test_noise_and_other_sentences_are_ignored();
    test_southern_and_eastern_hemispheres();
    test_out_of_range_fields_are_rejected();
    puts("nmea tests: ok");
    return 0;
}
