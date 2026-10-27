from pathlib import Path
import re
import subprocess
import tempfile
import tomllib
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class StickyBoardTest(unittest.TestCase):
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
        self.assertEqual((buses["i2c0"]["sda"], buses["i2c0"]["scl"]), (1, 0))
        self.assertEqual((buses["i2c1"]["sda"], buses["i2c1"]["scl"]), (3, 2))
        self.assertEqual(devices["touch0"]["bindings"]["i2c"], "i2c1")
        for name in ("rtc0", "environment0", "battery0"):
            self.assertEqual(devices[name]["bindings"]["i2c"], "i2c0")
        for name in ("display0", "storage0"):
            binding = devices[name]["bindings"]
            self.assertEqual(binding["spi"], "spi0")
            self.assertIn(binding["cs"], buses["spi0"]["cs"])
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
static const int expected_pins[] = {45, 46, 47, 10, 42};
static esp_err_t record(int operation, int pin) {
    assert(operation == calls % 5);
    assert(pin == expected_pins[calls / 5]);
    return calls++ == fail_at ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_hold_dis(int pin) { return record(0, pin); }
esp_err_t gpio_reset_pin(int pin) { return record(1, pin); }
esp_err_t gpio_sleep_sel_dis(int pin) { return record(2, pin); }
esp_err_t gpio_set_level(int pin, int level) { assert(level == 1); return record(3, pin); }
esp_err_t gpio_set_direction(int pin, int mode) { assert(mode == GPIO_MODE_OUTPUT); return record(4, pin); }
'''
        source += function + r'''
int main(void) {
    assert(solar_os_board_boot_init() == ESP_OK);
#ifdef SOLAR_OS_BOARD_BOOT_OUTPUTS
    assert(calls == 25);
    for (int step = 0; step < 25; step++) {
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
