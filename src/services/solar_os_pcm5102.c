#include "solar_os_pcm5102.h"

#include <stdio.h>
#include <string.h>

#include "pcm5102.h"
#include "solar_os_audio.h"
#include "solar_os_stream.h"

#define PCM5102_DEVICE_NAME_MAX 16U

typedef struct {
  bool attached;
  int i2s_port;
  int bck_pin;
  int din_pin;
  int rck_pin;
  uint8_t volume;
  char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
  char stream_id[SOLAR_OS_STREAM_ID_MAX];
} solar_os_pcm5102_device_t;

static solar_os_pcm5102_device_t pcm5102_audio;

static const solar_os_stream_audio_format_t pcm5102_native_format = {
    .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    .sample_rate = PCM5102_SAMPLE_RATE,
    .channels = 2U,
    .bits_per_sample = 16U,
    .frames_per_block = PCM5102_FRAMES_PER_BLOCK,
};

static bool pcm5102_requested_format_supported(
    const solar_os_stream_audio_format_t *format) {
  if (format == NULL) {
    return true;
  }
  return (format->sample_rate == 0U ||
          format->sample_rate == PCM5102_SAMPLE_RATE) &&
         (format->channels == 0U || format->channels == 1U ||
          format->channels == 2U) &&
         (format->bits_per_sample == 0U || format->bits_per_sample == 16U) &&
         format->sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE;
}

static esp_err_t
pcm5102_stream_open(void *user, const char *owner,
                    const solar_os_stream_open_options_t *options,
                    solar_os_stream_handle_t *handle) {
  (void)owner;
  solar_os_pcm5102_device_t *device = user;
  if (device == NULL || !device->attached || handle == NULL ||
      !pcm5102_requested_format_supported(
          options != NULL ? &options->requested_audio : NULL)) {
    return device != NULL && device->attached ? ESP_ERR_NOT_SUPPORTED
                                              : ESP_ERR_INVALID_STATE;
  }

  solar_os_stream_audio_format_t format = pcm5102_native_format;
  if (options != NULL && options->requested_audio.channels != 0U) {
    format.channels = options->requested_audio.channels;
  }
  const esp_err_t err =
      pcm5102_open(device->i2s_port, (gpio_num_t)device->bck_pin,
                   (gpio_num_t)device->din_pin,
                   (gpio_num_t)device->rck_pin);
  if (err == ESP_OK) {
    handle->context = device;
    handle->audio = format;
  }
  return err;
}

static void pcm5102_stream_close(void *user, solar_os_stream_handle_t *handle) {
  (void)user;
  pcm5102_close();
  handle->context = NULL;
}

static esp_err_t pcm5102_stream_write(void *user,
                                      solar_os_stream_handle_t *handle,
                                      const void *data, size_t len,
                                      uint32_t timeout_ms, size_t *written) {
  solar_os_pcm5102_device_t *device = user;
  if (device == NULL || handle == NULL || handle->context != device ||
      data == NULL || written == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  const size_t frame_bytes = handle->audio.channels * sizeof(int16_t);
  if (frame_bytes == 0U || len == 0U || (len % frame_bytes) != 0U) {
    return ESP_ERR_INVALID_SIZE;
  }
  size_t frames_written = 0U;
  const esp_err_t err =
      pcm5102_write_s16(data, len / frame_bytes, handle->audio.channels,
                        device->volume, timeout_ms, &frames_written);
  *written = frames_written * frame_bytes;
  return err;
}

static esp_err_t pcm5102_set_volume(void *user, uint8_t volume) {
  solar_os_pcm5102_device_t *device = user;
  if (device == NULL || !device->attached || volume > 100U) {
    return ESP_ERR_INVALID_ARG;
  }
  device->volume = volume;
  return ESP_OK;
}

static esp_err_t pcm5102_register_stream(solar_os_pcm5102_device_t *device) {
  solar_os_stream_driver_t driver = {
      .info =
          {
              .type = SOLAR_OS_STREAM_TYPE_AUDIO,
              .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
              .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
              .audio = pcm5102_native_format,
          },
      .open = pcm5102_stream_open,
      .close = pcm5102_stream_close,
      .write = pcm5102_stream_write,
      .user = device,
  };
  strlcpy(driver.info.id, device->stream_id, sizeof(driver.info.id));
  strlcpy(driver.info.provider, "i2s-output", sizeof(driver.info.provider));
  strlcpy(driver.info.device, device->id, sizeof(driver.info.device));
  strlcpy(driver.info.unit, "frames", sizeof(driver.info.unit));
  strlcpy(driver.info.format, "pcm-s16le", sizeof(driver.info.format));
  strlcpy(driver.info.summary, "I2S audio playback",
          sizeof(driver.info.summary));
  return solar_os_stream_register(&driver);
}

esp_err_t solar_os_pcm5102_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count) {
  if (name == NULL || bindings == NULL || binding_count == 0U ||
      strnlen(name, PCM5102_DEVICE_NAME_MAX) >= PCM5102_DEVICE_NAME_MAX ||
      pcm5102_audio.attached) {
    return pcm5102_audio.attached ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_ARG;
  }

  int i2s_port = -1;
  int bck_pin = -1;
  int din_pin = -1;
  int rck_pin = -1;
  for (size_t i = 0; i < binding_count; i++) {
    if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT) {
      i2s_port = bindings[i].value;
      continue;
    }
    if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_GPIO) {
      continue;
    }
    if (strcmp(bindings[i].role, "bck") == 0) {
      bck_pin = bindings[i].value;
    } else if (strcmp(bindings[i].role, "din") == 0) {
      din_pin = bindings[i].value;
    } else if (strcmp(bindings[i].role, "rck") == 0) {
      rck_pin = bindings[i].value;
    }
  }
  if (i2s_port < 0 || bck_pin < 0 || din_pin < 0 || rck_pin < 0 ||
      bck_pin == din_pin ||
      bck_pin == rck_pin || din_pin == rck_pin) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(&pcm5102_audio, 0, sizeof(pcm5102_audio));
  pcm5102_audio.bck_pin = bck_pin;
  pcm5102_audio.din_pin = din_pin;
  pcm5102_audio.rck_pin = rck_pin;
  pcm5102_audio.i2s_port = i2s_port;
  pcm5102_audio.volume = 70U;
  strlcpy(pcm5102_audio.id, name, sizeof(pcm5102_audio.id));
  const int stream_len =
      snprintf(pcm5102_audio.stream_id, sizeof(pcm5102_audio.stream_id),
               "%s.playback", name);
  if (stream_len < 0 || (size_t)stream_len >= sizeof(pcm5102_audio.stream_id)) {
    memset(&pcm5102_audio, 0, sizeof(pcm5102_audio));
    return ESP_ERR_INVALID_ARG;
  }
  pcm5102_audio.attached = true;

  esp_err_t err = pcm5102_register_stream(&pcm5102_audio);
  if (err != ESP_OK) {
    memset(&pcm5102_audio, 0, sizeof(pcm5102_audio));
    return err;
  }

  solar_os_audio_device_info_t info = {
      .capabilities =
          SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT | SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME,
      .native_format = pcm5102_native_format,
  };
  strlcpy(info.id, pcm5102_audio.id, sizeof(info.id));
  snprintf(info.name, sizeof(info.name), "I2S output I2S%d BCK%d DIN%d RCK%d",
           i2s_port, bck_pin, din_pin, rck_pin);
  strlcpy(info.provider, "expansion", sizeof(info.provider));
  strlcpy(info.playback_stream, pcm5102_audio.stream_id,
          sizeof(info.playback_stream));
  const solar_os_audio_device_ops_t ops = {
      .set_volume = pcm5102_set_volume,
  };
  err = solar_os_audio_register_device_ex(&info, &ops, &pcm5102_audio);
  if (err != ESP_OK) {
    (void)solar_os_stream_unregister(pcm5102_audio.stream_id);
    memset(&pcm5102_audio, 0, sizeof(pcm5102_audio));
  }
  return err;
}

esp_err_t solar_os_pcm5102_detach(const char *name) {
  if (name == NULL || !pcm5102_audio.attached ||
      strcmp(name, pcm5102_audio.id) != 0) {
    return ESP_ERR_NOT_FOUND;
  }
  esp_err_t err = solar_os_stream_unregister(pcm5102_audio.stream_id);
  if (err != ESP_OK) {
    return err;
  }
  err = solar_os_audio_unregister_device(pcm5102_audio.id);
  if (err != ESP_OK) {
    return err;
  }
  memset(&pcm5102_audio, 0, sizeof(pcm5102_audio));
  return ESP_OK;
}
