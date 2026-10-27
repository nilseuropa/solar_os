#include <assert.h>
#include <string.h>
#include "solar_os_audio_pdm.h"
#include "solar_os_audio.h"
#include "solar_os_stream.h"
#include "audio_pdm.h"

static solar_os_stream_driver_t stream;
static bool opened, registered;
static esp_err_t stream_error, device_error, unregister_error;
static unsigned opens;

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
esp_err_t audio_pdm_prepare_power(int pin) { assert(pin == 38); assert(!opened); return ESP_OK; }
esp_err_t audio_pdm_open(int clk, int data, int power, unsigned slot) {
    assert(clk == 19 && data == 20 && power == 38 && slot == 0 && !opened);
    opened = true; opens++; return ESP_OK;
}
esp_err_t audio_pdm_close(void) { opened = false; return ESP_OK; }
esp_err_t audio_pdm_read(void *data, size_t len, uint32_t timeout, size_t *read_len) {
    assert(opened && timeout == 100); memset(data, 0x21, len); *read_len = len; return ESP_OK;
}
esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *value) {
    assert(!strcmp(value->info.id, "mic0.capture"));
    assert(value->info.sharing == SOLAR_OS_STREAM_SHARING_EXCLUSIVE);
    assert(value->info.direction == SOLAR_OS_STREAM_DIRECTION_SOURCE && !value->write);
    stream = *value; registered = stream_error == ESP_OK; return stream_error;
}
esp_err_t solar_os_stream_unregister(const char *id) {
    assert(!strcmp(id, "mic0.capture") && registered);
    if (opened) return ESP_ERR_INVALID_STATE;
    registered = false; return ESP_OK;
}
esp_err_t solar_os_audio_register_device(const solar_os_audio_device_info_t *info) {
    assert(!strcmp(info->id, "mic0") && !strcmp(info->capture_stream, "mic0.capture"));
    assert(info->capabilities == SOLAR_OS_AUDIO_DEVICE_CAP_INPUT);
    assert(info->native_format.channels == 1 && info->native_format.sample_rate == 16000);
    return device_error;
}
esp_err_t solar_os_audio_unregister_device(const char *id) {
    assert(!strcmp(id, "mic0")); return unregister_error;
}
int main(void) {
    solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .value=0},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="clk", .value=19},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="din", .value=20},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="power", .value=38},
    };
    bindings[0].value = 1;
    assert(solar_os_audio_pdm_attach("mic0", bindings, 4) == ESP_ERR_INVALID_ARG);
    bindings[0].value = 0;
    stream_error = ESP_FAIL;
    assert(solar_os_audio_pdm_attach("mic0", bindings, 4) == ESP_FAIL && !registered && !opened);
    stream_error = ESP_OK; device_error = ESP_FAIL;
    assert(solar_os_audio_pdm_attach("mic0", bindings, 4) == ESP_FAIL && !registered && !opened);
    device_error = ESP_OK;
    assert(solar_os_audio_pdm_attach("mic0", bindings, 4) == ESP_OK && registered && !opened);
    assert(solar_os_audio_pdm_attach("mic0", bindings, 4) == ESP_ERR_INVALID_STATE);
    solar_os_stream_handle_t handle = {.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE};
    solar_os_stream_open_options_t options = {.requested_audio={.sample_rate=48000}};
    assert(stream.open(stream.user, "test", &options, &handle) == ESP_ERR_NOT_SUPPORTED && opens == 0);
    options.requested_audio.sample_rate = 16000;
    options.requested_audio.channels = 2;
    assert(stream.open(stream.user, "test", &options, &handle) == ESP_ERR_NOT_SUPPORTED && opens == 0);
    options.requested_audio.channels = 1;
    assert(stream.open(stream.user, "test", &options, &handle) == ESP_OK && opened);
    assert(handle.audio.bits_per_sample == 16 && handle.audio.channels == 1 && handle.audio.sample_rate == 16000);
    int16_t data[4]; size_t read_len;
    assert(stream.read(stream.user, &handle, data, sizeof(data), 100, &read_len) == ESP_OK);
    assert(read_len == sizeof(data) && data[0] == 0x2121);
    assert(solar_os_audio_pdm_detach("mic0") == ESP_ERR_INVALID_STATE && opened);
    stream.close(stream.user, &handle);
    assert(!opened && handle.context == NULL);
    unregister_error = ESP_FAIL;
    assert(solar_os_audio_pdm_detach("mic0") == ESP_FAIL && registered);
    unregister_error = ESP_OK;
    assert(solar_os_audio_pdm_detach("mic0") == ESP_OK && !registered);
    assert(solar_os_audio_pdm_detach("mic0") == ESP_ERR_NOT_FOUND);
    return 0;
}
