#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "m5pm1.h"

typedef struct {
    uint8_t registers[0x40];
} fake_io_t;

static esp_err_t fake_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_io_t *fake = ctx;
    memcpy(data, &fake->registers[reg], len);
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    fake_io_t *fake = ctx;
    memcpy(&fake->registers[reg], data, len);
    return ESP_OK;
}

int main(void)
{
    fake_io_t fake = {0};
    /* Power-on defaults: GPIO0-4 and LED_EN open-drain, G2 left in IRQ mode. */
    fake.registers[0x13] = 0x3FU;
    fake.registers[0x16] = 0x10U;
    fake.registers[0x09] = 0x05U;
    fake.registers[0x0A] = 0x1EU; /* watchdog left armed by other firmware */
    const m5pm1_io_t io = {.read = fake_read, .write = fake_write, .ctx = &fake};
    m5pm1_t device;

    assert(m5pm1_init(&device, &io, 0x04U, 0x04U) == ESP_OK);
    assert(fake.registers[0x09] == 0x00U && fake.registers[0x0A] == 0x00U);
    assert(fake.registers[0x11] == 0x04U && fake.registers[0x10] == 0x04U);
    /* LoRa_EN (G2) becomes a push-pull plain GPIO; LED_EN and others untouched. */
    assert(fake.registers[0x13] == 0x3BU);
    assert((fake.registers[0x16] & 0x30U) == 0U);

    assert(m5pm1_configure_pwm0(&device, 5000U) == ESP_OK);
    assert((fake.registers[0x16] & 0xC0U) == 0xC0U);
    assert((fake.registers[0x13] & 0x08U) == 0U);
    assert(fake.registers[0x34] == (5000U & 0xFFU) && fake.registers[0x35] == (5000U >> 8));

    assert(m5pm1_configure(&device, 4U, true) == ESP_OK);
    assert((fake.registers[0x13] & 0x10U) == 0U);
    assert((fake.registers[0x13] & 0x20U) != 0U);
    assert(m5pm1_write(&device, M5PM1_LINE_COUNT, true) == ESP_ERR_INVALID_ARG);
    puts("ok");
    return 0;
}
