#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "waveshare_io.h"

static uint8_t registers[8], order[32];
static size_t writes;
static int fail_reg = -1;
static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    (void)ctx;
    if (reg == fail_reg) return ESP_FAIL;
    assert(reg + len <= sizeof(registers));
    memcpy(data, registers + reg, len);
    return ESP_OK;
}
static esp_err_t write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    (void)ctx;
    assert(writes < sizeof(order));
    order[writes++] = reg;
    if (reg == fail_reg) return ESP_FAIL;
    assert(len == 1 && reg < sizeof(registers));
    registers[reg] = *data;
    return ESP_OK;
}
int main(void)
{
    solar_os_ws_io_chip_t d;
    const solar_os_ws_io_transport_t io = {.read=read_reg, .write=write_reg};
    assert(solar_os_ws_io_chip_init(&d, &io, 0x92, 0xf0) == ESP_OK);
    assert(writes == 2 && order[0] == 3 && order[1] == 2);
    assert(registers[2] == 0xf0 && registers[3] == 0x92);
    assert(solar_os_ws_io_chip_write(&d, 2, true) == ESP_OK);
    assert(order[2] == 3 && order[3] == 2);
    assert(d.output == 0x96 && d.direction == 0xf4);
    fail_reg = 3;
    assert(solar_os_ws_io_chip_write(&d, 1, false) == ESP_FAIL);
    assert(d.output == 0x96);
    fail_reg = 2;
    assert(solar_os_ws_io_chip_configure(&d, 4, false) == ESP_FAIL);
    assert(d.direction == 0xf4);
    fail_reg = -1;
    bool level = false;
    registers[4] = 0x80;
    assert(solar_os_ws_io_chip_read(&d, 7, &level) == ESP_OK && level);
    assert(solar_os_ws_io_chip_read(&d, 8, &level) == ESP_ERR_INVALID_ARG);
    assert(solar_os_ws_io_chip_pwm(&d, 247) == ESP_OK && registers[5] == 247);
    uint16_t raw = 0;
    registers[6] = 0xab; registers[7] = 2;
    assert(solar_os_ws_io_chip_adc(&d, &raw) == ESP_OK && raw == 0x2ab);
    registers[7] = 4;
    assert(solar_os_ws_io_chip_adc(&d, &raw) == ESP_ERR_INVALID_RESPONSE && raw == 0x2ab);
    fail_reg = 6;
    assert(solar_os_ws_io_chip_adc(&d, &raw) == ESP_FAIL);
    fail_reg = 2;
    assert(solar_os_ws_io_chip_init(&d, &io, 0, 0) == ESP_FAIL && !d.ready);
    assert(solar_os_ws_io_chip_write(&d, 0, false) == ESP_ERR_INVALID_STATE);
    puts("Waveshare I/O protocol, sequencing, ADC bounds and failed-write retry: ok");
}
