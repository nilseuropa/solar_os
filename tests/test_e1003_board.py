"""E1003 hardware contracts, independent of the existing Sticky profile."""
import json
from pathlib import Path
import re
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

    def test_usb_console_requires_explicit_start(self):
        self.assertNotIn("SOLAR_OS_BOARD_DEFAULT_PORT_SHELL", self.board["defines"])
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
        self.assertEqual(display["clock"], 10000)
        self.assertEqual(self.board["defines"]["SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ"], "10000000")
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

    def test_button_holds_pair_or_escape_and_consume_release(self):
        source = (ROOT / "tests/host/buttons_test.c").read_text().split("int main(void)", 1)[0]
        source += '#include "solar_os_board.h"\n'
        source += r'''
static unsigned pairings, escapes;
static solar_os_input_source_t key_source;
static void handle_key_pairing(void) { pairings++; }
static void handle_key_input(uint8_t key) {
    assert(key == SOLAR_OS_KEY_ESCAPE); escapes++;
    assert(solar_os_input_write_char(key_source, (char)key) == ESP_OK);
}
'''
        source += production_functions((ROOT / "src/main.c").read_text(), ("board_button_long_press",))
        source += r'''
static void poll_after(unsigned ms) { now_us += ms * 1000ULL; solar_os_buttons_poll(); }
static void press(int pin) { levels[pin] = 0; poll_after(1); poll_after(25); }
static void release(int pin) { levels[pin] = 1; poll_after(1); poll_after(25); }
static void expect_key(uint8_t key) {
    solar_os_input_key_event_t events[2];
    assert(solar_os_input_read_events(events, 2) == 2);
    assert(events[0].key == key && events[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(events[1].key == key && events[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
}
int main(void) {
    assert(solar_os_input_init() == ESP_OK);
    assert(solar_os_input_key_source_open("key", SOLAR_OS_INPUT_SOURCE_BUTTONS, &key_source) == ESP_OK);
    levels[4] = 1; levels[5] = 0;
    solar_os_buttons_set_long_press_handler(board_button_long_press, NULL);
    assert(solar_os_buttons_init() == ESP_OK);
    /* A button held through startup must be released before it can act. */
    poll_after(5000); expect_no_events(); assert(escapes == 0);
    release(5); expect_no_events();
    press(4); poll_after(100); release(4); expect_key(SOLAR_OS_KEY_UP);
    press(4); poll_after(1999); expect_no_events(); assert(pairings == 0);
    poll_after(1); expect_no_events(); assert(pairings == 1);
    poll_after(5000); assert(pairings == 1);
    release(4); expect_no_events();
    press(5); poll_after(100); release(5); expect_key(SOLAR_OS_KEY_DOWN);
    press(5); poll_after(1999); expect_no_events(); assert(escapes == 0);
    poll_after(1); expect_key(SOLAR_OS_KEY_ESCAPE); assert(escapes == 1);
    poll_after(5000); expect_no_events(); assert(escapes == 1);
    release(5); expect_no_events();
    press(5); poll_after(100); release(5); expect_key(SOLAR_OS_KEY_DOWN);
    assert(!board_button_long_press(3, NULL)); expect_no_events();
    assert(pairings == 1 && escapes == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            defines = self.board["defines"]
            names = ("SOLAR_OS_BOARD_BUTTONS", "SOLAR_OS_BOARD_PIN_BLE_PAIR",
                     "SOLAR_OS_BOARD_PIN_APP_EXIT", "SOLAR_OS_BOARD_PIN_ESCAPE")
            header = '#pragma once\n#include "solar_os_keys.h"\n#define SOLAR_OS_BOARD_HAS_BUTTONS 1\n'
            for name in names:
                if name in defines:
                    header += f'#define {name} ' + re.sub(r"GPIO_NUM_(\d+)", r"\1", defines[name]) + '\n'
            Path(tmp, "solar_os_board.h").write_text(header)
            path, binary = Path(tmp, "buttons.c"), Path(tmp, "buttons")
            path.write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"), "-I", tmp,
                "-I", str(ROOT / "tests/host/gpio_controller_stubs"), "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"), "-idirafter", str(ROOT / "include"),
                str(path), *[str(ROOT / "src/services" / name) for name in (
                    "solar_os_input.c", "solar_os_keys.c", "solar_os_input_composition.c", "solar_os_buttons.c")],
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_connector_layout_is_complete_and_runtime_visible(self):
        layout = self.board["connector"]
        self.assertEqual((layout["rows"], layout["columns"]), (6, 1))
        expected = [
            ("power", None, "3V3"), ("ground", None, "GND"),
            ("gpio", 47, "GPIO47"), ("gpio", 6, "GPIO6/ADC"),
            ("gpio", 20, "SCL"), ("gpio", 19, "SDA"),
        ]
        entries = self.board["connectors"]
        self.assertEqual(len(entries), len(expected))
        for row, (entry, (kind, gpio, label)) in enumerate(zip(entries, expected)):
            self.assertEqual((entry["connector"], entry["position"], entry["row"], entry["column"]),
                             ("J2", row + 1, row, 0))
            self.assertEqual((entry["kind"], entry.get("gpio"), entry["label"]), (kind, gpio, label))

        # Exercise the actual runtime guard with the generated layout macros.
        # Connector entries alone previously left rows/columns zero, hiding J2.
        header = generate_header(self.board, self.catalog)
        macros = [line for line in header.splitlines() if line.startswith((
            "#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_",
            "#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT "))]
        source = '#include <assert.h>\n#include "solar_os_pins.h"\n' + "\n".join(macros) + "\n"
        source += production_functions((ROOT / "src/services/solar_os_pins.c").read_text(),
                                       ("solar_os_connector_layout_get_info",))
        source += r'''
int main(void) {
    solar_os_connector_layout_info_t layout;
    assert(solar_os_connector_layout_get_info(&layout));
    assert(layout.rows == 6 && layout.columns == 1);
    assert(SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT == 6);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "layout.c"), Path(tmp, "layout")
            path.write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"),
                str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
