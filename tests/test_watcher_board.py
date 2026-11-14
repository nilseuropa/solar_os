from copy import deepcopy
from pathlib import Path
import sys
import unittest

from test_flavor_packages import generate_flavor_config
from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from solaros_board_manifest import (
    ManifestError, generate_header, load_board_manifest, load_driver_catalog,
    required_packages, validate_board,
)


class WatcherBoardTest(unittest.TestCase):
    def setUp(self):
        directory = ROOT / "boards/manifests"
        self.board = load_board_manifest(directory / "seeed_sensecap_watcher.toml", directory)
        self.drivers = load_driver_catalog(ROOT / "boards/expansion_drivers.toml")

    def test_builtin_devices_survive_flavor_pruning_without_camera_or_cdc(self):
        catalog = generate_flavor_config.load_catalog(ROOT / "packages/solar_os_packages.toml")
        for flavor in ("writerdeck", "full"):
            _, _, groups, packages = generate_flavor_config.load_flavor(
                ROOT / "flavors" / f"{flavor}.toml", catalog)
            required = set(required_packages(self.board, self.drivers))
            selected = generate_flavor_config.enable_required_packages(catalog, packages, required)
            selected = generate_flavor_config.apply_target_pruning(catalog, selected, "esp32s3")
            _, selected = generate_flavor_config.apply_board_capability_pruning(
                catalog, groups, selected, set(self.board["build"]["capabilities"]))
            self.assertTrue(all(selected[package] for package in required))
        validate_board(self.board, self.drivers)
        self.assertNotIn("cdc", self.board["build"]["capabilities"])
        self.assertNotIn("camera", self.board["build"]["capabilities"])
        self.assertFalse(any(device["driver"] == "esp32-camera" for device in self.board["devices"]))
        self.assertFalse(any(device.get("detachable", False) for device in self.board["devices"]))
        self.assertEqual(self.board["devices"][0]["name"], "gpiox0")
        self.assertTrue(self.drivers["pca9535"].early)

    def test_connector_gpio_ownership_and_shared_bus_conflicts(self):
        header = generate_header(self.board, self.drivers)
        pins = {pin["gpio"]: pin for pin in self.board["pins"]}
        free = {gpio for gpio, pin in pins.items() if pin["policy"] == "free"}
        self.assertEqual(free, {19, 20})
        connectors = {(p["connector"], p["position"]): p for p in self.board["connectors"]}
        self.assertEqual(len(connectors), 12)
        for key in (("J2", 3), ("J5", 2)):
            self.assertEqual(connectors[key]["gpio"], 47)
        for key in (("J2", 6), ("J5", 1)):
            self.assertEqual(connectors[key]["gpio"], 48)
        self.assertIn('.role = "button", .target = "gpiox0", .value = 3', header)
        self.assertIn('.role = "irq", .target = "gpiox0", .value = 5', header)
        # Reusing onboard SPI or I2C signals for another fixed peripheral is rejected.
        for pin in (4, 47):
            invalid = deepcopy(self.board)
            next(d for d in invalid["devices"] if d["name"] == "pixels0")["bindings"]["data"] = pin
            with self.assertRaises(ManifestError):
                validate_board(invalid, self.drivers)

    def test_primary_display_attaches_before_display_service_and_touch(self):
        # Exercise the real board-device dispatcher with the actual SPD2010
        # descriptor. A late display can attach successfully but leaves the
        # display service and touch source unavailable for the entire boot.
        source = (ROOT / "src/services/solar_os_spd2010_display.c").read_text()
        descriptor = source[source.index(
            "const solar_os_expansion_driver_t solar_os_spd2010_expansion_driver") :]
        dispatcher = production_functions(
            (ROOT / "src/services/solar_os_expansion.c").read_text(),
            ("expansion_init_board_defaults",))
        self.assertTrue(self.drivers["spd2010"].early)
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define SOLAR_OS_EXPANSION_CATEGORY_DISPLAY 1
#define SOLAR_OS_BOARD_CAP_EXPANSION_GPIO 1
#define SOLAR_OS_BOARD_CAP_PSRAM 2
#define SOLAR_OS_EXPANSION_ORIGIN_BOARD 1
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT 3
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr,...) do { int err_ = (expr); if (err_) return err_; } while (0)
typedef struct {
    const char *name, *summary;
    int category, required_capabilities;
    bool early;
    const void *binding_specs;
    size_t binding_spec_count;
    esp_err_t (*attach)(const char *, const void *, size_t);
    esp_err_t (*detach)(const char *);
} solar_os_expansion_driver_t;
typedef struct { const char *driver, *name; const void *bindings; size_t binding_count; bool detachable; } solar_os_expansion_default_device_t;
static const int specs[1] = {0};
static bool power_ready, display_ready, service_ready, touch_ready;
static int order;
static esp_err_t attach(const char *name, const void *bindings, size_t count) {
    (void)bindings; (void)count; assert(!strcmp(name, "display0"));
    assert(power_ready && order++ == 1); display_ready = true; return ESP_OK;
}
static esp_err_t detach(const char *name) { (void)name; return ESP_OK; }
''' + descriptor + r'''
static const solar_os_expansion_driver_t power = {.name = "pca9535", .early = true};
static const solar_os_expansion_driver_t touch = {.name = "spd2010-touch"};
static const solar_os_expansion_default_device_t board_default_devices[] = {
    {.driver = "pca9535", .name = "gpiox0"},
    {.driver = "spd2010", .name = "display0"},
    {.driver = "spd2010-touch", .name = "touch0"},
};
static bool board_device_detached[3];
static esp_err_t ensure_devices_mutex(void) { return ESP_OK; }
static esp_err_t solar_os_resources_init(void) { return ESP_OK; }
static esp_err_t solar_os_buses_init(void) { return ESP_OK; }
static bool devices_lock_take(void) { return true; }
static void devices_lock_give(void) { }
static const solar_os_expansion_driver_t *find_driver(const char *name) {
    if (!strcmp(name, power.name)) return &power;
    if (!strcmp(name, touch.name)) return &touch;
    assert(!strcmp(name, "spd2010")); return &solar_os_spd2010_expansion_driver;
}
static bool expansion_device_exists(const char *name) {
    return !strcmp(name, "gpiox0") ? power_ready : !strcmp(name, "display0") ? display_ready : touch_ready;
}
static esp_err_t expansion_attach(const char *driver, const char *name, const void *bindings,
    size_t count, int origin, bool automatic, bool detachable, bool defaults) {
    assert(origin == 1 && automatic && !detachable && defaults);
    if (!strcmp(driver, "pca9535")) {
        assert(order++ == 0); power_ready = true;
    } else if (!strcmp(driver, "spd2010")) {
        return solar_os_spd2010_expansion_driver.attach(name, bindings, count);
    } else {
        assert(service_ready && display_ready && order++ == 2); touch_ready = true;
    }
    return ESP_OK;
}
''' + dispatcher + r'''
int main(void) {
    assert(expansion_init_board_defaults(true) == ESP_OK);
    assert(power_ready && display_ready && !touch_ready);
    service_ready = true;
    assert(expansion_init_board_defaults(false) == ESP_OK && touch_ready);
    assert(order == 3);
}
''')

    def test_wheel_push_debounce_held_start_read_errors_and_poll_rate(self):
        source = (ROOT / "src/services/solar_os_rotary_encoder.c").read_text()
        function = production_functions(source, ("rotary_poll_button",))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define ROTARY_BUTTON_POLL_MS 10U
#define ROTARY_BUTTON_DEBOUNCE_MS 25U
#define SOLAR_OS_KEY_ENTER 13
typedef struct { char controller[16]; uint8_t line; } solar_os_gpio_line_ref_t;
typedef struct {
    bool have_button;
    solar_os_gpio_line_ref_t button;
    bool button_raw, button_pressed, button_armed;
    uint32_t button_changed_ms, button_polled_ms, button_presses, button_errors, dropped;
    int input_source;
} solar_os_rotary_device_t;
static bool level = true;
static int reads, enters;
static esp_err_t read_error, write_error;
static esp_err_t solar_os_gpio_line_read(const solar_os_gpio_line_ref_t *line, bool *out) {
    assert(!strcmp(line->controller, "gpiox0") && line->line == 3);
    reads++; *out = level; return read_error;
}
static esp_err_t solar_os_input_write_char(int source, char key) {
    assert(source == 7 && key == 13);
    if (!write_error) enters++;
    return write_error;
}
''' + function + r'''
int main(void) {
    solar_os_rotary_device_t d = {.input_source = 7, .button = {.controller = "gpiox0", .line = 3}};
    rotary_poll_button(&d, 10); assert(reads == 0);
    d.have_button = true;
    for (uint32_t ms = 0; ms < 10; ms++) rotary_poll_button(&d, ms);
    assert(reads == 0);
    level = false; rotary_poll_button(&d, 10);
    level = true; rotary_poll_button(&d, 20);  /* bounce */
    level = false; rotary_poll_button(&d, 30);
    rotary_poll_button(&d, 50); assert(!d.button_pressed);
    rotary_poll_button(&d, 60); assert(d.button_pressed && d.button_armed && enters == 0);
    rotary_poll_button(&d, 80); assert(enters == 0);  /* no held-key repeat */
    level = true; rotary_poll_button(&d, 90);
    read_error = ESP_FAIL; rotary_poll_button(&d, 120);
    assert(d.button_pressed && d.button_errors == 1 && enters == 0);
    read_error = ESP_OK; rotary_poll_button(&d, 130);
    assert(enters == 1 && d.button_presses == 1 && !d.button_armed);
    rotary_poll_button(&d, 140); assert(enters == 1);
    /* A button held at startup must be released before a new click is armed. */
    d.button_raw = d.button_pressed = true;
    level = true; rotary_poll_button(&d, 150); rotary_poll_button(&d, 180);
    assert(enters == 1);
    level = false; rotary_poll_button(&d, 190); rotary_poll_button(&d, 220);
    level = true; rotary_poll_button(&d, 230);
    write_error = ESP_FAIL; rotary_poll_button(&d, 260);
    assert(d.dropped == 1 && enters == 1 && !d.button_armed);
    /* Tick wrap does not stall polling or the mechanical debounce. */
    d.button_polled_ms = UINT32_MAX - 5;
    d.button_raw = true; d.button_changed_ms = UINT32_MAX - 20;
    level = false; rotary_poll_button(&d, 5);
    assert(d.button_pressed && d.button_armed);
    return 0;
}
''', "-fsanitize=address,undefined", "-no-pie")


if __name__ == "__main__":
    unittest.main()
