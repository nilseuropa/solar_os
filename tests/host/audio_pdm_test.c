#include <assert.h>
#include <string.h>
#include "audio_pdm.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"

enum { NONE, POWER, ALLOCATE, INITIALIZE, ENABLE, DISABLE, DELETE };
static unsigned failure, live, reads, slot;
static bool enabled;
static int power;
static size_t result_bytes;
static esp_err_t read_result;

esp_err_t gpio_set_level(int pin, int value) {
    assert(pin == 38);
    if (value && failure == POWER) return ESP_FAIL;
    power = value; return ESP_OK;
}
esp_err_t gpio_set_direction(int pin, int mode) {
    assert(pin == 38 && mode == GPIO_MODE_OUTPUT && power == 0); return ESP_OK;
}
void vTaskDelay(TickType_t ticks) { assert(ticks == 20); }
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) {
    assert(config->id == 0 && !tx && config->role == I2S_ROLE_MASTER);
    assert(config->dma_desc_num == 4 && config->dma_frame_num == 256);
    if (failure == ALLOCATE) return ESP_ERR_NO_MEM;
    assert(!live); live = 1; *rx = &live; return ESP_OK;
}
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t handle, const i2s_pdm_rx_config_t *config) {
    assert(handle == &live && live && config->clk_cfg.sample_rate_hz == 16000);
    assert(config->slot_cfg.data_bit_width == 16 && config->slot_cfg.slot_mode == I2S_SLOT_MODE_MONO);
    assert(config->slot_cfg.slot_mask == (int)(slot ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT));
    assert(config->gpio_cfg.clk == 19 && config->gpio_cfg.din == 20 && !config->gpio_cfg.invert_flags.clk_inv);
    return failure == INITIALIZE ? ESP_FAIL : ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle) {
    assert(handle == &live && live && !enabled);
    if (failure == ENABLE) return ESP_FAIL;
    enabled = true; return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle) {
    assert(handle == &live && enabled);
    if (failure == DISABLE) return ESP_FAIL;
    enabled = false; return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t handle) {
    assert(handle == &live && live);
    if (enabled || failure == DELETE) return ESP_ERR_INVALID_STATE;
    live = 0; return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *data, size_t len, size_t *read_len, uint32_t timeout) {
    assert(handle == &live && enabled && timeout == 1000);
    memset(data, 0x32, len); *read_len = result_bytes; reads++; return read_result;
}
int main(void) {
    assert(audio_pdm_open(19, 19, 38, 0) == ESP_ERR_INVALID_ARG);
    assert(audio_pdm_open(19, 20, 19, 0) == ESP_ERR_INVALID_ARG);
    assert(audio_pdm_open(19, 20, 38, 2) == ESP_ERR_INVALID_ARG);
    for (failure = POWER; failure <= ENABLE; failure++) {
        assert(audio_pdm_open(19, 20, 38, 0) != ESP_OK);
        assert(!live && !power && !enabled);
    }
    failure = NONE;
    for (slot = 0; slot < 2; slot++) {
        assert(audio_pdm_open(19, 20, 38, slot) == ESP_OK && power && enabled);
        assert(audio_pdm_open(19, 20, 38, slot) == ESP_ERR_INVALID_STATE);
        int16_t buffer[16]; size_t count;
        result_bytes = sizeof(buffer); read_result = ESP_OK;
        assert(audio_pdm_read(buffer, sizeof(buffer), UINT32_MAX, &count) == ESP_OK && count == sizeof(buffer));
        assert(buffer[0] == 0x3232);
        result_bytes = 8; read_result = ESP_ERR_TIMEOUT;
        assert(audio_pdm_read(buffer, sizeof(buffer), 1000, &count) == ESP_ERR_TIMEOUT && count == 8);
        result_bytes = 1;
        assert(audio_pdm_read(buffer, sizeof(buffer), 1000, &count) == ESP_ERR_INVALID_SIZE && count == 0);
        unsigned before = reads;
        assert(audio_pdm_read(buffer, 3, 1000, &count) == ESP_ERR_INVALID_SIZE && reads == before);
        assert(audio_pdm_close() == ESP_OK && !live && !enabled && !power);
        assert(audio_pdm_read(buffer, sizeof(buffer), 1000, &count) == ESP_ERR_INVALID_STATE);
    }
    slot = 0;
    assert(audio_pdm_open(19, 20, 38, 0) == ESP_OK);
    failure = DISABLE;
    assert(audio_pdm_close() == ESP_FAIL && live && !power);
    failure = NONE;
    assert(audio_pdm_close() == ESP_OK && !live);
    assert(audio_pdm_open(19, 20, 38, 0) == ESP_OK);
    failure = DELETE;
    assert(audio_pdm_close() == ESP_ERR_INVALID_STATE && live && !enabled && !power);
    failure = NONE;
    assert(audio_pdm_close() == ESP_OK && !live);
    assert(audio_pdm_close() == ESP_OK);
    return 0;
}
