import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NMEA_SOURCE = ROOT / "src/drivers/nmea.c"
NMEA_HEADER = ROOT / "src/drivers/nmea.h"


HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nmea.h"

static void feed_sentence(nmea_parser_t *parser,
                          nmea_fix_state_t *state,
                          const char *line,
                          bool expect_accepted)
{
    bool accepted = false;
    for (const char *c = line; *c != '\0'; c++) {
        if (nmea_parser_feed(parser, (uint8_t)*c, state)) {
            accepted = true;
        }
    }
    assert(accepted == expect_accepted);
}

int main(void)
{
    nmea_parser_t parser;
    nmea_fix_state_t state;
    nmea_parser_reset(&parser);
    nmea_fix_state_reset(&state);

    /* Canonical RMC example: 49 deg 16.45 min N, 123 deg 11.12 min W. */
    feed_sentence(&parser, &state,
                  "$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68\r\n",
                  true);
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

    /* GGA merges quality, satellites, HDOP, and altitude. */
    feed_sentence(&parser, &state,
                  "$GNGGA,225446.00,4916.45,N,12311.12,W,1,08,0.94,545.4,M,46.9,M,,*55\r\n",
                  true);
    assert(state.gga_count > 0);
    assert(state.quality == 1);
    assert(state.satellites == 8);
    assert(state.hdop_e2 == 94);
    assert(state.altitude_valid);
    assert(state.altitude_msl_mm == 545400);

    /* A corrupted checksum must not touch the state. */
    nmea_fix_state_reset(&state);
    feed_sentence(&parser, &state,
                  "$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*69\r\n",
                  false);
    assert(state.rmc_count == 0);

    /* A void fix keeps time but reports invalid. */
    feed_sentence(&parser, &state,
                  "$GNRMC,120000.00,V,,,,,,,010126,,,N*64\r\n",
                  true);
    assert(state.rmc_count > 0);
    assert(!state.valid);
    assert(state.date_valid);
    assert(state.year == 2026 && state.month == 1 && state.day == 1);

    /* Garbage between sentences and non-RMC/GGA types are ignored. */
    nmea_fix_state_reset(&state);
    feed_sentence(&parser, &state, "\xff\x00garbage$GPGSV,1,1,00*79\r\n", false);
    assert(state.rmc_count == 0 && state.gga_count == 0);

    /* Truncated sentence followed by a fresh valid one. */
    feed_sentence(&parser, &state, "$GPRMC,2254", false);
    feed_sentence(&parser, &state,
                  "$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68\r\n",
                  true);
    assert(state.valid);

    /* Southern and eastern hemispheres flip the signs. */
    nmea_fix_state_reset(&state);
    feed_sentence(&parser, &state,
                  "$GNRMC,120000.00,A,3348.70,S,15112.55,E,0.0,0.0,010126,,,A*57\r\n",
                  true);
    assert(state.valid);
    assert(state.latitude_deg_e7 == -338116667);
    assert(state.longitude_deg_e7 == 1512091667);

    /* Out-of-range coordinates and oversized numeric fields are rejected. */
    nmea_fix_state_reset(&state);
    feed_sentence(&parser, &state,
                  "$GPRMC,120000,A,9500.00,N,01000.00,E,0.0,0.0,010126,,,A*7A\r\n",
                  true);
    assert(!state.valid);
    feed_sentence(&parser, &state,
                  "$GPRMC,120000,A,4500.00,N,18100.00,E,0.0,0.0,010126,,,A*7E\r\n",
                  true);
    assert(!state.valid);
    feed_sentence(&parser, &state,
                  "$GPRMC,120000,A,4500.00,N,01000.00,E,9999999999999999999,0.0,010126,,,A*60\r\n",
                  true);
    assert(state.valid);
    assert(state.ground_speed_mm_s == 0);

    puts("ok");
    return 0;
}
'''


class NmeaParserTest(unittest.TestCase):
    def test_parser_compiles_and_passes_reference_vectors(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "nmea.c").write_text(NMEA_SOURCE.read_text(encoding="utf-8"))
            (work / "nmea.h").write_text(NMEA_HEADER.read_text(encoding="utf-8"))
            harness = work / "harness.c"
            harness.write_text(HARNESS)
            binary = work / "harness"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(work),
                            str(harness), str(work / "nmea.c"),
                            "-o", str(binary)], check=True)
            output = subprocess.check_output([str(binary)])
            self.assertEqual(output.strip(), b"ok")

    def test_parser_is_freestanding(self):
        source = NMEA_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn("malloc", source)
        self.assertNotIn("float", source)
        self.assertNotIn("double", source)
        for include in ("esp_", "freertos", "solar_os_"):
            self.assertNotIn(include, source)
