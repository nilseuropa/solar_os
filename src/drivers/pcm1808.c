#include "pcm1808.h"

#include <stdbool.h>

#include "driver/i2s_std.h"
#include "soc/soc_caps.h"
#include "solar_os_board.h"

#define PCM1808_DMA_DESC_NUM 4U
#define PCM1808_DMA_FRAME_NUM PCM1808_FRAMES_PER_BLOCK
#define PCM1808_READ_FRAMES 128U
#define PCM1808_READ_TIMEOUT_MS 1000U

typedef struct {
  bool active;
  i2s_chan_handle_t rx_handle;
} pcm1808_state_t;

static pcm1808_state_t pcm1808;

int pcm1808_i2s_port(void) {
  for (int port = 0; port < SOC_I2S_NUM; port++) {
    if ((SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK & (1U << port)) != 0U) {
      return port;
    }
  }
  return -1;
}

static void pcm1808_delete_channel(void) {
  if (pcm1808.rx_handle == NULL) {
    return;
  }
  if (pcm1808.active) {
    (void)i2s_channel_disable(pcm1808.rx_handle);
  }
  (void)i2s_del_channel(pcm1808.rx_handle);
  pcm1808.rx_handle = NULL;
  pcm1808.active = false;
}

esp_err_t pcm1808_open(gpio_num_t mclk_pin, gpio_num_t bck_pin,
                       gpio_num_t ws_pin, gpio_num_t dout_pin) {
  if (!GPIO_IS_VALID_OUTPUT_GPIO(mclk_pin) ||
      !GPIO_IS_VALID_OUTPUT_GPIO(bck_pin) ||
      !GPIO_IS_VALID_OUTPUT_GPIO(ws_pin) || !GPIO_IS_VALID_GPIO(dout_pin) ||
      mclk_pin == bck_pin || mclk_pin == ws_pin || mclk_pin == dout_pin ||
      bck_pin == ws_pin || bck_pin == dout_pin || ws_pin == dout_pin) {
    return ESP_ERR_INVALID_ARG;
  }
  if (pcm1808.rx_handle != NULL || pcm1808.active) {
    return ESP_ERR_INVALID_STATE;
  }
  const int port = pcm1808_i2s_port();
  if (port < 0) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)port, I2S_ROLE_MASTER);
  channel_config.auto_clear = true;
  channel_config.dma_desc_num = PCM1808_DMA_DESC_NUM;
  channel_config.dma_frame_num = PCM1808_DMA_FRAME_NUM;
  esp_err_t err = i2s_new_channel(&channel_config, NULL, &pcm1808.rx_handle);
  if (err != ESP_OK) {
    return err;
  }

  i2s_std_config_t standard_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PCM1808_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = mclk_pin,
              .bclk = bck_pin,
              .ws = ws_pin,
              .dout = I2S_GPIO_UNUSED,
              .din = dout_pin,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  standard_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  err = i2s_channel_init_std_mode(pcm1808.rx_handle, &standard_config);
  if (err == ESP_OK) {
    err = i2s_channel_enable(pcm1808.rx_handle);
  }
  if (err != ESP_OK) {
    pcm1808_delete_channel();
    return err;
  }
  pcm1808.active = true;
  return ESP_OK;
}

esp_err_t pcm1808_read_s16(int16_t *samples, size_t frames,
                           uint32_t timeout_ms, size_t *frames_read) {
  if (samples == NULL || frames == 0U || frames_read == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *frames_read = 0U;
  if (!pcm1808.active || pcm1808.rx_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  int32_t raw[PCM1808_READ_FRAMES * PCM1808_CHANNELS];
  const uint32_t read_timeout_ms =
      timeout_ms == 0U || timeout_ms == UINT32_MAX
          ? PCM1808_READ_TIMEOUT_MS
          : (timeout_ms > PCM1808_READ_TIMEOUT_MS
                 ? PCM1808_READ_TIMEOUT_MS
                 : timeout_ms);
  while (*frames_read < frames) {
    const size_t remaining = frames - *frames_read;
    const size_t count =
        remaining < PCM1808_READ_FRAMES ? remaining : PCM1808_READ_FRAMES;
    const size_t requested_bytes =
        count * PCM1808_CHANNELS * sizeof(raw[0]);
    size_t input_bytes = 0U;
    const esp_err_t err = i2s_channel_read(
        pcm1808.rx_handle, raw, requested_bytes, &input_bytes,
        read_timeout_ms);
    if ((input_bytes % (PCM1808_CHANNELS * sizeof(raw[0]))) != 0U) {
      return ESP_ERR_INVALID_SIZE;
    }
    const size_t input_frames =
        input_bytes / (PCM1808_CHANNELS * sizeof(raw[0]));
    for (size_t i = 0; i < input_frames * PCM1808_CHANNELS; i++) {
      samples[(*frames_read * PCM1808_CHANNELS) + i] =
          (int16_t)(raw[i] >> 16);
    }
    *frames_read += input_frames;
    if (err != ESP_OK) {
      return err;
    }
    if (input_bytes != requested_bytes) {
      return ESP_ERR_TIMEOUT;
    }
  }
  return ESP_OK;
}

void pcm1808_close(void) { pcm1808_delete_channel(); }
