#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_controls_api.h"
#include "solar_os_native_audio_routes.h"
#include "solar_os_native_audio_routes_abi.h"
#include "solar_os_native_stream.h"
#include "solar_os_native_handles.h"
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_controls.h"
#include "solar_os_audio.h"
#include "freertos/FreeRTOS.h"
#include "audio_control_registry.inc"
static unsigned allocations;
static unsigned retained_leases;
static const solar_os_app_t fixture_app = {.name = "parameters-test"};
static bool app_scope;
const solar_os_app_t *solar_os_application_descriptor(void)
{ return app_scope ? &fixture_app : NULL; }
int solar_os_native_library_retain(uint32_t lease, uint32_t *retained)
{
    if (lease != 8) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    ++retained_leases;
    *retained = 12;
    return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_library_release(uint32_t lease)
{
    assert(lease == 12 && retained_leases == 1);
    --retained_leases;
    return SOLAR_OS_NATIVE_OK;
}
static float parameter_value = 1;
size_t strlcpy(char *out, const char *in, size_t capacity)
{ size_t size = strlen(in); if (capacity) { size_t n = size < capacity - 1 ? size : capacity - 1; memcpy(out, in, n); out[n] = 0; } return size; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); void *p = calloc(n, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
uint64_t solar_os_time_uptime_ms(void) { return 1; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *time) { *time = 0; return ESP_ERR_INVALID_STATE; }
static esp_err_t read_test_source(void *user, const solar_os_stream_read_options_t *options, float *value)
{ (void)user; (void)options; *value = 0; return ESP_OK; }
static esp_err_t get_parameter(void *user, float *out) { assert(user == &parameter_value); *out = parameter_value; return ESP_OK; }
static esp_err_t set_parameter(void *user, float value) { assert(user == &parameter_value); parameter_value = value; return ESP_OK; }

typedef struct {
    const solar_os_native_parameters_api_v1_t *api;
    solar_os_native_handle_t handle;
    float value;
    bool close_during_set;
} provider_fixture_t;

static int native_provider_get(void *user, float *out)
{
    provider_fixture_t *fixture = user;
    *out = fixture->value;
    return SOLAR_OS_NATIVE_OK;
}

static int native_provider_set(void *user, float value)
{
    provider_fixture_t *fixture = user;
    if (fixture->close_during_set) {
        assert(fixture->api->provider_close(fixture->handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
        assert(retained_leases == 1);
    }
    fixture->value = value;
    return SOLAR_OS_NATIVE_OK;
}

static void native_parameter_provider(const solar_os_native_parameters_api_v1_t *api)
{
    provider_fixture_t fixture = {.api = api, .value = 2.0f};
    char label[] = "Gain";
    solar_os_native_parameter_definition_v1_t definition = {
        .name = "gain", .label = label, .unit = "dB", .maximum = 8.0f, .step = 2.0f,
        .get = native_provider_get, .set = native_provider_set, .user = &fixture,
    };
    assert(api->provider_register("device.test", 0, &definition, 1, &fixture.handle) ==
           SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !fixture.handle);
    assert(api->provider_register("device.test", 9, &definition, 1, &fixture.handle) ==
           SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !fixture.handle);
    assert(!allocations && !retained_leases);
    assert(api->provider_register("device.test", 8, &definition, 1, &fixture.handle) == SOLAR_OS_NATIVE_OK);
    assert(retained_leases == 1);
    label[0] = 'X';
    solar_os_native_parameter_info_v1_t info;
    assert(api->find("device.test.gain", &info) == SOLAR_OS_NATIVE_OK && !strcmp(info.label, "Gain"));
    solar_os_native_handle_t claim;
    assert(api->claim("device.test.gain", "writer", &claim) == SOLAR_OS_NATIVE_OK);
    assert(api->provider_update_range(fixture.handle, "gain", 0.0f, 4.0f) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->release(claim) == SOLAR_OS_NATIVE_OK);
    assert(api->provider_update_range(fixture.handle, "gain", 0.0f, 4.0f) == SOLAR_OS_NATIVE_OK);
    assert(api->set("device.test.gain", 5.0f) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    fixture.close_during_set = true;
    assert(api->set("device.test.gain", 3.0f) == SOLAR_OS_NATIVE_OK && fixture.value == 4.0f);
    assert(api->find("device.test.gain", &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->provider_close(fixture.handle) == SOLAR_OS_NATIVE_OK);
    assert(!retained_leases && !allocations);

    app_scope = true;
    assert(api->provider_register("device.test", 0, &definition, 1, &fixture.handle) == SOLAR_OS_NATIVE_OK);
    app_scope = false;
    assert(solar_os_native_handle_close_owner(&fixture_app) == SOLAR_OS_NATIVE_OK);
    assert(api->provider_close(fixture.handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->count() && !allocations);
}
static bool try_audio_detach;
static float audio_gain;
static esp_err_t audio_gain_get(void *user, float *out)
{
    assert(user == &audio_gain);
    *out = audio_gain;
    return ESP_OK;
}

static esp_err_t audio_gain_set(void *user, float value)
{
    assert(user == &audio_gain);
    if (try_audio_detach) {
        /* Removal requested from an active callback must leave its state alive. */
        assert(solar_os_audio_unregister_device("Mic.1") == ESP_ERR_INVALID_STATE);
        assert(solar_os_audio_device_count() == 1);
    }
    audio_gain = value;
    return ESP_OK;
}

static void audio_device_parameters(const solar_os_native_parameters_api_v1_t *parameters)
{
    char path[64], owner[40];
    assert(parameters->resource_path("audio", "audio0", "input-gain", path, sizeof(path)) == 0);
    assert(strcmp(path, "audio.audio0.input-gain") == 0);
    assert(parameters->resource_path("audio", "Mic.1", NULL, owner, sizeof(owner)) == 0);
    assert(parameters->resource_path("audio", "Mic.1", "input-gain", path, sizeof(path)) == 0);
    assert(strcmp(path, "audio..4d69632e31.input-gain") == 0);
    assert(parameters->resource_path("audio", "Mic.1", "input-gain", owner, 8) ==
           SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && owner[0] == '\0');
    assert(parameters->resource_path("audio", "15 chars -----", "input-gain", path, sizeof(path)) == 0);
    assert(parameters->resource_path("audio", "Mic.1", "input-gain", path, sizeof(path)) == 0);

    const solar_os_audio_device_info_t device = {
        .id = "Mic.1", .name = "Test microphone", .provider = "test",
        .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_INPUT | SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN,
        .input_gain_min_db = 0.0f, .input_gain_max_db = 37.5f, .input_gain_step_db = 3.0f,
        .native_format = {.sample_rate = 16000, .channels = 2, .bits_per_sample = 16},
    };
    const solar_os_audio_device_ops_t ops = {.get_input_gain = audio_gain_get, .set_input_gain = audio_gain_set};
    assert(solar_os_audio_register_device_ex(&device, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(solar_os_audio_device_count() == 0 && parameters->count() == 0);
    assert(solar_os_audio_register_device_ex(&device, &ops, &audio_gain) == ESP_OK);
    assert(solar_os_audio_register_device_ex(&device, &ops, &audio_gain) == ESP_ERR_INVALID_STATE);
    solar_os_native_parameter_info_v1_t info;
    assert(parameters->find(path, &info) == 0 && strcmp(info.unit, "dB") == 0);
    assert(info.minimum == 0.0f && info.maximum == 37.5f && info.step == 3.0f);
    assert(parameters->set(path, 38.0f) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && audio_gain == 0.0f);
    solar_os_native_handle_t claim;
    assert(parameters->claim(path, "level-control", &claim) == 0);
    assert(parameters->set(path, 9.0f) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(parameters->set_owned(claim, 9.0f) == 0 && audio_gain == 9.0f);
    try_audio_detach = true;
    assert(parameters->set_owned(claim, 37.5f) == 0 && audio_gain == 37.5f);
    try_audio_detach = false;
    assert(parameters->find(path, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(solar_os_audio_unregister_device(device.id) == ESP_OK);
    assert(parameters->release(claim) == 0);
    assert(solar_os_audio_device_count() == 0 && parameters->count() == 0);
}

typedef struct {
    uint8_t volume;
    bool fail, probe_contention, refuse_volume, refuse_close;
} route_fixture_t;

static esp_err_t speaker_volume(void *user, uint8_t volume)
{
    route_fixture_t *fixture = user;
    if (fixture->probe_contention) {
        assert(solar_os_audio_set_default_output(NULL) == ESP_ERR_INVALID_STATE);
        assert(solar_os_audio_set_volume(90) == ESP_ERR_INVALID_STATE);
        assert(solar_os_audio_unregister_device("speaker") == ESP_ERR_INVALID_STATE);
    }
    if (fixture->fail) return ESP_FAIL;
    if (fixture->refuse_volume) return ESP_ERR_INVALID_STATE;
    fixture->volume = volume;
    return ESP_OK;
}

static esp_err_t speaker_open(void *user, const char *owner, const solar_os_stream_open_options_t *options,
    solar_os_stream_handle_t *handle)
{
    (void)user;
    assert(!strcmp(owner, "route-test") && options->direction == SOLAR_OS_STREAM_DIRECTION_SINK);
    assert(options->timeout_ms == 17 && options->requested_audio.sample_rate == 44100);
    handle->audio = options->requested_audio;
    return ESP_OK;
}

static esp_err_t speaker_write(void *user, solar_os_stream_handle_t *handle, const void *bytes, size_t size,
    uint32_t timeout, size_t *written)
{
    (void)user;
    assert(handle->audio.sample_rate == 44100 && timeout == 9 && size == 4 && !memcmp(bytes, "pcm", size));
    *written = size;
    return ESP_OK;
}

static esp_err_t speaker_close(void *user, solar_os_stream_handle_t *handle)
{
    route_fixture_t *fixture = user;
    assert(handle->direction == SOLAR_OS_STREAM_DIRECTION_SINK);
    return fixture->refuse_close ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void solar_os_audio_get_status(solar_os_audio_status_t *status)
{
    *status = (solar_os_audio_status_t){ .initialized = true, .sample_rate = 48000, .channels = 2,
        .bits_per_sample = 16, .output_codec = "Speaker", .i2s_port = 1, .mclk_pin = -1,
        .bclk_pin = 2, .ws_pin = 3, .dout_pin = 4, .din_pin = -1, .pa_pin = -1 };
}

static void audio_routes(void)
{
    const solar_os_native_audio_routes_api_v1_t *api = solar_os_native_audio_routes_get_service(
        SOLAR_OS_NATIVE_AUDIO_ROUTES_SERVICE, SOLAR_OS_NATIVE_AUDIO_ROUTES_ABI, sizeof(*api));
    assert(api && !api->count() && !api->output_available());
    assert(!solar_os_native_audio_routes_get_service(SOLAR_OS_NATIVE_AUDIO_ROUTES_SERVICE, 2, 8));
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    assert(!api->get_default_output(id, sizeof(id)) && !*id);
    assert(api->get_volume() == 50);
    solar_os_audio_board_status_v1_t board_status;
    assert(!api->board_status(&board_status) && board_status.initialized);
    assert(board_status.format.sample_rate == 48000 && board_status.i2s_port == 1);
    assert(board_status.mclk_pin == -1 && board_status.bclk_pin == 2);
    assert(!strcmp(board_status.output_codec, "Speaker") && !strcmp(board_status.input_codec, "-"));

    const solar_os_audio_device_info_t speaker = {
        .id = "speaker", .name = "Speaker", .provider = "test", .playback_stream = "speaker.pcm",
        .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT | SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME,
        .native_format = {.sample_rate = 48000, .channels = 2, .bits_per_sample = 16, .frames_per_block = 256},
    };
    route_fixture_t fixture = {.probe_contention = true};
    const solar_os_audio_device_ops_t ops = {.set_volume = speaker_volume};
    solar_os_stream_driver_t speaker_stream = {
        .info = {.id = "speaker.pcm", .provider = "test", .device = "speaker", .format = "s16le",
            .type = SOLAR_OS_STREAM_TYPE_AUDIO, .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE, .audio = speaker.native_format},
        .open = speaker_open, .write = speaker_write, .close_checked = speaker_close, .user = &fixture,
    };
    assert(!solar_os_stream_register(&speaker_stream));
    assert(!solar_os_audio_register_device_ex(&speaker, &ops, &fixture));
    assert(api->count() == 1 && api->output_available());
    solar_os_audio_route_v1_t by_index, by_id;
    assert(!api->info(0, &by_index) && !api->find("speaker", &by_id));
    assert(!strcmp(by_index.id, by_id.id) && !strcmp(by_id.playback_stream, "speaker.pcm"));
    assert(by_id.format.sample_rate == 48000 && by_id.format.frames_per_block == 256);
    assert(by_id.capabilities == speaker.capabilities && api->info(1, &by_index) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);

    fixture.fail = true;
    assert(api->set_default_output("speaker") == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!api->get_default_output(id, sizeof(id)) && !*id);
    fixture.fail = false;
    assert(!api->set_default_output("speaker") && fixture.volume == 50);
    assert(api->get_default_output(id, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !*id);
    assert(!api->get_default_output(id, sizeof(id)) && !strcmp(id, "speaker"));
    assert(!api->set_volume(NULL, 68) && fixture.volume == 68 && api->get_volume() == 68);
    uint8_t volume_after;
    assert(!api->toggle_mute(&volume_after) && volume_after == 0 && fixture.volume == 0);
    fixture.fail = true;
    assert(api->toggle_mute(&volume_after) == SOLAR_OS_NATIVE_ERROR_FAILED && volume_after == 0);
    fixture.fail = false;
    assert(!api->toggle_mute(&volume_after) && volume_after == 68 && fixture.volume == 68);
    fixture.probe_contention = false;
    assert(!api->set_volume("speaker", 23) && fixture.volume == 23 && api->get_volume() == 68);
    const solar_os_native_stream_api_v1_t *streams = solar_os_native_stream_get_service(
        "stream", 1, sizeof(*streams));
    assert(streams);
    const solar_os_native_stream_options_v1_t request = {
        .direction = SOLAR_OS_NATIVE_STREAM_SINK, .timeout_ms = 17,
        .format = {.sample_rate = 44100, .channels = 2, .bits_per_sample = 16},
    };
    solar_os_native_handle_t lease;
    assert(!api->open_default("route-test", &request, &lease) && streams->valid(lease));
    solar_os_native_stream_info_v1_t opened;
    assert(!streams->handle_info(lease, &opened) && !strcmp(opened.device, "speaker"));
    assert(opened.media.sample_rate == 44100 && opened.direction == SOLAR_OS_NATIVE_STREAM_SINK);
    size_t written;
    assert(!streams->write(lease, "pcm", 4, 9, &written) && written == 4);
    assert(solar_os_audio_unregister_device("speaker") == ESP_ERR_INVALID_STATE);

    route_fixture_t backup_fixture = {0};
    solar_os_audio_device_info_t backup = speaker;
    strcpy(backup.id, "backup");
    strcpy(backup.playback_stream, "backup.pcm");
    strcpy(speaker_stream.info.id, "backup.pcm");
    strcpy(speaker_stream.info.device, "backup");
    speaker_stream.user = &backup_fixture;
    assert(!solar_os_stream_register(&speaker_stream));
    assert(!solar_os_audio_register_device_ex(&backup, &ops, &backup_fixture));
    solar_os_native_handle_t other;
    assert(!api->open_default("route-test", &request, &other));
    assert(!streams->handle_info(other, &opened) && !strcmp(opened.device, "backup"));
    assert(!streams->close(other) && !streams->close(lease));

    fixture.refuse_volume = fixture.refuse_close = true;
    assert(api->open_default("route-test", &request, &lease) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(lease && !streams->valid(lease));
    assert(streams->write(lease, "pcm", 4, 9, &written) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !written);
    assert(streams->close(lease) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(solar_os_audio_unregister_device("speaker") == ESP_ERR_INVALID_STATE);
    fixture.refuse_volume = fixture.refuse_close = false;
    assert(!streams->close(lease));
    assert(!solar_os_audio_unregister_device("backup"));
    assert(!solar_os_stream_unregister("backup.pcm"));
    assert(!solar_os_audio_unregister_device("speaker") && !api->output_available());
    assert(!solar_os_stream_unregister("speaker.pcm"));
    assert(!api->get_default_output(id, sizeof(id)) && !*id);
    assert(!strcmp(by_id.playback_stream, "speaker.pcm"));
    puts("Audio routes: copied discovery, output selection, volume, mute and retryable detach passed");
}

static esp_err_t microphone_read(void *user, solar_os_stream_handle_t *handle, void *bytes, size_t size,
    uint32_t timeout, size_t *received)
{
    (void)user;
    assert(handle->audio.channels == 2 && size % 4 == 0 && (timeout == 1000 || timeout == UINT32_MAX));
    int16_t *samples = bytes;
    for (size_t i = 0; i < size / sizeof(*samples); ++i) samples[i] = i % 2 ? -20000 : 10000;
    *received = size;
    return ESP_OK;
}

static void audio_capture_routes(void)
{
    const solar_os_native_audio_routes_api_v1_t *routes = solar_os_native_audio_routes_get_service(
        SOLAR_OS_NATIVE_AUDIO_ROUTES_SERVICE, 1, sizeof(*routes));
    const solar_os_native_stream_api_v1_t *streams = solar_os_native_stream_get_service("stream", 1, sizeof(*streams));
    assert(routes && streams);
    const solar_os_audio_device_info_t device = {
        .id = "mic", .name = "Microphone", .capture_stream = "mic.pcm", .provider = "test",
        .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_INPUT,
        .native_format = {.sample_rate = 1000, .channels = 2, .bits_per_sample = 16, .frames_per_block = 2},
    };
    const solar_os_stream_driver_t driver = {
        .info = {.id = "mic.pcm", .device = "mic", .provider = "test", .format = "s16le",
            .type = SOLAR_OS_STREAM_TYPE_AUDIO, .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE, .audio = device.native_format},
        .read = microphone_read,
    };
    assert(!solar_os_stream_register(&driver));
    assert(!solar_os_audio_register_device_ex(&device, NULL, NULL));
    solar_os_stream_handle_t raw = SOLAR_OS_STREAM_HANDLE_INIT;
    assert(!solar_os_audio_open_default(SOLAR_OS_STREAM_DIRECTION_SOURCE, "capture-test", NULL, &raw, NULL));
    assert(solar_os_audio_open_default(SOLAR_OS_STREAM_DIRECTION_SOURCE, "capture-test", NULL, &raw, NULL)
        == ESP_ERR_INVALID_STATE && solar_os_stream_handle_valid(&raw));
    assert(!solar_os_stream_close_ex(&raw));
    const solar_os_native_stream_options_v1_t options = {.direction = SOLAR_OS_NATIVE_STREAM_SOURCE};
    solar_os_native_handle_t stream;
    assert(!routes->open_default("capture-test", &options, &stream));
    int16_t samples[10] = {0};
    assert(routes->capture_frames(stream, 5, samples, 9) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !samples[0]);
    assert(!routes->capture_frames(stream, 5, samples, 10));
    for (size_t i = 0; i < 10; ++i) assert(samples[i] == (i % 2 ? -20000 : 10000));
    solar_os_audio_level_t level;
    assert(!routes->measure_level(stream, 5, -1, &level));
    assert(level.samples == 10 && level.peak_percent == 61 && level.average_percent == 45);
    assert(!routes->measure_level(stream, 5, 1, &level));
    assert(level.samples == 5 && level.peak_percent == 61 && level.average_percent == 61);
    assert(routes->measure_level(stream, 5, 2, &level) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!streams->close(stream));
    assert(routes->capture_frames(stream, 5, samples, 10) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!solar_os_audio_unregister_device("mic") && !solar_os_stream_unregister("mic.pcm"));
}

typedef struct solar_os_board_display { uint8_t hardware; } solar_os_board_display_t;
static solar_os_board_display_t fixture_panel = {.hardware = 30};
static solar_os_board_display_t *display_handle = &fixture_panel;
static uint8_t display_brightness = 30, follow_brightness, saved_brightness;
static bool panel_supported, fail_panel_update;
static solar_os_parameter_registration_t display_brightness_registration;
static atomic_flag display_brightness_access = ATOMIC_FLAG_INIT;
#define SOLAR_OS_BOARD_HAS_DISPLAY 1
#define DISPLAY_BOARD_TARGET_NAME "display0"
#define SOLAR_OS_LOGW(tag, ...) ((void)(tag))
esp_err_t solar_os_display_set_brightness(uint8_t percent);
static bool solar_os_board_display_brightness_supported(const solar_os_board_display_t *panel)
{ return panel == &fixture_panel && panel_supported; }
static esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *panel, uint8_t *out)
{ *out = panel->hardware; return ESP_OK; }
static esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *panel, uint8_t value)
{
    if (fail_panel_update) return ESP_FAIL;
    panel->hardware = value;
    return ESP_OK;
}
static esp_err_t solar_os_backlight_display_changed(uint8_t value)
{
    assert(solar_os_display_set_brightness(2) == ESP_ERR_INVALID_STATE);
    follow_brightness = value;
    return ESP_OK;
}
static esp_err_t display_save_brightness(uint8_t value) { saved_brightness = value; return ESP_OK; }
#include "display_brightness_parameter.inc"

static void display_parameters(const solar_os_native_parameters_api_v1_t *api)
{
    char path[64];
    assert(!api->resource_path("display", "display0", "brightness", path, sizeof(path)));
    assert(!strcmp(path, DISPLAY_BRIGHTNESS_PARAMETER_PATH));
    solar_os_native_parameter_info_v1_t info;
    display_register_brightness();
    assert(api->find(path, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    panel_supported = true;
    display_register_brightness();
    display_register_brightness();
    assert(api->count() == 1 && !api->find(path, &info));
    assert(info.minimum == 0 && info.maximum == 100 && info.step == 1 && !strcmp(info.unit, "%"));
    uint8_t percent;
    assert(!solar_os_display_get_brightness(&percent) && percent == 30);
    solar_os_native_handle_t claim;
    assert(!api->claim(path, "app", &claim));
    assert(solar_os_display_set_brightness(34) == ESP_ERR_INVALID_STATE && fixture_panel.hardware == 30);
    assert(!api->set_owned(claim, 62.6f));
    assert(fixture_panel.hardware == 63 && follow_brightness == 63 && saved_brightness == 63);
    assert(!api->release(claim));
    assert(!solar_os_display_set_brightness(42));
    float readout;
    assert(!api->get(path, &readout) && readout == 42 && saved_brightness == 42);
    fail_panel_update = true;
    assert(api->set(path, 45) == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!api->get(path, &readout) && readout == 42 && saved_brightness == 42 && follow_brightness == 42);
    assert(!solar_os_parameters_unregister(&display_brightness_registration));
}

int main(void)
{
    const solar_os_stream_driver_t source = {
        .info = {.id = "source", .provider = "test", .format = "scalar"},
        .read_scalar = read_test_source,
    };
    assert(solar_os_stream_register(&source) == ESP_OK);
    const solar_os_native_parameters_api_v1_t *parameters = solar_os_native_controls_api_get_service("parameters", 1, sizeof(*parameters)); assert(parameters);
    audio_device_parameters(parameters);
    audio_routes();
    audio_capture_routes();
    display_parameters(parameters);
    native_parameter_provider(parameters);
    const solar_os_native_controls_api_v1_t *controls = solar_os_native_controls_api_get_service("controls", 1, sizeof(*controls)); assert(controls);
    solar_os_parameter_definition_t definition = {.name = "gain", .label = "Gain", .unit = "dB", .minimum = 1, .maximum = 100,
        .step = 0, .curve = SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC, .get = get_parameter, .set = set_parameter, .user = &parameter_value};
    solar_os_parameter_registration_t registration = SOLAR_OS_PARAMETER_REGISTRATION_INIT;
    assert(!solar_os_parameters_register("test", &definition, 1, &registration));
    solar_os_native_parameter_info_v1_t info; assert(parameters->count() == 1 && !parameters->find("test.gain", &info));
    assert(info.curve == SOLAR_OS_NATIVE_PARAMETER_LOGARITHMIC && info.minimum == 1 && info.maximum == 100 && !strcmp(info.unit, "dB"));
    assert(!parameters->set_normalized("test.gain", 65535) && parameter_value == 100);
    assert(parameters->set_normalized("test.gain", 65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && parameter_value == 100);
    solar_os_native_handle_t claim, other;
    assert(!parameters->claim("test.gain", "writer", &claim));
    assert(parameters->claim("test.gain", "writer", &other) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other);
    assert(parameters->set("test.gain", 10) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!parameters->set_owned(claim, 10) && parameter_value == 10);
    uint32_t normalized; assert(!parameters->get_normalized("test.gain", &normalized) && normalized >= 32767 && normalized <= 32768);
    assert(!solar_os_parameters_unregister(&registration));
    assert(!solar_os_parameters_register("test", &definition, 1, &registration));
    assert(!parameters->claim("test.gain", "writer", &other));
    assert(parameters->set_owned(claim, 20) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && parameter_value == 10);
    assert(!parameters->release(claim));
    assert(parameters->set("test.gain", 20) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!parameters->set_owned(other, 20) && parameter_value == 20); assert(!parameters->release(other));
    solar_os_native_control_config_v1_t config = {.name = "knob", .source = "source", .input_minimum = -1, .input_maximum = 1, .inverted = 1};
    assert(!controls->create(&config) && controls->count() == 1);
    memset(config.source, '?', sizeof(config.source)); assert(controls->create(&config) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(controls->set("knob", 65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!controls->set("knob", 50000)); uint32_t value; assert(!controls->get("knob", &value) && value == 50000);
    solar_os_native_control_info_v1_t control; assert(!controls->find("knob", &control) && control.config.inverted && !strcmp(control.config.source, "source"));
    uint32_t binding; assert(!controls->bind_parameter("knob", "test.gain", true, &binding));
    solar_os_control_binding_note(binding, 123, true, true, 10, 11, ESP_ERR_TIMEOUT, false);
    solar_os_native_control_binding_v1_t binding_info; assert(!controls->binding_info(0, &binding_info));
    assert(binding_info.id == binding && binding_info.target == SOLAR_OS_NATIVE_CONTROL_PARAMETER && binding_info.pickup_latched && binding_info.pickup_seen);
    assert(binding_info.last_generation == 0 && binding_info.pickup_previous == 10 && binding_info.last_target_value == 11 && binding_info.error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    solar_os_control_binding_note(binding, 123, true, true, 10, 11, ESP_OK, true);
    assert(!controls->binding_info(0, &binding_info) && binding_info.last_generation == 123 && binding_info.applied == 1 && binding_info.errors == 1);
    assert(controls->bind_midi_cc("knob", 256, 1, &binding) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !binding);
    assert(!controls->bind_midi_cc("knob", 1, 127, &binding) && controls->binding_count() == 2);
    assert(!controls->binding_info(1, &binding_info) && binding_info.target == SOLAR_OS_NATIVE_CONTROL_MIDI_CC && binding_info.midi_controller == 127);
    size_t removed; assert(!controls->unbind("knob", &removed) && removed == 2);
    assert(!controls->delete_control("knob") && !controls->count()); assert(!solar_os_parameters_unregister(&registration));
    assert(!allocations && !solar_os_native_handle_count()); puts("native parameters and controls with real registries: values, claims and bindings passed"); return 0;
}
