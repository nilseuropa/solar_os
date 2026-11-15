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
        self.assertIn('#define SOLAR_OS_BOARD_KEY_GPIO_LINE "gpiox0:3"', header)
        self.assertIn("key", self.board["build"]["capabilities"])
        main = (ROOT / "src/main.c").read_text().split("void app_main(void)", 1)[1]
        self.assertLess(main.index("solar_os_expansion_init_early()"), main.index("key_button_init()"))
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

    def test_wheel_direction_matches_mounted_phase_order(self):
        wheel = next(d for d in self.board["devices"] if d["name"] == "wheel0")
        self.assertNotIn("button", wheel["bindings"])
        source = (ROOT / "src/services/solar_os_rotary_encoder.c").read_text()
        definitions = source[source.index("#define ROTARY_TASK_STACK"):source.index("typedef struct {")]
        worker = production_functions(source, ("rotary_worker",))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_key_codes.h"
#define tskIDLE_PRIORITY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0
#define portTICK_PERIOD_MS 1
''' + definitions + r'''
typedef struct {
    bool stop_requested, worker_done, have_button;
    uint8_t state;
    int edge_queue, input_source;
    unsigned clockwise, counter_clockwise, dropped;
} solar_os_rotary_device_t;
static solar_os_rotary_device_t device;
static uint8_t edges[10], keys[2];
static unsigned position, written;
static int xQueueReceive(int queue, uint8_t *state, int timeout) {
    (void)queue; (void)timeout;
    if (position == sizeof(edges)) { device.stop_requested = true; return 0; }
    *state = edges[position++]; return pdTRUE;
}
static esp_err_t solar_os_input_write_char(int source, char key) {
    (void)source; assert(written < 2); keys[written++] = (uint8_t)key; return ESP_OK;
}
static uint32_t xTaskGetTickCount(void) { return 0; }
static void rotary_poll_button(solar_os_rotary_device_t *d, uint32_t now) { (void)now; assert(!d->have_button); }
static void solar_os_task_delete_internal(void *task) { assert(task == NULL); }
''' + worker + r'''
int main(void) {
    /* Physical GPIO41/42 states for an Up detent followed by a Down detent. */
    const uint8_t physical[] = {3,1,0,2,3, 3,2,0,1,3};
''' + f'    const unsigned pin_a = {wheel["bindings"]["a"]}, pin_b = {wheel["bindings"]["b"]};\n' + r'''
    for (unsigned i = 0; i < sizeof(edges); i++) {
        unsigned a = (physical[i] >> (pin_a == 41 ? 0 : 1)) & 1;
        unsigned b = (physical[i] >> (pin_b == 41 ? 0 : 1)) & 1;
        edges[i] = (uint8_t)((b << 1) | a);
    }
    rotary_worker(&device);
    assert(device.worker_done && written == 2 && !device.dropped);
    assert(keys[0] == SOLAR_OS_KEY_UP && keys[1] == SOLAR_OS_KEY_DOWN);
}
''', "-I", str(ROOT / "include"))

    def test_expander_key_uses_shared_short_long_policy_and_debounce(self):
        main = (ROOT / "src/main.c").read_text()
        functions = production_functions(main, (
            "key_level_is_pressed", "key_button_is_pressed", "key_rtc_is_pressed",
            "key_button_configure_gpio", "key_prepare_rtc_wakeup",
            "handle_key_pairing", "handle_key_input", "handle_key_action",
            "handle_key_short_press", "key_button_init", "poll_key_button"))
        defaults = production_functions((ROOT / "src/services/solar_os_power.c").read_text(),
                                        ("default_key_action",))
        defines = self.board["defines"]
        configuration = "\n".join(f"#define {key} {defines[key]}" for key in (
            "SOLAR_OS_BOARD_KEY_GPIO_LINE", "SOLAR_OS_BOARD_KEY_SHORT_ACTION",
            "SOLAR_OS_BOARD_KEY_LONG_ACTION"))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_gpio_controller.h"
#include "solar_os_power.h"
#include "solar_os_keys.h"
#define SOLAR_OS_PACKAGE_SERVICE_BLE 1
#define SOLAR_OS_BOARD_CAP_KEY 1
#define SOLAR_OS_BOARD_CAP_BLE 2
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 0
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0
#define KEY_LINE_POLL_MS 10U
#define KEY_LINE_DEBOUNCE_MS 25U
#define KEY_SHORT_PRESS_MIN_MS 30U
#define KEY_LONG_PRESS_MS 1200U
#define SOLAR_OS_INPUT_SOURCE_BUTTONS 1
#define SOLAR_OS_LOGI(...) ((void)0)
#define SOLAR_OS_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(err) assert((err) == ESP_OK)
''' + configuration + r'''
static solar_os_gpio_line_ref_t key_line;
static bool key_line_ready, key_line_raw, key_line_pressed;
static uint32_t key_line_polled_ms, key_line_changed_ms;
static bool down = true, key_irq_pending, key_interrupt_ready, key_pressed;
static bool key_ignore_until_released, key_long_press_fired;
static uint32_t now, key_pressed_ms, last_status_update_ms;
static int key_input_source;
static unsigned reads, chars, pairs, forgets, wakes, sleeps;
static solar_os_power_key_action_t key_release_action;
static solar_os_power_status_t status;
static esp_err_t read_error;
static uint32_t millis_u32(void) { return now; }
static bool board_has(int cap) { (void)cap; return true; }
bool solar_os_gpio_line_parse(const char *name, solar_os_gpio_line_ref_t *line) {
    assert(!strcmp(name, "gpiox0:3")); strcpy(line->controller, "gpiox0"); line->line = 3; return true;
}
esp_err_t solar_os_gpio_line_configure(const solar_os_gpio_line_ref_t *line,
    solar_os_gpio_line_mode_t mode, solar_os_gpio_line_pull_t pull) {
    assert(!strcmp(line->controller, "gpiox0") && line->line == 3);
    assert(mode == SOLAR_OS_GPIO_LINE_MODE_INPUT && pull == SOLAR_OS_GPIO_LINE_PULL_NONE);
    return ESP_OK;
}
esp_err_t solar_os_gpio_line_read(const solar_os_gpio_line_ref_t *line, bool *level) {
    assert(!strcmp(line->controller, "gpiox0") && line->line == 3); reads++;
    *level = !down; return read_error;
}
static esp_err_t solar_os_input_key_source_open(const char *name, int type, int *out) {
    assert(!strcmp(name, "system-key") && type == SOLAR_OS_INPUT_SOURCE_BUTTONS); *out = 1; return ESP_OK;
}
static esp_err_t solar_os_input_write_char(int source, char key) {
    assert(source == 1 && key == SOLAR_OS_KEY_ENTER); chars++; return ESP_OK;
}
void solar_os_power_get_status(solar_os_power_status_t *out) { *out = status; }
void solar_os_power_note_activity(uint32_t ms) { status.last_activity_ms = ms; }
static bool solar_os_ble_keyboard_enabled_for_current_boot(void) { return true; }
static esp_err_t solar_os_ble_keyboard_forget(void) { forgets++; return ESP_OK; }
static esp_err_t solar_os_ble_keyboard_start_pairing(void) { pairs++; return ESP_OK; }
esp_err_t solar_os_shutdown_request(void) { assert(false); return ESP_OK; }
static void update_status(void) { }
static void draw_terminal_if_needed(void) { }
static void enter_light_sleep(const char *reason) { (void)reason; sleeps++; }
static void enter_deep_sleep(const char *reason) { (void)reason; assert(false); }
static void enter_suspend(const char *reason) { (void)reason; status.suspend_active = true; }
static void exit_suspend(const char *reason) { (void)reason; status.suspend_active = false; wakes++; }
''' + defaults + functions + r'''
static void poll(bool pressed, uint32_t elapsed) { down = pressed; now += elapsed; poll_key_button(); }
static void press(void) { poll(true, 10); poll(true, 30); }
static void release(void) { poll(false, 10); poll(false, 30); }
int main(void) {
    status.key_action = default_key_action(false);
    status.key_long_action = default_key_action(true);
    assert(status.key_action == SOLAR_OS_POWER_KEY_ACTION_ENTER);
    assert(status.key_long_action == SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR);
    /* A wheel held during startup is consumed, including a long hold. */
    key_button_init(); assert(key_ignore_until_released && reads == 1);
    poll(true, 5); assert(reads == 1);  /* Bound I2C polling frequency. */
    poll(true, 5000); assert(!pairs && !chars);
    release(); assert(!key_ignore_until_released && !chars);
    assert(!key_rtc_is_pressed() && key_prepare_rtc_wakeup() == ESP_ERR_NOT_SUPPORTED);
    /* Contact bounce cannot send Enter. */
    poll(true, 10); poll(false, 10); poll(true, 10);
    poll(true, 20); assert(!key_pressed && !chars);
    poll(true, 10); assert(key_pressed);
    poll(true, 50); release(); assert(chars == 1 && !pairs);
    /* A hold pairs once, and releasing it cannot also send Enter. */
    press(); poll(true, 1190); assert(!pairs);
    poll(true, 10); assert(pairs == 1 && forgets == 1);
    poll(true, 5000); release(); assert(pairs == 1 && chars == 1);
    /* Read failures preserve the held state instead of fabricating release. */
    press(); poll(false, 100); read_error = ESP_FAIL;
    poll(false, 30); assert(key_pressed && chars == 1);
    read_error = ESP_OK; poll(false, 10); assert(!key_pressed && chars == 2);
    /* User KEY settings still control the push button. */
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_SLEEP;
    press(); poll(true, 50); release(); assert(sleeps == 1 && chars == 2);
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_ENTER;
    status.suspend_active = true;
    press(); assert(!status.suspend_active && wakes == 1);
    poll(true, 2000); release(); assert(chars == 2 && pairs == 1);
    /* Millisecond wrap preserves polling, debounce, and short-press timing. */
    now = key_line_polled_ms = UINT32_MAX - 5U;
    press(); poll(true, 50); release(); assert(chars == 3 && pairs == 1);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"),
              "-fsanitize=address,undefined", "-no-pie")

    def test_optional_encoder_push_debounce_held_start_read_errors_and_poll_rate(self):
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
