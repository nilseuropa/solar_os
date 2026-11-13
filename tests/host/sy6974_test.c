#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sy6974.h"

static uint8_t registers[12];
static unsigned writes;
static esp_err_t read_error, write_error;
static esp_err_t read_regs(void *ctx, uint8_t reg, uint8_t *data, size_t size)
{
    (void)ctx; assert(reg < sizeof(registers) && size == 1);
    if (read_error) return read_error;
    *data = registers[reg]; return ESP_OK;
}
static esp_err_t write_regs(void *ctx, uint8_t reg, const uint8_t *data, size_t size)
{
    (void)ctx; assert(reg < sizeof(registers) && size == 1);
    if (write_error) return write_error;
    registers[reg] = *data; writes++; return ESP_OK;
}
int main(void)
{
    const sy6974_io_t io = {.read = read_regs, .write = write_regs};
    sy6974_t d;
    /* Never configure an unrelated charger merely because its address is 0x6b. */
    registers[11] = 0x14;
    assert(sy6974_init(&d, &io) == ESP_ERR_NOT_FOUND && writes == 0);
    registers[11] = 0x42; registers[5] = 0xff;
    assert(sy6974_init(&d, &io) == ESP_OK && registers[5] == 0xcf);
    assert(d.part_number == 8 && d.revision == 2);
    registers[0] = 0xe0; registers[2] = 0xc0; registers[4] = 0x07;
    assert(sy6974_set_input_current_limit(&d,1000) == ESP_OK && registers[0] == 0xe9);
    assert(sy6974_set_charge_current(&d,480) == ESP_OK && registers[2] == 0xc8);
    assert(sy6974_set_charge_voltage(&d,4208) == ESP_OK && registers[4] == 0x5f);
    registers[6] = 0xf0;
    assert(sy6974_set_input_voltage_limit(&d,4100) == ESP_OK && registers[6] == 0xf2);
    assert(sy6974_set_input_voltage_limit(&d,4150) == ESP_ERR_INVALID_ARG);
    unsigned before = writes;
    assert(sy6974_set_input_current_limit(&d,1050) == ESP_ERR_INVALID_ARG);
    assert(sy6974_set_input_current_limit(&d,3300) == ESP_ERR_INVALID_ARG);
    assert(sy6974_set_charge_current(&d,500) == ESP_ERR_INVALID_ARG);
    assert(sy6974_set_charge_current(&d,3060) == ESP_ERR_INVALID_ARG);
    assert(sy6974_set_charge_voltage(&d,4200) == ESP_ERR_INVALID_ARG);
    assert(writes == before);
    registers[1] = 0x8f;
    assert(sy6974_set_enabled(&d,true) == ESP_OK && registers[1] == 0x9f);
    registers[8] = 0x54; registers[9] = 0x08;
    sy6974_status_t status;
    assert(sy6974_read_status(&d,&status) == ESP_OK);
    assert(status.input_present && status.power_good && status.enabled);
    assert(status.state == SY6974_CHARGE_STATE_FAST && status.fault == 8);
    assert(status.input_current_limit_ma == 1000 && status.charge_current_ma == 480 && status.charge_voltage_mv == 4208);
    registers[8] = 0xe0; /* OTG output is not an external input supply. */
    assert(sy6974_read_status(&d,&status) == ESP_OK && !status.input_present);
    read_error = ESP_ERR_TIMEOUT;
    assert(sy6974_read_status(&d,&status) == ESP_ERR_TIMEOUT);
    assert(sy6974_set_enabled(&d,false) == ESP_ERR_TIMEOUT);
    read_error = 0; write_error = ESP_FAIL;
    assert(sy6974_set_charge_current(&d,480) == ESP_FAIL);
    write_error = 0;
    assert(sy6974_deinit(&d) == ESP_OK);
    assert(sy6974_set_enabled(&d,true) == ESP_ERR_INVALID_STATE);
    write_error = ESP_FAIL;
    assert(sy6974_init(&d,&io) == ESP_FAIL && !d.initialized);
    puts("SY6974B identity, current/voltage, status and error tests passed");
    return 0;
}
