#include "pcm5102.h"

#include <stdbool.h>

#include "driver/i2s_std.h"
#include "soc/soc_caps.h"
#include "solar_os_board.h"

#define PCM5102_DMA_DESC_NUM 4U
#define PCM5102_DMA_FRAME_NUM PCM5102_FRAMES_PER_BLOCK
#define PCM5102_WRITE_FRAMES 128U
#define PCM5102_WRITE_TIMEOUT_MAX_MS 1000U

typedef struct {
  bool active;
  i2s_chan_handle_t tx_handle;
} pcm5102_state_t;

static pcm5102_state_t pcm5102;

static void pcm5102_delete_channel(void) {
  if (pcm5102.tx_handle == NULL) {
    return;
  }
  if (pcm5102.active) {
    (void)i2s_channel_disable(pcm5102.tx_handle);
  }
  (void)i2s_del_channel(pcm5102.tx_handle);
  pcm5102.tx_handle = NULL;
  pcm5102.active = false;
}

esp_err_t pcm5102_open(int i2s_port, gpio_num_t bck_pin, gpio_num_t din_pin,
                       gpio_num_t rck_pin) {
  if (i2s_port < 0 || i2s_port >= SOC_I2S_NUM ||
      (SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK & (1U << i2s_port)) == 0U ||
      !GPIO_IS_VALID_OUTPUT_GPIO(bck_pin) ||
      !GPIO_IS_VALID_OUTPUT_GPIO(din_pin) ||
      !GPIO_IS_VALID_OUTPUT_GPIO(rck_pin) || bck_pin == din_pin ||
      bck_pin == rck_pin || din_pin == rck_pin) {
    return ESP_ERR_INVALID_ARG;
  }
  if (pcm5102.tx_handle != NULL || pcm5102.active) {
    return ESP_ERR_INVALID_STATE;
  }
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)i2s_port, I2S_ROLE_MASTER);
  channel_config.auto_clear_after_cb = true;
  channel_config.dma_desc_num = PCM5102_DMA_DESC_NUM;
  channel_config.dma_frame_num = PCM5102_DMA_FRAME_NUM;
  esp_err_t err = i2s_new_channel(&channel_config, &pcm5102.tx_handle, NULL);
  if (err != ESP_OK) {
    return err;
  }

  i2s_std_config_t standard_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PCM5102_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = bck_pin,
              .ws = rck_pin,
              .dout = din_pin,
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  err = i2s_channel_init_std_mode(pcm5102.tx_handle, &standard_config);
  if (err == ESP_OK) {
    err = i2s_channel_enable(pcm5102.tx_handle);
  }
  if (err != ESP_OK) {
    pcm5102_delete_channel();
    return err;
  }
  pcm5102.active = true;
  return ESP_OK;
}

esp_err_t pcm5102_write_s16(const int16_t *samples, size_t frames,
                            uint8_t channels, uint8_t volume,
                            uint32_t timeout_ms, size_t *frames_written) {
  if (samples == NULL || frames == 0U || channels == 0U || channels > 2U ||
      volume > 100U || frames_written == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *frames_written = 0U;
  if (!pcm5102.active || pcm5102.tx_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  int32_t stereo[PCM5102_WRITE_FRAMES * 2U];
  const uint32_t write_timeout_ms =
      timeout_ms > PCM5102_WRITE_TIMEOUT_MAX_MS
          ? PCM5102_WRITE_TIMEOUT_MAX_MS
          : timeout_ms;
  while (*frames_written < frames) {
    const size_t remaining = frames - *frames_written;
    const size_t count =
        remaining < PCM5102_WRITE_FRAMES ? remaining : PCM5102_WRITE_FRAMES;
    for (size_t i = 0; i < count; i++) {
      const size_t source_frame = *frames_written + i;
      const int32_t left =
          ((int32_t)samples[source_frame * channels] * volume) / 100;
      const int32_t right =
          channels == 2U
              ? ((int32_t)samples[(source_frame * channels) + 1U] * volume) /
                    100
              : left;
      /*
       * Use native 32-bit DMA samples so the valid S16 data is explicitly in
       * the most-significant half of each 32-bit Philips-I2S slot.  Relying on
       * mixed 16-bit data and 32-bit slots leaves alignment target-dependent.
       */
      stereo[i * 2U] = (int32_t)((int64_t)left * 65536LL);
      stereo[(i * 2U) + 1U] = (int32_t)((int64_t)right * 65536LL);
    }

    size_t output_bytes = 0U;
    const size_t requested_bytes = count * 2U * sizeof(int32_t);
    const esp_err_t err = i2s_channel_write(
        pcm5102.tx_handle, stereo, requested_bytes, &output_bytes,
        write_timeout_ms);
    *frames_written += output_bytes / (2U * sizeof(int32_t));
    if (err != ESP_OK) {
      return err;
    }
    if (output_bytes != requested_bytes) {
      return ESP_ERR_TIMEOUT;
    }
  }
  return ESP_OK;
}

void pcm5102_close(void) { pcm5102_delete_channel(); }
