from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


def run_c(source):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'test.c'
        path.write_text(source)
        binary = Path(directory) / 'test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'tests/host/usb_stubs'),
                        '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'src/drivers'),
                        str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)


class UsbPinModesTest(unittest.TestCase):
    def test_serial_jtag_stops_before_releasing_pads_and_retries_cleanup(self):
        source = (ROOT / 'src/drivers/cdc_port.c').read_text()
        functions = production_functions(source, ('cdc_port_init', 'uninstall_on_driver_core', 'cdc_port_deinit'))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "cdc_port.h"
#include "usb_phy_port.h"
static bool ready, installed, fail_pins;
static int driver_core, core, resets, installs, ipcs, pull_restores;
static esp_err_t stop_error;
typedef struct { uint32_t tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
static bool usb_serial_jtag_is_driver_installed(void) { return installed; }
static int xPortGetCoreID(void) { return core; }
static void usb_serial_jtag_ll_phy_disable_pull_override(void) { pull_restores++; }
static esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{ assert(config->tx_buffer_size && config->rx_buffer_size && !installed); installed = true; installs++; return ESP_OK; }
static esp_err_t usb_serial_jtag_driver_uninstall(void)
{ if (stop_error) return stop_error; installed = false; return ESP_OK; }
static esp_err_t esp_ipc_call_blocking(int target, void (*fn)(void *), void *arg)
{ assert(target == driver_core); ipcs++; fn(arg); return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(!installed);
    assert(config->pin_bit_mask == ((1ULL << 19) | (1ULL << 20)));
    assert(config->mode == GPIO_MODE_DISABLE && !config->pull_up_en && !config->pull_down_en && !config->intr_type);
    resets++;
    return fail_pins ? ESP_FAIL : ESP_OK;
}
''' + functions + r'''
int main(void)
{
    cdc_port_config_t config = {.rx_buffer_size = 4096, .tx_buffer_size = 2048};
    fail_pins = true;
    assert(cdc_port_init(&config) == ESP_FAIL && !installed && !installs);
    fail_pins = false; core = 1;
    assert(cdc_port_init(&config) == ESP_OK && ready && installed && driver_core == 1 && pull_restores == 1);
    core = 0; stop_error = ESP_FAIL;
    int previous = resets;
    assert(cdc_port_deinit() == ESP_FAIL && ready && installed && resets == previous);
    stop_error = ESP_OK; fail_pins = true;
    assert(cdc_port_deinit() == ESP_FAIL && !ready && !installed && resets == previous + 1);
    assert(ipcs == 2);
    fail_pins = false;
    assert(cdc_port_deinit() == ESP_OK && resets == previous + 2);
    assert(cdc_port_init(&config) == ESP_OK && ready && installed && pull_restores == 2);
    assert(cdc_port_deinit() == ESP_OK && !ready && !installed);
}
''')

    def test_cached_adc_calibration_does_not_skip_pin_reconfiguration(self):
        source = (ROOT / 'src/drivers/adc_port.c').read_text()
        function = production_functions(source, ('adc_port_configure_pin',))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include "esp_err.h"
typedef int gpio_num_t, adc_atten_t, adc_bitwidth_t, adc_unit_t, adc_channel_t;
typedef struct { int atten, bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { bool configured; int atten, bitwidth; } adc_port_channel_state_t;
static adc_port_channel_state_t adc_channels[2][11];
static int adc_units[2] = {1, 2}, adc_mutex, configs, calibrations;
static bool analog_mux;
static esp_err_t config_error;
#define ADC_PORT_CHANNEL_COUNT 11
#define portMAX_DELAY 0
#define TAG "test"
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t e = (expr); if (e) return e; } while (0)
static bool adc_port_is_adc_capable(int pin, int *unit, int *channel)
{ assert(pin == 19); *unit = 1; *channel = 8; return true; }
static int adc_unit_index(int unit) { return unit; }
static esp_err_t adc_port_ensure_init(void) { return ESP_OK; }
static esp_err_t adc_port_ensure_unit_locked(int unit) { assert(unit == 1); return ESP_OK; }
static void xSemaphoreTake(int mutex, int timeout) { (void)mutex; (void)timeout; }
static void xSemaphoreGive(int mutex) { (void)mutex; }
static esp_err_t adc_oneshot_config_channel(int unit, int channel, const adc_oneshot_chan_cfg_t *config)
{ assert(unit == 2 && channel == 8 && config->atten == 12 && config->bitwidth == 12); configs++; if (!config_error) analog_mux = true; return config_error; }
static esp_err_t adc_port_init_calibration_locked(int unit, int channel, int atten, int bits, adc_port_channel_state_t *state)
{ (void)unit; (void)channel; (void)atten; (void)bits; (void)state; calibrations++; return ESP_OK; }
''' + function + r'''
int main(void)
{
    assert(adc_port_configure_pin(19, 12, 12) == ESP_OK && analog_mux);
    assert(configs == 1 && calibrations == 1);
    analog_mux = false; /* USB or digital GPIO owned this pin in between. */
    assert(adc_port_configure_pin(19, 12, 12) == ESP_OK && analog_mux);
    assert(configs == 2 && calibrations == 1);
    config_error = ESP_FAIL;
    assert(adc_port_configure_pin(19, 12, 12) == ESP_FAIL && configs == 3);
    config_error = ESP_OK;
    assert(adc_port_configure_pin(19, 12, 12) == ESP_OK && configs == 4 && calibrations == 1);
}
''')


if __name__ == '__main__':
    unittest.main()
