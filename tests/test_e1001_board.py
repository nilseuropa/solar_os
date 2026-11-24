"""E1001 hardware contracts: the 7.5-inch monochrome reTerminal."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from solaros_board_manifest import generate_header, load_board_manifest, load_driver_catalog, required_packages, validate_board
from tests.test_foreground_presenter import production_functions
from tests.test_keyboard_layouts import write_keyboard_layouts


class E1001BoardTest(unittest.TestCase):
    def setUp(self):
        self.catalog = load_driver_catalog(ROOT / "boards/expansion_drivers.toml")
        self.board = load_board_manifest(ROOT / "boards/manifests/seeed_reterminal_e1001.toml")

    def test_native_usb_cannot_claim_the_i2c_pins(self):
        validate_board(self.board, self.catalog)
        self.assertNotIn("cdc", self.board["build"]["capabilities"])
        buses = {b["name"]: b for b in self.board["buses"]}
        self.assertEqual((buses["i2c0"]["sda"], buses["i2c0"]["scl"]), (19, 20))
        self.assertEqual((buses["i2c1"]["sda"], buses["i2c1"]["scl"]), (39, 40))
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

    def test_display_uses_the_gdey075t7_panel_on_shared_spi(self):
        devices = {d["name"]: d for d in self.board["devices"]}
        display = devices["display0"]
        self.assertEqual(display["driver"], "uc8179")
        self.assertEqual(display["bindings"]["panel"], 2)
        self.assertEqual((display["bindings"]["cs"], display["bindings"]["dc"],
                          display["bindings"]["reset"], display["bindings"]["busy"]),
                         (10, 11, 12, 13))
        self.assertEqual(devices["storage0"]["bindings"]["cs"], 14)
        defines = self.board["defines"]
        self.assertEqual((defines["SOLAR_OS_BOARD_DISPLAY_WIDTH"],
                          defines["SOLAR_OS_BOARD_DISPLAY_HEIGHT"]), ("800", "480"))

    def test_buttons_match_the_e1003(self):
        defines = self.board["defines"]
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_KEY"], "GPIO_NUM_3")
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_BLE_PAIR"], "GPIO_NUM_4")
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_ESCAPE"], "GPIO_NUM_5")
        self.assertNotIn("SOLAR_OS_BOARD_PIN_APP_EXIT", defines)

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
            write_keyboard_layouts(tmp)
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
        self.assertEqual((layout["rows"], layout["columns"]), (8, 1))
        expected = [
            ("power", None, "3V3"), ("ground", None, "GND"),
            ("gpio", 46, "GPIO46"), ("gpio", 2, "GPIO2/ADC"),
            ("gpio", 17, "TX1"), ("gpio", 18, "RX1"),
            ("gpio", 20, "SCL"), ("gpio", 19, "SDA"),
        ]
        entries = self.board["connectors"]
        self.assertEqual(len(entries), len(expected))
        for row, (entry, (kind, gpio, label)) in enumerate(zip(entries, expected)):
            self.assertEqual((entry["connector"], entry["position"], entry["row"], entry["column"]),
                             ("J2", row + 1, row, 0))
            self.assertEqual((entry["kind"], entry.get("gpio"), entry["label"]), (kind, gpio, label))

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
    assert(layout.rows == 8 && layout.columns == 1);
    assert(SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT == 8);
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

    def test_flash_psram_and_generated_build(self):
        config = json.loads((ROOT / "boards/seeed_reterminal_e1001.json").read_text())
        self.assertEqual(config["upload"]["flash_size"], "32MB")
        self.assertEqual(config["build"]["psram_type"], "opi")
        self.assertEqual(self.board["defines"]["SOLAR_OS_BOARD_DISPLAY_DEFAULT_SCALE"], "2")
        packages = required_packages(self.board, self.catalog)
        for package in ("expansion_uc8179", "expansion_sdspi", "sy6974", "driver_sht40", "driver_pcf8563"):
            self.assertIn(package, packages)
        self.assertNotIn("expansion_it8951", packages)
        self.assertIn('"UC8179/GDEY075T7"', generate_header(self.board, self.catalog))

    def test_header_adc_limits_and_reserved_peripherals(self):
        pins = {p["gpio"]: p for p in self.board["pins"]}
        self.assertTrue(pins[2]["adc"])
        for pin in (17, 18, 46):
            self.assertFalse(pins[pin].get("adc", False))
        free = {p["gpio"] for p in pins.values() if p["policy"] == "free"}
        self.assertEqual(free, {2, 17, 18, 46})
        for pin in free:
            self.assertTrue(pins[pin]["pwm"])
        for pin in (0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19, 20, 21,
                    38, 39, 40, 41, 42, 43, 44, 45):
            self.assertEqual(pins[pin]["policy"], "fixed")


if __name__ == "__main__":
    unittest.main()
