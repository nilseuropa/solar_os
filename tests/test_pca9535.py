from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Pca9535Test(unittest.TestCase):
    def test_ports_initialization_and_failed_writes(self):
        source = r'''
#include <assert.h>
#include <string.h>
#include "pca9535.h"
static uint8_t regs[8], writes[32];
static unsigned count, fail_at;
static bool read_fails;
static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t size) {
    (void)ctx; assert(reg < 2 && size == 1);
    if (read_fails) return ESP_FAIL;
    *data = regs[reg]; return ESP_OK;
}
static esp_err_t write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t size) {
    (void)ctx; assert(reg >= 2 && reg < 8 && size == 1 && count < 32);
    writes[count++] = reg;
    if (count == fail_at) return ESP_FAIL;
    regs[reg] = *data; return ESP_OK;
}
int main(void) {
    const solar_os_pca9535_io_t io = {.read=read_reg, .write=write_reg};
    solar_os_pca9535_t d = {0};
    bool level = false;
    assert(solar_os_pca9535_read(&d, 0, &level) == ESP_ERR_INVALID_STATE);
    assert(solar_os_pca9535_init(NULL, &io, 0, 0) == ESP_ERR_INVALID_ARG);
    for (unsigned failed = 1; failed <= 6; ++failed) {
        count = 0; fail_at = failed;
        assert(solar_os_pca9535_init(&d, &io, 0x1234, 0xfff0) == ESP_FAIL);
        assert(!d.initialized && count == failed);
    }
    memset(regs, 0xff, sizeof(regs)); count = fail_at = 0;
    assert(solar_os_pca9535_init(&d, &io, 0x1234, 0xfff0) == ESP_OK);
    assert(count == 6 && !memcmp(writes, (uint8_t[]){2,3,4,5,6,7}, 6));
    assert(regs[2] == 0x34 && regs[3] == 0x12 && regs[4] == 0 && regs[5] == 0);
    assert(d.output == 0x1234 && d.direction == 0xfff0);
    for (unsigned line = 0; line < 16; ++line) {
        regs[0] = regs[1] = 0; regs[line / 8] = 1U << (line % 8);
        assert(solar_os_pca9535_read(&d, line, &level) == ESP_OK && level);
        assert(solar_os_pca9535_read(&d, line ^ 1, &level) == ESP_OK && !level);
    }
    assert(solar_os_pca9535_write(&d, 16, true) == ESP_ERR_INVALID_ARG);
    assert(solar_os_pca9535_read(&d, 0, NULL) == ESP_ERR_INVALID_ARG);
    level = true; read_fails = true;
    assert(solar_os_pca9535_read(&d, 0, &level) == ESP_FAIL && level);
    read_fails = false;
    /* A failed latch write must not enable an output or alter the other bank. */
    count = 0; fail_at = 1;
    assert(solar_os_pca9535_write(&d, 15, true) == ESP_FAIL);
    assert(count == 1 && writes[0] == 3 && d.output == 0x1234 && d.direction == 0xfff0);
    /* A direction failure retains the successful latch for a safe retry. */
    count = 0; fail_at = 2;
    assert(solar_os_pca9535_write(&d, 15, true) == ESP_FAIL);
    assert(count == 2 && writes[0] == 3 && writes[1] == 7);
    assert(d.output == 0x9234 && d.direction == 0xfff0);
    count = fail_at = 0;
    assert(solar_os_pca9535_write(&d, 15, true) == ESP_OK);
    assert(count == 1 && writes[0] == 7 && d.direction == 0x7ff0);
    assert(solar_os_pca9535_configure(&d, 15, false) == ESP_OK && d.direction == 0xfff0);
    assert(regs[2] == 0x34 && regs[6] == 0xf0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/drivers"),
                str(path / "test.c"), str(ROOT / "src/drivers/pca9535.c"),
                "-o", str(path / "test"),
            ], check=True)
            subprocess.run([str(path / "test")], check=True)
