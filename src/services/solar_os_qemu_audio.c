#include "solar_os_qemu_audio.h"
#include "solar_os_qemu_audio_protocol.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "solar_os_audio.h"
#include "solar_os_stream.h"

static struct {
    bool attached, opened;
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    char stream_id[SOLAR_OS_STREAM_ID_MAX];
} device;

static const solar_os_stream_audio_format_t native_format = {
    .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    .sample_rate = SOLAR_OS_QEMU_AUDIO_RATE,
    .channels = 2, .bits_per_sample = 16, .frames_per_block = 256,
};

static uint32_t read_reg(unsigned offset)
{ return REG_READ(SOLAR_OS_QEMU_AUDIO_BASE + offset); }
static void write_reg(unsigned offset, uint32_t value)
{ REG_WRITE(SOLAR_OS_QEMU_AUDIO_BASE + offset, value); }

static void wait_for_audio(void)
{
    TickType_t ticks = pdMS_TO_TICKS(2);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t set_volume(void *user, uint8_t volume)
{
    (void)user;
    if (!device.attached) return ESP_ERR_INVALID_STATE;
    if (volume > 100) return ESP_ERR_INVALID_ARG;
    write_reg(SOLAR_OS_QEMU_AUDIO_REG_VOLUME, volume);
    return read_reg(SOLAR_OS_QEMU_AUDIO_REG_ERROR) ? ESP_FAIL : ESP_OK;
}

static esp_err_t stream_open(void *user, const char *owner,
    const solar_os_stream_open_options_t *options, solar_os_stream_handle_t *handle)
{
    (void)user; (void)owner;
    if (!device.attached || device.opened) return ESP_ERR_INVALID_STATE;
    solar_os_stream_audio_format_t format = native_format;
    if (options) {
        const solar_os_stream_audio_format_t *requested = &options->requested_audio;
        if (requested->sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
            (requested->sample_rate && requested->sample_rate != native_format.sample_rate) ||
            (requested->bits_per_sample && requested->bits_per_sample != 16) ||
            requested->channels > 2) return ESP_ERR_NOT_SUPPORTED;
        if (requested->channels) format.channels = requested->channels;
    }
    write_reg(SOLAR_OS_QEMU_AUDIO_REG_CONTROL, SOLAR_OS_QEMU_AUDIO_START);
    if (read_reg(SOLAR_OS_QEMU_AUDIO_REG_ERROR) || !read_reg(SOLAR_OS_QEMU_AUDIO_REG_ACTIVE)) return ESP_FAIL;
    device.opened = true;
    handle->context = &device;
    handle->audio = format;
    return ESP_OK;
}

static esp_err_t stream_write(void *user, solar_os_stream_handle_t *handle,
    const void *data, size_t length, uint32_t timeout_ms, size_t *written)
{
    (void)user;
    if (!data || !written || !handle || handle->context != &device || !device.opened) return ESP_ERR_INVALID_ARG;
    *written = 0;
    const unsigned channels = handle->audio.channels;
    const unsigned frame_bytes = channels * sizeof(int16_t);
    if (!frame_bytes || length % frame_bytes) return ESP_ERR_INVALID_SIZE;
    const size_t frames = length / frame_bytes;
    const uint8_t *bytes = data;
    const int64_t start = esp_timer_get_time();
    size_t done = 0;
    while (done < frames) {
        size_t available = read_reg(SOLAR_OS_QEMU_AUDIO_REG_FREE);
        if (!available) {
            if (read_reg(SOLAR_OS_QEMU_AUDIO_REG_ERROR) || !read_reg(SOLAR_OS_QEMU_AUDIO_REG_ACTIVE)) return ESP_FAIL;
            if (timeout_ms != UINT32_MAX &&
                (uint64_t)(esp_timer_get_time() - start) >= (uint64_t)timeout_ms * 1000) return ESP_ERR_TIMEOUT;
            wait_for_audio();
            continue;
        }
        size_t count = frames - done;
        if (count > available) count = available;
        if (count > SOLAR_OS_QEMU_AUDIO_STAGE_FRAMES) count = SOLAR_OS_QEMU_AUDIO_STAGE_FRAMES;
        for (size_t i = 0; i < count; i++) {
            uint32_t frame;
            if (channels == 1) {
                uint16_t sample;
                memcpy(&sample, bytes + (done + i) * frame_bytes, sizeof(sample));
                frame = sample | ((uint32_t)sample << 16);
            } else memcpy(&frame, bytes + (done + i) * frame_bytes, sizeof(frame));
            write_reg(SOLAR_OS_QEMU_AUDIO_REG_DATA + i * sizeof(frame), frame);
        }
        write_reg(SOLAR_OS_QEMU_AUDIO_REG_COMMIT, count);
        if (read_reg(SOLAR_OS_QEMU_AUDIO_REG_ERROR)) return ESP_FAIL;
        done += count;
        *written = done * frame_bytes;
    }
    return ESP_OK;
}

static void stream_close(void *user, solar_os_stream_handle_t *handle)
{
    (void)user;
    if (!device.opened || handle->context != &device) return;
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (read_reg(SOLAR_OS_QEMU_AUDIO_REG_QUEUED) && esp_timer_get_time() < deadline) wait_for_audio();
    write_reg(SOLAR_OS_QEMU_AUDIO_REG_CONTROL, SOLAR_OS_QEMU_AUDIO_STOP);
    while (read_reg(SOLAR_OS_QEMU_AUDIO_REG_ACTIVE) && esp_timer_get_time() < deadline) wait_for_audio();
    if (read_reg(SOLAR_OS_QEMU_AUDIO_REG_ACTIVE))
        write_reg(SOLAR_OS_QEMU_AUDIO_REG_CONTROL, SOLAR_OS_QEMU_AUDIO_RESET);
    handle->context = NULL;
    device.opened = false;
}

esp_err_t solar_os_qemu_audio_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t binding_count)
{
    (void)bindings;
    if (!name || !name[0] || strlen(name) >= sizeof(device.id) || binding_count) return ESP_ERR_INVALID_ARG;
    if (device.attached) return ESP_ERR_INVALID_STATE;
    if (read_reg(SOLAR_OS_QEMU_AUDIO_REG_MAGIC) != SOLAR_OS_QEMU_AUDIO_MAGIC ||
        read_reg(SOLAR_OS_QEMU_AUDIO_REG_VERSION) != SOLAR_OS_QEMU_AUDIO_VERSION ||
        !read_reg(SOLAR_OS_QEMU_AUDIO_REG_READY)) return ESP_ERR_NOT_SUPPORTED;
    memset(&device, 0, sizeof(device));
    strcpy(device.id, name);
    snprintf(device.stream_id, sizeof(device.stream_id), "%s.playback", name);
    write_reg(SOLAR_OS_QEMU_AUDIO_REG_CONTROL, SOLAR_OS_QEMU_AUDIO_RESET);
    solar_os_stream_driver_t driver = {
        .info = {.type = SOLAR_OS_STREAM_TYPE_AUDIO, .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE, .audio = native_format},
        .open = stream_open, .close = stream_close, .write = stream_write, .user = &device,
    };
    strcpy(driver.info.id, device.stream_id);
    strcpy(driver.info.provider, "qemu-audio");
    strcpy(driver.info.device, device.id);
    strcpy(driver.info.unit, "frames");
    strcpy(driver.info.format, "pcm-s16le");
    strcpy(driver.info.summary, "QEMU host audio playback");
    esp_err_t err = solar_os_stream_register(&driver);
    if (err != ESP_OK) return err;
    solar_os_audio_device_info_t info = {
        .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT | SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME,
        .native_format = native_format,
    };
    strcpy(info.id, device.id);
    strcpy(info.name, "QEMU host audio output");
    strcpy(info.provider, "qemu");
    strcpy(info.playback_stream, device.stream_id);
    const solar_os_audio_device_ops_t ops = {.set_volume = set_volume};
    device.attached = true;
    err = solar_os_audio_register_device_ex(&info, &ops, &device);
    if (err != ESP_OK) {
        solar_os_stream_unregister(device.stream_id);
        memset(&device, 0, sizeof(device));
    } else ESP_LOGI("qemu_audio", "%s attached: 48000 Hz, 16-bit stereo output", name);
    return err;
}

esp_err_t solar_os_qemu_audio_detach(const char *name)
{
    if (!name || !device.attached || strcmp(name, device.id)) return ESP_ERR_NOT_FOUND;
    esp_err_t err = solar_os_stream_unregister(device.stream_id);
    if (err != ESP_OK) return err;
    err = solar_os_audio_unregister_device(device.id);
    if (err != ESP_OK) return err;
    write_reg(SOLAR_OS_QEMU_AUDIO_REG_CONTROL, SOLAR_OS_QEMU_AUDIO_RESET);
    memset(&device, 0, sizeof(device));
    return ESP_OK;
}

const solar_os_expansion_driver_t solar_os_qemu_audio_expansion_driver = {
    .name = "qemu-audio", .category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary = "QEMU host audio output",
    .required_capabilities = SOLAR_OS_BOARD_CAP_QEMU | SOLAR_OS_BOARD_CAP_AUDIO,
    .attach = solar_os_qemu_audio_attach, .detach = solar_os_qemu_audio_detach,
};
