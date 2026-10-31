"""E1003 hardware contracts, independent of the existing Sticky profile."""
import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from solaros_board_manifest import generate_header, load_board_manifest, load_driver_catalog, required_packages, validate_board
from tests.test_foreground_presenter import production_functions


class E1003BoardTest(unittest.TestCase):
    def setUp(self):
        self.catalog = load_driver_catalog(ROOT / "boards/expansion_drivers.toml")
        self.board = load_board_manifest(ROOT / "boards/manifests/seeed_reterminal_e1003.toml")

    def test_native_usb_cannot_claim_the_i2c_pins(self):
        validate_board(self.board, self.catalog)
        caps = self.board["build"]["capabilities"]
        self.assertNotIn("cdc", caps)
        self.assertNotIn("cdc_usb_serial_jtag", self.board["build"]["drivers"])
        buses = {b["name"]: b for b in self.board["buses"]}
        self.assertEqual((buses["i2c0"]["sda"], buses["i2c0"]["scl"]), (19, 20))
        self.assertEqual((buses["uart0"]["tx"], buses["uart0"]["rx"]), (43, 44))

    def test_usb_console_autostarts_without_duplicate_startup_scripts(self):
        self.assertEqual(self.board["defines"]["SOLAR_OS_BOARD_DEFAULT_PORT_SHELL"], '"uart0"')
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define SOLAR_OS_PACKAGE_JOB_POINTER_ARROWS 0
#define ESP_LOGI(...) ((void)0)
typedef struct { int value; } solar_os_context_t;
static solar_os_context_t context;
static unsigned starts;
static esp_err_t result;
esp_err_t solar_os_port_shell_start(solar_os_context_t *ctx, const char *port,
    bool run_startup, uint8_t *id) {
    assert(ctx == &context && !strcmp(port,"uart0") && !run_startup && id);
    starts++; *id=16; return result;
}
'''
        source += production_functions((ROOT / "src/services/solar_os_board_boot.c").read_text(),
            ("solar_os_board_boot_start_jobs",))
        source += r'''
int main(void) {
    assert(solar_os_board_boot_start_jobs(&context) == ESP_OK);
#ifdef SOLAR_OS_BOARD_DEFAULT_PORT_SHELL
    assert(starts == 1);
    result = ESP_ERR_NO_MEM;
    assert(solar_os_board_boot_start_jobs(&context) == ESP_ERR_NO_MEM && starts == 2);
#else
    assert(starts == 0);
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "boot.c"), Path(tmp, "boot")
            path.write_text(source)
            for enabled in (True, False):
                flags = ['-DSOLAR_OS_BOARD_DEFAULT_PORT_SHELL="uart0"'] if enabled else []
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "tests/host"), str(path), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_display_sd_and_peripheral_bus_contract(self):
        devices = {d["name"]: d for d in self.board["devices"]}
        buses = {b["name"]: b for b in self.board["buses"]}
        spi = buses["spi0"]
        self.assertEqual((spi["sclk"], spi["miso"], spi["mosi"]), (7, 8, 9))
        self.assertEqual(set(spi["cs"]), {10, 14})
        self.assertEqual(devices["display0"]["driver"], "it8951")
        display = devices["display0"]["bindings"]
        self.assertNotIn("dc", display)
        self.assertEqual((display["power"], display["ite_power"], display["reset"], display["busy"]), (11, 21, 12, 13))
        for name in ("display0", "storage0"):
            self.assertEqual(devices[name]["bindings"]["spi"], "spi0")
        for name in ("touch0", "rtc0", "environment0", "charger0"):
            self.assertEqual(devices[name]["bindings"]["i2c"], "i2c0")
        charger = devices["charger0"]["bindings"]
        self.assertEqual((charger["addr"], charger["adc"], charger["power"], charger["divider"]), (0x6b, 1, 40, 2000))
        self.assertEqual((charger["input_current"], charger["charge_current"], charger["charge_voltage"]), (1000, 480, 4208))
        self.assertEqual(devices["mic0"]["bindings"], {"i2s": 0, "clk": 42, "din": 41, "power": 38, "slot": 0})

    def test_large_display_memory_and_generated_build(self):
        defines = self.board["defines"]
        self.assertEqual((defines["SOLAR_OS_BOARD_DISPLAY_WIDTH"], defines["SOLAR_OS_BOARD_DISPLAY_HEIGHT"]), ("1872", "1404"))
        self.assertEqual(defines["SOLAR_OS_BOARD_DISPLAY_DEFAULT_SCALE"], "3")
        packages = required_packages(self.board, self.catalog)
        self.assertIn("expansion_it8951", packages)
        self.assertIn("sy6974", packages)
        self.assertNotIn("expansion_sticky_display", packages)
        header = generate_header(self.board, self.catalog)
        self.assertIn('"IT8951/ED103TC2"', header)
        self.assertIn('.role = "ite_power", .value = 21', header)
        config = json.loads((ROOT / "boards/seeed_reterminal_e1003.json").read_text())
        self.assertEqual(config["upload"]["flash_size"], "32MB")
        self.assertEqual(config["build"]["psram_type"], "opi")

    def test_header_adc_limits_and_reserved_peripherals(self):
        pins = {p["gpio"]: p for p in self.board["pins"]}
        self.assertTrue(pins[6]["adc"])
        self.assertFalse(pins[47].get("adc", False))
        free = {p["gpio"] for p in pins.values() if p["policy"] == "free"}
        self.assertEqual(free, {6, 47})
        for pin in (1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 19, 20, 21, 38, 39, 40, 41, 42, 43, 44, 45, 46, 48):
            self.assertEqual(pins[pin]["policy"], "fixed")


if __name__ == "__main__":
    unittest.main()
