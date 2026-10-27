#include "audio_pdm.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static struct {
    i2s_chan_handle_t channel;
    bool enabled;
    int power_pin;
} pdm = {.power_pin = -1};

esp_err_t audio_pdm_prepare_power(int power_pin) {
    if (power_pin < 0) return power_pin == -1 ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(power_pin)) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = gpio_set_level(power_pin, 0);
    if (ret == ESP_OK) ret = gpio_set_direction(power_pin, GPIO_MODE_OUTPUT);
    return ret;
}

esp_err_t audio_pdm_close(void) {
    esp_err_t ret = ESP_OK;
    if (pdm.channel != NULL) {
        if (pdm.enabled) {
            ret = i2s_channel_disable(pdm.channel);
            if (ret == ESP_OK) pdm.enabled = false;
        }
        const esp_err_t deleted = i2s_del_channel(pdm.channel);
        if (ret == ESP_OK) ret = deleted;
        if (deleted == ESP_OK) { pdm.channel = NULL; pdm.enabled = false; }
    }
    if (pdm.power_pin >= 0) {
        const esp_err_t powered_off = gpio_set_level(pdm.power_pin, 0);
        if (ret == ESP_OK) ret = powered_off;
        if (powered_off == ESP_OK && pdm.channel == NULL) pdm.power_pin = -1;
    }
    return ret;
}

esp_err_t audio_pdm_open(int clk_pin, int data_pin, int power_pin, unsigned slot) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(clk_pin) || !GPIO_IS_VALID_GPIO(data_pin) ||
        clk_pin == data_pin || power_pin == clk_pin || power_pin == data_pin ||
        power_pin < -1 || slot > 1) return ESP_ERR_INVALID_ARG;
    if (pdm.channel != NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = audio_pdm_prepare_power(power_pin);
    if (ret != ESP_OK) return ret;
    pdm.power_pin = power_pin;
    if (power_pin >= 0) ret = gpio_set_level(power_pin, 1);
    if (ret == ESP_OK) {
        TickType_t ticks = pdMS_TO_TICKS(20);
        vTaskDelay(ticks ? ticks : 1);
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        channel.dma_desc_num = 4;
        channel.dma_frame_num = AUDIO_PDM_FRAMES_PER_BLOCK;
        ret = i2s_new_channel(&channel, NULL, &pdm.channel);
    }
    if (ret == ESP_OK) {
        i2s_pdm_rx_config_t config = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_PDM_SAMPLE_RATE),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {.clk=clk_pin, .din=data_pin, .invert_flags={.clk_inv=false}},
        };
        config.slot_cfg.slot_mask = slot ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
        ret = i2s_channel_init_pdm_rx_mode(pdm.channel, &config);
    }
    if (ret == ESP_OK) {
        ret = i2s_channel_enable(pdm.channel);
        pdm.enabled = ret == ESP_OK;
    }
    if (ret != ESP_OK) (void)audio_pdm_close();
    return ret;
}

esp_err_t audio_pdm_read(void *data, size_t len, uint32_t timeout_ms, size_t *read_len) {
    if (read_len == NULL) return ESP_ERR_INVALID_ARG;
    *read_len = 0;
    if (data == NULL || len == 0 || len % sizeof(int16_t)) return ESP_ERR_INVALID_SIZE;
    if (!pdm.channel || !pdm.enabled) return ESP_ERR_INVALID_STATE;
    if (timeout_ms == 0 || timeout_ms > 1000) timeout_ms = 1000;
    esp_err_t ret = i2s_channel_read(pdm.channel, data, len, read_len, timeout_ms);
    if (*read_len > len || *read_len % sizeof(int16_t)) {
        *read_len = 0;
        ret = ESP_ERR_INVALID_SIZE;
    }
    return ret;
}
