#include "co2_scd41.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_port_a.h"

#define SCD41_I2C_ADDR 0x62
#define SCD41_XFER_TIMEOUT_MS 100
#define SCD41_READ_LEN 12U

static const char *TAG = "co2_scd41";
static i2c_master_dev_handle_t scd41_dev_handle;
static bool scd41_initialized;

esp_err_t co2_scd41_init(void)
{
    if (scd41_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = i2c_bus_port_a_init();
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_master_bus_handle_t bus_handle = i2c_bus_port_a_get_handle();
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_bus_port_a_lock();

    if (scd41_dev_handle == NULL) {
        const i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = SCD41_I2C_ADDR,
            .scl_speed_hz = 100000U,
        };
        ret = i2c_master_bus_add_device(bus_handle, &dev_config, &scd41_dev_handle);
        if (ret != ESP_OK) {
            i2c_bus_port_a_unlock();
            ESP_LOGW(TAG, "device add failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* Start periodic measurement (command 0x21b1) -- SCD41 wakes up in
     * idle mode and won't produce any readings until this is sent
     * once. The reference this was ported from didn't include this
     * step, presumably because its setup() already did it elsewhere. */
    const uint8_t start_cmd[2] = {0x21, 0xb1};
    ret = i2c_master_transmit(scd41_dev_handle, start_cmd, sizeof(start_cmd), SCD41_XFER_TIMEOUT_MS);
    i2c_bus_port_a_unlock();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start periodic measurement failed: %s", esp_err_to_name(ret));
        return ret;
    }

    scd41_initialized = true;
    ESP_LOGI(TAG, "SCD41 ready on Port A (periodic measurement started)");
    return ESP_OK;
}

esp_err_t co2_scd41_read(co2_scd41_reading_t *reading)
{
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    reading->valid = false;

    if (!scd41_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Locked for the whole write-delay-read sequence, not just the
     * individual transfers -- SCD41 needs the two halves of this
     * exchange back to back, and the delay below yields the CPU, which
     * would otherwise let another Port A device's transaction (e.g.
     * CardKB's background polling, which runs every tick regardless of
     * which app is in front) interleave and corrupt both. */
    i2c_bus_port_a_lock();

    const uint8_t read_cmd[2] = {0xec, 0x05};
    esp_err_t ret = i2c_master_transmit(scd41_dev_handle, read_cmd, sizeof(read_cmd), SCD41_XFER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        i2c_bus_port_a_unlock();
        return ret;
    }

    /* SCD41 needs ~1ms to prepare the response after this command. */
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t data[SCD41_READ_LEN] = {0};
    ret = i2c_master_receive(scd41_dev_handle, data, sizeof(data), SCD41_XFER_TIMEOUT_MS);
    i2c_bus_port_a_unlock();
    if (ret != ESP_OK) {
        return ret;
    }

    /* data[2],[5],[8] are per-word CRCs and data[9..11] is a sensor-
     * status word -- read but deliberately not validated/used, same as
     * the reference this was ported from. */
    reading->co2_ppm = (float)(((uint16_t)data[0] << 8) | data[1]);
    reading->temperature_c = -45.0f + 175.0f * (float)(((uint16_t)data[3] << 8) | data[4]) / 65536.0f;
    reading->humidity_pct = 100.0f * (float)(((uint16_t)data[6] << 8) | data[7]) / 65536.0f;
    reading->valid = true;
    return ESP_OK;
}
