#include "io_expander_ch422g.h"

#include "esp_check.h"
#include "i2c_bus.h"

static const char *TAG = "ch422g";

/* CH422G has no single I2C address: each "register" is its own slave
 * address, addressed with a plain one-byte write (no register-address
 * prefix byte). */
#define CH422G_ADDR_WR_SET (0x48 >> 1)
#define CH422G_ADDR_WR_IO (0x70 >> 1)

/* WR_SET bit 0: 1 = IO0-7 are push-pull outputs, 0 = inputs. Set by
 * default on power-up, but written explicitly here for a known state. */
#define CH422G_WR_SET_IO_OE_BIT 0x01

/* Reset default for the IO0-7 output register: all pins idle high. */
#define CH422G_WR_IO_RESET_VAL 0xff

static uint8_t output_shadow = CH422G_WR_IO_RESET_VAL;
static bool initialized;

esp_err_t io_expander_ch422g_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus init failed");
    ESP_RETURN_ON_ERROR(i2c_bus_probe(CH422G_ADDR_WR_SET), TAG, "CH422G not found on i2c bus");

    const uint8_t wr_set = CH422G_WR_SET_IO_OE_BIT;
    ESP_RETURN_ON_ERROR(i2c_bus_transmit(CH422G_ADDR_WR_SET, &wr_set, 1),
                        TAG,
                        "enable IO0-7 output mode failed");

    output_shadow = CH422G_WR_IO_RESET_VAL;
    ESP_RETURN_ON_ERROR(i2c_bus_transmit(CH422G_ADDR_WR_IO, &output_shadow, 1),
                        TAG,
                        "write IO0-7 reset state failed");

    initialized = true;
    return ESP_OK;
}

esp_err_t io_expander_ch422g_set_pin(uint8_t pin, bool level)
{
    if (pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized) {
        ESP_RETURN_ON_ERROR(io_expander_ch422g_init(), TAG, "lazy init failed");
    }

    const uint8_t updated = level
        ? (uint8_t)(output_shadow | (1U << pin))
        : (uint8_t)(output_shadow & ~(1U << pin));
    if (updated == output_shadow) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2c_bus_transmit(CH422G_ADDR_WR_IO, &updated, 1), TAG, "write IO0-7 failed");
    output_shadow = updated;
    return ESP_OK;
}
