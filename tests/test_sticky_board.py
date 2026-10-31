from pathlib import Path
import re
import subprocess
import tempfile
import tomllib
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class StickyBoardTest(unittest.TestCase):
    def test_touch_landscape_axes_and_scale(self):
        board = tomllib.loads((ROOT / "boards/manifests/seeed_reterminal_sticky.toml").read_text())
        touch = next(device for device in board["devices"] if device["name"] == "touch0")
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_input.h"
#include "gt911.h"
#define SOLAR_OS_DISPLAY_PRIMARY_TARGET "display0"
typedef struct {
    bool active, pressed, home_pressed, swap_xy;
    uint8_t rotation, pointer_id, home_key;
    uint16_t width, height;
    int16_t x, y;
    solar_os_input_source_t source;
} gt911_device_t;
static gt911_sample_t sample;
static solar_os_input_pointer_event_t event;
static unsigned events;
static size_t strlcpy(char *out, const char *in, size_t size) {
    size_t length=strlen(in);assert(length<size);memcpy(out,in,length+1);return length;
}
esp_err_t gt911_read(gt911_sample_t *out) { *out=sample; return ESP_OK; }
esp_err_t solar_os_input_write_pointer(solar_os_input_source_t id, const solar_os_input_pointer_event_t *out) {
    assert(id==1);event=*out;events++;return ESP_OK;
}
esp_err_t solar_os_input_write_key_tap(solar_os_input_source_t id, uint16_t physical, uint16_t usage, uint8_t key, uint8_t modifiers) {
    (void)id;(void)physical;(void)usage;(void)key;(void)modifiers;assert(false);return ESP_FAIL;
}
'''
        source += production_functions((ROOT / "src/services/solar_os_gt911.c").read_text(), ("poll_device",))
        source += production_functions((ROOT / "src/services/solar_os_input.c").read_text(),
            ("solar_os_input_pointer_apply_orientation", "solar_os_input_pointer_apply_display_transform"))
        source += r'''
static solar_os_input_pointer_event_t point(gt911_device_t *device, unsigned x, unsigned y, unsigned scale) {
    sample=(gt911_sample_t){.valid=true,.touched=true,.x=x,.y=y};
    unsigned before=events;poll_device(device);assert(events==before+1);
    solar_os_input_pointer_event_t result=event;
    assert(!strcmp(result.target,"display0"));
    assert(solar_os_input_pointer_apply_display_transform(&result,800,480,0,scale)==ESP_OK);
    return result;
}
int main(void) {
    for(unsigned scale=1;scale<=2;scale++) {
        gt911_device_t device={.active=true,.source=1,.width=800,.height=480,
            .rotation=TOUCH_ROTATION,.swap_xy=TOUCH_SWAP_XY};
        solar_os_input_pointer_event_t right=point(&device,240,100,scale);
        solar_os_input_pointer_event_t left=point(&device,240,700,scale);
        assert(left.x<right.x && left.y==right.y && left.delta_x<0 && left.delta_y==0);
        solar_os_input_pointer_event_t top=point(&device,400,400,scale);
        solar_os_input_pointer_event_t bottom=point(&device,100,400,scale);
        assert(bottom.x==top.x && bottom.y>top.y && bottom.delta_x==0 && bottom.delta_y>0);
        solar_os_input_pointer_event_t corner=point(&device,0,0,scale);
        assert(corner.x==(int)(799/scale) && corner.y==(int)(479/scale));
        corner=point(&device,479,799,scale);
        assert(corner.x==0 && corner.y==0);
        unsigned before=events;sample.x=480;poll_device(&device);assert(events==before);
        sample.x=479;sample.y=800;poll_device(&device);assert(events==before);
        sample=(gt911_sample_t){.valid=true};poll_device(&device);
        assert(events==before+1 && event.action==SOLAR_OS_INPUT_POINTER_RELEASE);
    }
    gt911_device_t generic={.active=true,.source=1,.width=800,.height=480};
    solar_os_input_pointer_event_t unchanged=point(&generic,799,479,1);
    assert(unchanged.x==799 && unchanged.y==479);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "touch.c"), Path(tmp, "touch")
            path.write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", f'-DTOUCH_ROTATION={touch["bindings"]["rotation"]}',
                f'-DTOUCH_SWAP_XY={touch["bindings"].get("swap_xy", 0)}',
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/drivers"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_button_holds_pair_or_exit_and_consume_release(self):
        board = tomllib.loads((ROOT / "boards/manifests/seeed_reterminal_sticky.toml").read_text())
        source = (ROOT / "tests/host/buttons_test.c").read_text().split("int main(void)", 1)[0]
        source += '#include "solar_os_board.h"\n'
        source += r'''
static unsigned pairings, exits;
static solar_os_input_source_t key_source;
static void handle_key_pairing(void) { pairings++; }
static void handle_key_input(uint8_t key) {
    assert(key == SOLAR_OS_KEY_APP_EXIT); exits++;
    assert(solar_os_input_write_char(key_source, (char)key) == ESP_OK);
}
'''
        source += production_functions((ROOT / "src/main.c").read_text(), ("board_button_long_press",))
        source += r'''
static void poll_after(unsigned ms) { now_us += ms * 1000ULL; solar_os_buttons_poll(); }
static void press(int pin) { levels[pin] = 0; poll_after(1); poll_after(25); }
static void release(int pin) { levels[pin] = 1; poll_after(1); poll_after(25); }
static void expect_arrow(uint8_t key) {
    solar_os_input_key_event_t events[2];
    assert(solar_os_input_read_events(events, 2) == 2);
    assert(events[0].key == key && events[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(events[1].key == key && events[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
}
int main(void) {
    assert(solar_os_input_init() == ESP_OK);
    assert(solar_os_input_key_source_open("key", SOLAR_OS_INPUT_SOURCE_BUTTONS, &key_source) == ESP_OK);
    levels[5] = 0; levels[6] = 1;
    solar_os_buttons_set_long_press_handler(board_button_long_press, NULL);
    assert(solar_os_buttons_init() == ESP_OK);
    poll_after(5000); expect_no_events(); assert(pairings == 0);
    release(5); expect_no_events();
    press(5); poll_after(100); release(5);
    expect_arrow(SOLAR_OS_KEY_UP); assert(pairings == 0);
    press(5); poll_after(1999); expect_no_events(); assert(pairings == 0);
    poll_after(1); expect_no_events(); assert(pairings == 1);
    poll_after(5000); assert(pairings == 1);
    release(5); expect_no_events();
    press(6); poll_after(100); release(6); expect_arrow(SOLAR_OS_KEY_DOWN);
    press(6); poll_after(1999); expect_no_events(); assert(exits == 0);
    poll_after(1); expect_arrow(SOLAR_OS_KEY_APP_EXIT); assert(exits == 1);
    poll_after(5000); expect_no_events(); assert(exits == 1);
    release(6); expect_no_events();
    press(6); poll_after(100); release(6); expect_arrow(SOLAR_OS_KEY_DOWN);
    press(5); poll_after(100); release(5); expect_arrow(SOLAR_OS_KEY_UP);
    assert(pairings == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            definitions = re.sub(r"GPIO_NUM_(\d+)", r"\1", board["defines"]["SOLAR_OS_BOARD_BUTTONS"])
            Path(tmp, "solar_os_board.h").write_text('#pragma once\n#include "solar_os_keys.h"\n'
                '#define SOLAR_OS_BOARD_HAS_BUTTONS 1\n#define SOLAR_OS_BOARD_PIN_BLE_PAIR 5\n'
                '#define SOLAR_OS_BOARD_PIN_APP_EXIT 6\n#define SOLAR_OS_BOARD_BUTTONS ' + definitions + '\n')
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

    def test_latch_stays_on_until_power_off(self):
        board = tomllib.loads((ROOT / "boards/manifests/seeed_reterminal_sticky.toml").read_text())
        source = '#include <assert.h>\n#include <stddef.h>\n#include <string.h>\n#include "solar_os_power.h"\n'
        source += 'typedef int gpio_num_t;\n#define GPIO_MODE_OUTPUT 1\n'
        for name in ("SOLAR_OS_BOARD_BOOT_OUTPUTS", "SOLAR_OS_BOARD_POWER_LATCH_PINS", "SOLAR_OS_BOARD_POWEROFF_PROVIDER"):
            value = re.sub(r"GPIO_NUM_(\d+)", r"\1", board["defines"][name])
            source += f'#define {name} {value}\n'
        source += r'''
static bool held[64], deep_hold;
static int levels[64], calls, fail_at = -1;
static solar_os_power_off_fn_t power_off;
static void *power_user;
static esp_err_t result(void) { return calls++ == fail_at ? ESP_FAIL : ESP_OK; }
esp_err_t gpio_hold_dis(int pin) { esp_err_t err=result(); if (!err) held[pin]=false; return err; }
esp_err_t gpio_hold_en(int pin) { esp_err_t err=result(); if (!err) held[pin]=true; return err; }
esp_err_t gpio_reset_pin(int pin) { assert(!held[pin]); return result(); }
esp_err_t gpio_sleep_sel_dis(int pin) { (void)pin; return result(); }
esp_err_t gpio_set_level(int pin, int level) { assert(!held[pin]); esp_err_t err=result(); if (!err) levels[pin]=level; return err; }
esp_err_t gpio_set_direction(int pin, int mode) { (void)pin; assert(mode == GPIO_MODE_OUTPUT); return result(); }
void gpio_deep_sleep_hold_en(void) { deep_hold=true; }
esp_err_t solar_os_power_register_off_provider(const char *name, solar_os_power_off_fn_t off, void *user) {
    assert(strcmp(name, "board-power") == 0 && off && user); power_off=off; power_user=user; return ESP_OK;
}
static gpio_num_t power_latch_pins[] = SOLAR_OS_BOARD_POWER_LATCH_PINS;
'''
        source += production_functions((ROOT / "src/services/solar_os_board_boot.c").read_text(),
                                       ("board_power_off", "solar_os_board_boot_init"))
        source += r'''
int main(void) {
    assert(solar_os_board_boot_init() == ESP_OK);
    assert(!deep_hold && !held[45] && !held[46] && levels[45] == 1 && levels[46] == 1);
    assert(!held[47] && !held[10] && !held[42]);
    assert(power_off(NULL) == ESP_ERR_INVALID_ARG);
    for (int step=0; step<6; step++) {
        fail_at=-1; assert(solar_os_board_boot_init() == ESP_OK);
        calls=0; fail_at=step;
        assert(power_off(power_user) == ESP_FAIL && calls == step+1);
    }
    fail_at=-1; assert(solar_os_board_boot_init() == ESP_OK);
    assert(power_off(power_user) == ESP_OK);
    assert(deep_hold && levels[45] == 0 && levels[46] == 0 && held[45] && held[46]);
    assert(solar_os_board_boot_init() == ESP_OK);
    assert(levels[45] == 1 && levels[46] == 1 && !held[45] && !held[46]);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "latch.c"), Path(tmp, "latch")
            path.write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                            "-I", str(ROOT / "include"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_power_key_defaults_and_reserved_buttons(self):
        from solaros_board_manifest import generate_header, load_board_manifest, load_driver_catalog

        board = load_board_manifest(ROOT / "boards/manifests/seeed_reterminal_sticky.toml",
                                    ROOT / "boards/manifests")
        header = generate_header(board, load_driver_catalog(ROOT / "boards/expansion_drivers.toml"))
        self.assertIn("key", board["build"]["capabilities"])
        self.assertIn("buttons", board["build"]["capabilities"])
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_PIN_BLE_PAIR"], "GPIO_NUM_5")
        self.assertNotIn("GPIO_NUM_4", board["defines"]["SOLAR_OS_BOARD_BUTTONS"])
        macros = []
        for name in ("SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD", "SOLAR_OS_BOARD_USER_GPIO_MASK"):
            macros.append(re.search(r"^#define " + name + r" .+$", header, re.M).group(0))
        for name, value in board["defines"].items():
            if name.startswith("SOLAR_OS_BOARD_KEY_"):
                macros.append(f"#define {name} {value}")
        source = '#include <assert.h>\n#include "solar_os_power.h"\n#include "solar_os_keys.h"\n'
        source += re.sub(r"GPIO_NUM_(\d+)", r"\1", "\n".join(macros)) + "\n"
        source += production_functions((ROOT / "src/services/solar_os_power.c").read_text(),
                                       ("default_key_action",))
        source += r'''
int main(void) {
    assert(default_key_action(false) == SOLAR_OS_POWER_KEY_ACTION_SLEEP);
    assert(default_key_action(true) == SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN);
    assert(!(SOLAR_OS_BOARD_USER_GPIO_MASK & (1ULL << 5)));
    assert(!(SOLAR_OS_BOARD_USER_GPIO_MASK & (1ULL << 6)));
    assert(!(SOLAR_OS_BOARD_USER_GPIO_MASK & (1ULL << 4)));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "keys.c")
            path.write_text(source)
            binary = Path(tmp, "keys")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                            "-I", str(ROOT / "include"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_sht40_measurements_reject_corruption_and_bus_errors(self):
        source = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "sht40.h"
#include "freertos/task.h"
static esp_err_t transmit_result = ESP_OK, receive_result = ESP_OK;
static unsigned command;
static uint8_t frame[] = {0x66, 0x66, 0x93, 0x80, 0x00, 0xa2};
esp_err_t solar_os_bus_i2c_transmit(const char *bus, uint8_t addr, const uint8_t *data, size_t count) {
    assert(strcmp(bus, "sensor-bus") == 0 && addr == 0x44 && count == 1);
    command = data[0]; return transmit_result;
}
esp_err_t solar_os_bus_i2c_receive(const char *bus, uint8_t addr, uint8_t *data, size_t count) {
    assert(strcmp(bus, "sensor-bus") == 0 && addr == 0x44 && count == sizeof(frame));
    memcpy(data, frame, count); return receive_result;
}
void vTaskDelay(TickType_t ticks) { assert(ticks == pdMS_TO_TICKS(10)); }
int main(void) {
    sht40_t device;
    sht40_measurement_t sample = {0};
    assert(sht40_init_device(&device, "sensor-bus", 0x44) == ESP_OK && command == 0x94);
    assert(sht40_read_measurement_device(&device, &sample) == ESP_OK && command == 0xfd);
    assert(fabsf(sample.temperature_c - 25.0f) < 0.001f);
    assert(fabsf(sample.humidity_percent - 56.501f) < 0.002f);
    frame[2] ^= 1;
    assert(sht40_read_measurement_device(&device, &sample) == ESP_ERR_INVALID_RESPONSE);
    frame[2] ^= 1;
    frame[5] ^= 1;
    assert(sht40_read_measurement_device(&device, &sample) == ESP_ERR_INVALID_RESPONSE);
    frame[5] ^= 1;
    transmit_result = ESP_ERR_TIMEOUT;
    assert(sht40_read_measurement_device(&device, &sample) == ESP_ERR_TIMEOUT);
    transmit_result = ESP_OK; receive_result = ESP_FAIL;
    assert(sht40_read_measurement_device(&device, &sample) == ESP_FAIL);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "sht40_test.c")
            path.write_text(source)
            Path(tmp, "solar_os_buses.h").write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t solar_os_bus_i2c_transmit(const char *, uint8_t, const uint8_t *, size_t);
esp_err_t solar_os_bus_i2c_receive(const char *, uint8_t, uint8_t *, size_t);
''')
            binary = Path(tmp, "sht40_test")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", tmp,
                            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/drivers"),
                            str(path), str(ROOT / "src/drivers/sht40.c"), "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_onboard_devices_share_the_correct_buses(self):
        board = tomllib.loads((ROOT / "boards/manifests/seeed_reterminal_sticky.toml").read_text())
        buses = {bus["name"]: bus for bus in board["buses"]}
        devices = {device["name"]: device for device in board["devices"]}
        self.assertEqual(devices["display0"]["bindings"]["panel"], 1)
        self.assertEqual(devices["display0"]["bindings"]["clock"], 20000)
        self.assertIn("streaming_display", board["build"]["capabilities"])
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ"], "20000000")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_DEFAULT_SCALE"], "2")
        self.assertEqual((buses["i2c0"]["sda"], buses["i2c0"]["scl"]), (1, 0))
        self.assertEqual((buses["i2c1"]["sda"], buses["i2c1"]["scl"]), (3, 2))
        self.assertEqual(devices["touch0"]["bindings"]["i2c"], "i2c1")
        for name in ("rtc0", "environment0", "battery0"):
            self.assertEqual(devices[name]["bindings"]["i2c"], "i2c0")
        for name in ("display0", "storage0"):
            binding = devices[name]["bindings"]
            self.assertEqual(binding["spi"], "spi0")
            self.assertIn(binding["cs"], buses["spi0"]["cs"])
        self.assertNotIn("power", devices["storage0"]["bindings"])
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_PIN_SD_POWER"], "GPIO_NUM_10")
        self.assertIn(".pin = GPIO_NUM_10, .level = 1", board["defines"]["SOLAR_OS_BOARD_BOOT_OUTPUTS"])
        self.assertNotIn("cdc", board["build"]["capabilities"])
        self.assertEqual((buses["uart0"]["tx"], buses["uart0"]["rx"]), (43, 44))

    def test_early_outputs_are_board_scoped_and_fail_closed(self):
        board = tomllib.loads((ROOT / "boards/manifests/seeed_reterminal_sticky.toml").read_text())
        outputs = re.sub(r"GPIO_NUM_(\d+)", r"\1", board["defines"]["SOLAR_OS_BOARD_BOOT_OUTPUTS"])
        function = production_functions((ROOT / "src/services/solar_os_board_boot.c").read_text(),
                                        ("solar_os_board_boot_init",))
        source = r'''
#include <assert.h>
#include <stddef.h>
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_MODE_OUTPUT 1
static int calls, fail_at = -1;
static const int expected_pins[] = {45, 46, 47, 10, 42, 38};
static esp_err_t record(int operation, int pin) {
    assert(operation == calls % 5);
    assert(pin == expected_pins[calls / 5]);
    return calls++ == fail_at ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_hold_dis(int pin) { return record(0, pin); }
esp_err_t gpio_reset_pin(int pin) { return record(1, pin); }
esp_err_t gpio_sleep_sel_dis(int pin) { return record(2, pin); }
esp_err_t gpio_set_level(int pin, int level) { assert(level == (pin == 38 ? 0 : 1)); return record(3, pin); }
esp_err_t gpio_set_direction(int pin, int mode) { assert(mode == GPIO_MODE_OUTPUT); return record(4, pin); }
'''
        source += function + r'''
int main(void) {
    assert(solar_os_board_boot_init() == ESP_OK);
#ifdef SOLAR_OS_BOARD_BOOT_OUTPUTS
    const int operations = (int)(sizeof(expected_pins) / sizeof(expected_pins[0])) * 5;
    assert(calls == operations);
    for (int step = 0; step < operations; step++) {
        calls = 0; fail_at = step;
        assert(solar_os_board_boot_init() == ESP_FAIL);
        assert(calls == step + 1);
    }
#else
    assert(calls == 0);
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "boot.c")
            path.write_text(source)
            for enabled in (False, True):
                with self.subTest(board_outputs=enabled):
                    binary = Path(tmp, "boot")
                    flags = [f"-DSOLAR_OS_BOARD_BOOT_OUTPUTS={outputs}"] if enabled else []
                    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                                    "-I", str(ROOT / "tests/host"), str(path), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)
