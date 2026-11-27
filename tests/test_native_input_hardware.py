from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host"
SERVICES = ROOT / "src/services"


class NativeInputHardwareTest(unittest.TestCase):
    def test_shared_dpad_samples_and_calibration(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / "adc_port.h").write_text(r"""
#pragma once
#include "driver/gpio.h"
#include "esp_err.h"
#define ADC_ATTEN_DB_12 12
#define ADC_BITWIDTH_12 12
#include <stdbool.h>
typedef int adc_unit_t, adc_channel_t;
typedef struct { int raw; } adc_port_sample_t;
bool adc_port_is_adc_capable(gpio_num_t pin, adc_unit_t *unit, adc_channel_t *channel);
esp_err_t adc_port_configure_pin(gpio_num_t pin, int atten, int bits);
esp_err_t adc_port_read(gpio_num_t pin, adc_port_sample_t *sample);
""".lstrip())
            (tmp / "solar_os_board.h").write_text(r"""
#pragma once
#define SOLAR_OS_BOARD_HAS_ADC_DPAD 1
#define SOLAR_OS_BOARD_ADC_DPAD_AXES { \
    {.pin=4, .name="horizontal", .mid_key=1, .high_key=2, \
     .idle_max=100, .mid_min=1000, .mid_max=1900, .high_min=3000}, \
    {.pin=5, .name="vertical", .mid_key=3, .high_key=4, \
     .idle_max=120, .mid_min=1000, .mid_max=1900, .high_min=3000} }
""".lstrip())
            binary = tmp / "dpad"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1", "-DSOLAR_OS_PACKAGE_SERVICE_ADC_DPAD=1",
                "-I", directory, "-I", str(HOST / "native_abi_stubs"),
                "-I", str(HOST / "gpio_controller_stubs"), "-I", str(HOST),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src"), "-I", str(SERVICES),
                str(HOST / "native_input_hardware_test.c"),
                str(SERVICES / "solar_os_adc_dpad.c"),
                str(SERVICES / "solar_os_native_input_hardware.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)
