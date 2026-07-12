#include "i2c_bus_port_a.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board.h"

static const char *TAG = "i2c_bus_port_a";

static i2c_master_bus_handle_t bus_handle;
static SemaphoreHandle_t bus_mutex;

esp_err_t i2c_bus_port_a_init(void)
{
    if (bus_handle != NULL) {
        return ESP_OK;
    }

    bus_mutex = xSemaphoreCreateMutex();
    if (bus_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* GPIO1/2 (CoreS3) and GPIO32/33 (Core2) are all RTC-capable pins
     * -- if left in RTC/analog pad mode (their power-on-reset default
     * on some SoCs), the digital pull-up i2c_master configures below
     * has no effect, since pull-up/down for an RTC pad is a separate
     * analog register from the digital GPIO one. Force them back to
     * plain digital GPIO mode with an explicit pull-up first. */
    if (rtc_gpio_is_valid_gpio(SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA)) {
        rtc_gpio_deinit(SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA);
    }
    if (rtc_gpio_is_valid_gpio(SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL)) {
        rtc_gpio_deinit(SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL);
    }
    gpio_pullup_en(SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA);
    gpio_pullup_en(SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL);

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = SOLAR_OS_BOARD_PORT_A_I2C_PORT,
        .sda_io_num = SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA,
        .scl_io_num = SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Port A i2c bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Recover/clear the bus before first use -- if the lines were left
     * in a non-idle state (e.g. SDA stuck low from a partial
     * transaction on a previous boot/attempt), this drives clock
     * pulses until the slave releases SDA. */
    ret = i2c_master_bus_reset(bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Port A i2c bus reset failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG,
             "Port A i2c bus ready: port=%d SDA=%d SCL=%d",
             (int)SOLAR_OS_BOARD_PORT_A_I2C_PORT,
             (int)SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA,
             (int)SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_port_a_get_handle(void)
{
    return bus_handle;
}

void i2c_bus_port_a_lock(void)
{
    if (bus_mutex != NULL) {
        xSemaphoreTake(bus_mutex, portMAX_DELAY);
    }
}

void i2c_bus_port_a_unlock(void)
{
    if (bus_mutex != NULL) {
        xSemaphoreGive(bus_mutex);
    }
}
