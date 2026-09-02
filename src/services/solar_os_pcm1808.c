#include "solar_os_pcm1808.h"

#include <stdio.h>
#include <string.h>

#include "pcm1808.h"
#include "solar_os_audio.h"
#include "solar_os_resources.h"
#include "solar_os_stream.h"

#define PCM1808_DEVICE_NAME_MAX 16U

typedef struct {
  bool attached;
  int i2s_port;
  int mclk_pin;
  int bck_pin;
  int ws_pin;
  int dout_pin;
  char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
  char stream_id[SOLAR_OS_STREAM_ID_MAX];
} solar_os_pcm1808_device_t;

static solar_os_pcm1808_device_t pcm1808_audio;

static const solar_os_stream_audio_format_t pcm1808_native_format = {
    .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    .sample_rate = PCM1808_SAMPLE_RATE,
    .channels = PCM1808_CHANNELS,
    .bits_per_sample = PCM1808_BITS_PER_SAMPLE,
    .frames_per_block = PCM1808_FRAMES_PER_BLOCK,
};

static bool pcm1808_requested_format_supported(
    const solar_os_stream_audio_format_t *format) {
  if (format == NULL) {
    return true;
  }
  return (format->sample_rate == 0U ||
          format->sample_rate == PCM1808_SAMPLE_RATE) &&
         (format->channels == 0U || format->channels == PCM1808_CHANNELS) &&
         (format->bits_per_sample == 0U ||
          format->bits_per_sample == PCM1808_BITS_PER_SAMPLE) &&
         format->sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE;
}

static esp_err_t
pcm1808_stream_open(void *user, const char *owner,
                    const solar_os_stream_open_options_t *options,
                    solar_os_stream_handle_t *handle) {
  (void)owner;
  solar_os_pcm1808_device_t *device = user;
  if (device == NULL || !device->attached || handle == NULL ||
      !pcm1808_requested_format_supported(
          options != NULL ? &options->requested_audio : NULL)) {
    return device != NULL && device->attached ? ESP_ERR_NOT_SUPPORTED
                                              : ESP_ERR_INVALID_STATE;
  }

  const esp_err_t err =
      pcm1808_open((gpio_num_t)device->mclk_pin,
                   (gpio_num_t)device->bck_pin, (gpio_num_t)device->ws_pin,
                   (gpio_num_t)device->dout_pin);
  if (err == ESP_OK) {
    handle->context = device;
    handle->audio = pcm1808_native_format;
  }
  return err;
}

static void pcm1808_stream_close(void *user, solar_os_stream_handle_t *handle) {
  (void)user;
  pcm1808_close();
  handle->context = NULL;
}

static esp_err_t pcm1808_stream_read(void *user,
                                     solar_os_stream_handle_t *handle,
                                     void *data, size_t len,
                                     uint32_t timeout_ms, size_t *read_len) {
  solar_os_pcm1808_device_t *device = user;
  if (device == NULL || handle == NULL || handle->context != device ||
      data == NULL || read_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  const size_t frame_bytes = PCM1808_CHANNELS * sizeof(int16_t);
  if (len == 0U || (len % frame_bytes) != 0U) {
    return ESP_ERR_INVALID_SIZE;
  }
  size_t frames_read = 0U;
  const esp_err_t err = pcm1808_read_s16(
      data, len / frame_bytes, timeout_ms, &frames_read);
  *read_len = frames_read * frame_bytes;
  return err;
}

static esp_err_t pcm1808_register_stream(solar_os_pcm1808_device_t *device) {
  solar_os_stream_driver_t driver = {
      .info =
          {
              .type = SOLAR_OS_STREAM_TYPE_AUDIO,
              .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
              .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
              .audio = pcm1808_native_format,
          },
      .open = pcm1808_stream_open,
      .close = pcm1808_stream_close,
      .read = pcm1808_stream_read,
      .user = device,
  };
  strlcpy(driver.info.id, device->stream_id, sizeof(driver.info.id));
  strlcpy(driver.info.provider, "pcm1808", sizeof(driver.info.provider));
  strlcpy(driver.info.device, device->id, sizeof(driver.info.device));
  strlcpy(driver.info.unit, "frames", sizeof(driver.info.unit));
  strlcpy(driver.info.format, "pcm-s16le", sizeof(driver.info.format));
  strlcpy(driver.info.summary, "PCM1808 I2S audio capture",
          sizeof(driver.info.summary));
  return solar_os_stream_register(&driver);
}

esp_err_t solar_os_pcm1808_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count) {
  if (name == NULL || bindings == NULL || binding_count == 0U ||
      strnlen(name, PCM1808_DEVICE_NAME_MAX) >= PCM1808_DEVICE_NAME_MAX ||
      pcm1808_audio.attached) {
    return pcm1808_audio.attached ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_ARG;
  }

  int mclk_pin = -1;
  int bck_pin = -1;
  int ws_pin = -1;
  int dout_pin = -1;
  for (size_t i = 0; i < binding_count; i++) {
    if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_GPIO) {
      continue;
    }
    if (strcmp(bindings[i].role, "mclk") == 0) {
      mclk_pin = bindings[i].value;
    } else if (strcmp(bindings[i].role, "bck") == 0) {
      bck_pin = bindings[i].value;
    } else if (strcmp(bindings[i].role, "ws") == 0) {
      ws_pin = bindings[i].value;
    } else if (strcmp(bindings[i].role, "dout") == 0) {
      dout_pin = bindings[i].value;
    }
  }
  if (mclk_pin < 0 || bck_pin < 0 || ws_pin < 0 || dout_pin < 0 ||
      mclk_pin == bck_pin || mclk_pin == ws_pin || mclk_pin == dout_pin ||
      bck_pin == ws_pin || bck_pin == dout_pin || ws_pin == dout_pin) {
    return ESP_ERR_INVALID_ARG;
  }

  const int i2s_port = pcm1808_i2s_port();
  if (i2s_port < 0) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  esp_err_t err = solar_os_resource_claim(SOLAR_OS_RESOURCE_I2S_PORT, i2s_port,
                                          -1, name, "pcm1808");
  if (err != ESP_OK) {
    return err;
  }

  memset(&pcm1808_audio, 0, sizeof(pcm1808_audio));
  pcm1808_audio.i2s_port = i2s_port;
  pcm1808_audio.mclk_pin = mclk_pin;
  pcm1808_audio.bck_pin = bck_pin;
  pcm1808_audio.ws_pin = ws_pin;
  pcm1808_audio.dout_pin = dout_pin;
  strlcpy(pcm1808_audio.id, name, sizeof(pcm1808_audio.id));
  const int stream_len =
      snprintf(pcm1808_audio.stream_id, sizeof(pcm1808_audio.stream_id),
               "%s.capture", name);
  if (stream_len < 0 || (size_t)stream_len >= sizeof(pcm1808_audio.stream_id)) {
    (void)solar_os_resource_release(SOLAR_OS_RESOURCE_I2S_PORT, i2s_port, -1,
                                    name);
    memset(&pcm1808_audio, 0, sizeof(pcm1808_audio));
    return ESP_ERR_INVALID_ARG;
  }
  pcm1808_audio.attached = true;

  err = pcm1808_register_stream(&pcm1808_audio);
  if (err != ESP_OK) {
    (void)solar_os_resource_release(SOLAR_OS_RESOURCE_I2S_PORT, i2s_port, -1,
                                    name);
    memset(&pcm1808_audio, 0, sizeof(pcm1808_audio));
    return err;
  }

  solar_os_audio_device_info_t info = {
      .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_INPUT,
      .native_format = pcm1808_native_format,
  };
  strlcpy(info.id, pcm1808_audio.id, sizeof(info.id));
  snprintf(info.name, sizeof(info.name),
           "PCM1808 I2S%d MCLK%d BCK%d WS%d DOUT%d", i2s_port, mclk_pin,
           bck_pin, ws_pin, dout_pin);
  strlcpy(info.provider, "expansion", sizeof(info.provider));
  strlcpy(info.capture_stream, pcm1808_audio.stream_id,
          sizeof(info.capture_stream));
  err = solar_os_audio_register_device(&info);
  if (err != ESP_OK) {
    (void)solar_os_stream_unregister(pcm1808_audio.stream_id);
    (void)solar_os_resource_release(SOLAR_OS_RESOURCE_I2S_PORT, i2s_port, -1,
                                    name);
    memset(&pcm1808_audio, 0, sizeof(pcm1808_audio));
  }
  return err;
}

esp_err_t solar_os_pcm1808_detach(const char *name) {
  if (name == NULL || !pcm1808_audio.attached ||
      strcmp(name, pcm1808_audio.id) != 0) {
    return ESP_ERR_NOT_FOUND;
  }
  esp_err_t err = solar_os_stream_unregister(pcm1808_audio.stream_id);
  if (err != ESP_OK) {
    return err;
  }
  err = solar_os_audio_unregister_device(pcm1808_audio.id);
  if (err != ESP_OK) {
    return err;
  }
  err = solar_os_resource_release(SOLAR_OS_RESOURCE_I2S_PORT,
                                  pcm1808_audio.i2s_port, -1,
                                  pcm1808_audio.id);
  if (err != ESP_OK) {
    return err;
  }
  memset(&pcm1808_audio, 0, sizeof(pcm1808_audio));
  return ESP_OK;
}
