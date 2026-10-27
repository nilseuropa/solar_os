from pathlib import Path
import tomllib
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class SdSleepTest(unittest.TestCase):
    def test_shared_power_and_chip_selects_survive_light_sleep(self):
        board = tomllib.loads((ROOT / "boards/manifests/t_deck_plus.toml").read_text())
        defines = board["defines"]
        power = defines["SOLAR_OS_BOARD_PIN_SD_POWER"]
        self.assertEqual(power, defines["SOLAR_OS_BOARD_PIN_LCD_POWER"])
        bus = next(bus for bus in board["buses"] if bus["name"] == "spi0")
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t e = (expr); if (e != ESP_OK) return e; } while (0)
#define GPIO_NUM_10 10
#define GPIO_NUM_NC -1
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define SPI_DMA_CH_AUTO 3
#define pdMS_TO_TICKS(ms) (ms)
typedef int gpio_num_t, spi_host_device_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct { int dc_pin, reset_pin, backlight_pin, power_pin; bool power_active_high; } config_t;
typedef struct { config_t config; } tft_ili9341_t;
typedef struct {
    int host, sclk_pin, mosi_pin, miso_pin;
    size_t max_transfer_size, cs_count;
    struct { int pin; } cs[3];
} solar_os_bus_spi_config_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
static bool output[64], sleep_switch[64];
static unsigned level[64], config_calls, bus_starts;
static int fail_retention = -1;
static bool storage_power_configured;
static bool gpio_valid(int pin) { return pin >= 0 && pin < 64; }
static uint64_t gpio_pin_mask(int pin) { return gpio_valid(pin) ? 1ULL << pin : 0; }
static esp_err_t gpio_config(const gpio_config_t *config) {
    config_calls++;
    assert(config->mode == GPIO_MODE_OUTPUT);
    for (int pin = 0; pin < 64; pin++)
        if (config->pin_bit_mask & (1ULL << pin)) output[pin] = true;
    return ESP_OK;
}
static esp_err_t gpio_set_level(int pin, unsigned value) {
    assert(gpio_valid(pin)); level[pin] = value; return ESP_OK;
}
static esp_err_t gpio_sleep_sel_dis(int pin) {
    assert(gpio_valid(pin));
    if (pin == fail_retention) return ESP_FAIL;
    sleep_switch[pin] = false; return ESP_OK;
}
static esp_err_t gpio_hold_dis(int pin) { assert(gpio_valid(pin)); return ESP_OK; }
static void vTaskDelay(unsigned ticks) { (void)ticks; }
static esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma) {
    assert(host == 1 && bus->sclk_io_num == 40 && dma == SPI_DMA_CH_AUTO);
    bus_starts++; return ESP_OK;
}
'''
        source += f"\n#define SOLAR_OS_BOARD_PIN_SD_POWER {power}\n"
        source += production_functions(
            (ROOT / "src/drivers/tft_ili9341.c").read_text(),
            ("ili9341_configure_control_pins",))
        source += production_functions(
            (ROOT / "src/board/solar_os_board_storage_sd.c").read_text(),
            ("storage_power_on",))
        source += production_functions(
            (ROOT / "src/drivers/routed_spi_bus.c").read_text(),
            ("config_valid", "solar_os_routed_spi_start"))
        source += "\nstatic const int selects[] = {" + ",".join(map(str, bus["cs"])) + "};\n"
        source += r'''
static void reset(void) {
    memset(output, 0, sizeof(output));
    memset(level, 0, sizeof(level));
    /* ESP-IDF's GPIO reset workaround isolates pins during light sleep. */
    memset(sleep_switch, 1, sizeof(sleep_switch));
    storage_power_configured = false;
    fail_retention = -1;
}
static bool driven_during_sleep(int pin, unsigned value) {
    return output[pin] && !sleep_switch[pin] && level[pin] == value;
}
int main(void) {
    reset();
    tft_ili9341_t display = {.config = {11, -1, 42, 10, true}};
    solar_os_bus_spi_config_t bus = {.host = 1, .sclk_pin = 40, .mosi_pin = 41,
        .miso_pin = 38, .max_transfer_size = 4096, .cs_count = 3};
    for (size_t i = 0; i < 3; i++) bus.cs[i].pin = selects[i];
    bool initialized = false;
    assert(ili9341_configure_control_pins(&display) == ESP_OK);
    assert(solar_os_routed_spi_start(&bus, false, &initialized) == ESP_OK && initialized);
    for (unsigned cycle = 0; cycle < 5; cycle++) {
        /* Retain the card's power and SPI session while FAT stays mounted. */
        assert(driven_during_sleep(10, 1));
        for (size_t i = 0; i < 3; i++) assert(driven_during_sleep(selects[i], 1));
        /* The screen backlight can still turn off in sleep. */
        assert(sleep_switch[42]);
        assert(ili9341_configure_control_pins(&display) == ESP_OK);
    }
    /* Retention keeps the active configuration, rather than freezing a level:
     * explicit shutdown can still cut the shared rail. */
    assert(gpio_set_level(10, 0) == ESP_OK && driven_during_sleep(10, 0));
    display.config.power_active_high = false;
    assert(ili9341_configure_control_pins(&display) == ESP_OK);
    assert(driven_during_sleep(10, 0));
    display.config.power_pin = -1;
    assert(ili9341_configure_control_pins(&display) == ESP_OK);

    /* A storage-owned rail needs the same protection without a display. */
    reset();
    assert(storage_power_on() == ESP_OK);
    assert(driven_during_sleep(10, SOLAR_OS_BOARD_SD_POWER_ACTIVE_LEVEL));
    unsigned before = config_calls;
    assert(storage_power_on() == ESP_OK && config_calls == before);

    /* Failed retention must not be reported as a successfully ready device. */
    fail_retention = 10;
    display.config.power_pin = 10;
    assert(ili9341_configure_control_pins(&display) == ESP_FAIL);
    assert(storage_power_on() == ESP_FAIL);
    fail_retention = selects[1];
    before = bus_starts;
    assert(solar_os_routed_spi_start(&bus, false, &initialized) == ESP_FAIL);
    assert(bus_starts == before);
}
'''
        for active in (0, 1):
            with self.subTest(storage_active_level=active):
                run_c(source, f"-DSOLAR_OS_BOARD_SD_POWER_ACTIVE_LEVEL={active}")


if __name__ == "__main__":
    unittest.main()
