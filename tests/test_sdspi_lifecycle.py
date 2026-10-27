from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class SdspiLifecycleTest(unittest.TestCase):
    def test_cached_card_is_probed_before_explicit_recovery(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define SD_CARD_SECTOR_BYTES 512U
static bool card_ready;
static int probes;
static struct { struct { unsigned sector_size; } csd; } storage = {{512}}, *card;
static esp_err_t probe_error;
esp_err_t sd_card_read_sector(uint64_t sector, uint8_t *buffer) {
    assert(card == &storage && sector == 0 && buffer != NULL);
    probes++; return probe_error;
}
'''
        source += production_functions((ROOT / "src/drivers/sd_card.c").read_text(),
                                       ("sd_card_needs_reinit",))
        source += r'''
int main(void) {
    assert(!sd_card_needs_reinit() && probes == 0);
    card = &storage;
    assert(!sd_card_needs_reinit() && probes == 0);
    card_ready = true;
    assert(!sd_card_needs_reinit() && probes == 1);
    probe_error = ESP_ERR_TIMEOUT;
    assert(sd_card_needs_reinit() && probes == 2);
    assert(card_ready && card == &storage);
    storage.csd.sector_size = 1024;
    assert(sd_card_needs_reinit() && probes == 2);
    card = NULL;
    assert(!sd_card_needs_reinit() && probes == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "probe.c"), Path(tmp, "probe")
            path.write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tests/host"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_teardown_keeps_card_deselected_on_a_shared_bus(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include "esp_err.h"
#define SD_CARD_HAS_SDSPI 1
#define SOLAR_OS_PACKAGE_EXPANSION_SDMMC 0
#define SOLAR_OS_BOARD_PIN_SD_CARD_CS 9
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_OUTPUT 1
#define ESP_LOGW(tag, ...) ((void)(tag), ++warnings)
typedef int gpio_num_t;
enum { SD_CARD_TRANSPORT_NONE, SD_CARD_TRANSPORT_SDSPI_BOARD,
       SD_CARD_TRANSPORT_SDSPI_RUNTIME, SD_CARD_TRANSPORT_SDMMC };
int transport, sdspi_runtime_cs = 8;
bool sdspi_runtime_configured = true, sdspi_device_ready, sdspi_bus_acquired;
static const char *TAG = "sd_card";
static int levels[16], modes[16], releases, deinits, writes, warnings, failed_pin = -1;
static int current_cs;
esp_err_t gpio_set_level(int pin, int level) {
    assert(level == 1); writes++;
    if (pin == failed_pin) return ESP_FAIL;
    levels[pin] = level; return ESP_OK;
}
esp_err_t gpio_set_direction(int pin, int mode) {
    assert(mode == GPIO_MODE_OUTPUT && levels[pin] == 1);
    modes[pin] = mode; return ESP_OK;
}
static esp_err_t remove_device(int slot) {
    assert(slot == 42); deinits++;
    modes[current_cs] = GPIO_MODE_INPUT;
    levels[current_cs] = 0;
    return ESP_OK;
}
struct { int slot; esp_err_t (*deinit_p)(int); } host = {42, remove_device};
void solar_os_spi_bus_release(void) {
    assert(levels[current_cs] == 1 && modes[current_cs] == GPIO_MODE_OUTPUT);
    releases++;
}
'''
        source += production_functions((ROOT / "src/drivers/sd_card.c").read_text(),
                                       ("sd_card_deselect_spi", "sd_card_deinit_host"))
        source += r'''
static void check(int kind, int cs, bool legacy) {
    current_cs = cs; transport = kind;
    sdspi_device_ready = true; sdspi_bus_acquired = legacy;
    int before = deinits, released = releases;
    sd_card_deinit_host();
    assert(!sdspi_device_ready && deinits == before + 1);
    assert(modes[cs] == GPIO_MODE_OUTPUT && levels[cs] == 1);
    assert(releases == released + legacy);
    sd_card_deinit_host();
    assert(deinits == before + 1 && releases == released + legacy);
    /* Failure before a device handle exists still leaves a safe CS level. */
    levels[cs] = 0; modes[cs] = GPIO_MODE_INPUT;
    sd_card_deinit_host();
    assert(modes[cs] == GPIO_MODE_OUTPUT && levels[cs] == 1);
    failed_pin = cs;
    sd_card_deinit_host();
    assert(warnings == 1); warnings = 0; failed_pin = -1;
}
int main(void) {
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI
    check(SD_CARD_TRANSPORT_SDSPI_RUNTIME, 8, false);
    sdspi_runtime_configured = false;
    int before = writes;
    sd_card_deinit_host(); assert(writes == before);
#endif
#if SOLAR_OS_BOARD_STORAGE_SDSPI
    check(SD_CARD_TRANSPORT_SDSPI_BOARD, 9, true);
#endif
    transport = SD_CARD_TRANSPORT_NONE;
    int none_before = writes;
    sd_card_deinit_host(); assert(writes == none_before);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "cleanup.c"), Path(tmp, "cleanup")
            path.write_text(source)
            for legacy, runtime in ((0, 1), (1, 0), (1, 1)):
                with self.subTest(legacy=legacy, runtime=runtime):
                    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    f"-DSOLAR_OS_BOARD_STORAGE_SDSPI={legacy}",
                                    f"-DSOLAR_OS_PACKAGE_EXPANSION_SDSPI={runtime}",
                                    "-I", str(ROOT / "tests/host"), str(path), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)
