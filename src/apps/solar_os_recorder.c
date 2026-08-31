#include "solar_os_recorder.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_board_caps.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_media_widgets.h"
#include "solar_os_shell_io.h"
#include "solar_os_signal_widgets.h"
#include "solar_os_storage.h"
#include "solar_os_storage_browser.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define RECORDER_TASK_STACK 8192U
#define RECORDER_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define RECORDER_TICK_MS 40U
#define RECORDER_VISUAL_REFRESH_MS 40U
#define RECORDER_INPUT_REFRESH_MS 1000U
#define RECORDER_TICK_DEADLINE_MS 120U
#define RECORDER_WORKER_POLL_MS 20U
#define RECORDER_DISPLAY_HPM_HZ_TENTHS 255U
#define RECORDER_SCOPE_SAMPLES 256U
#define RECORDER_SPECTRUM_FFT_SIZE 256U
#define RECORDER_HEADER_HEIGHT 28
#define RECORDER_ROW_HEIGHT 24
#define RECORDER_FOOTER_HEIGHT 20
#define RECORDER_VOLUME_STEP 5
#define RECORDER_INPUT_MAX 8U
#define RECORDER_FILENAME_MAX 64U
#define RECORDER_SETUP_ROWS 8U
#define RECORDER_SETTINGS_MAGIC 0x52454353U
#define RECORDER_SETTINGS_VERSION 1U
#define RECORDER_SETTINGS_DIR ".recorder"
#define RECORDER_SETTINGS_FILE "settings.bin"
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(RECORDER_TASK_STACK);

typedef enum { RECORDER_MODE_TUI, RECORDER_MODE_GRAPHICS } recorder_mode_t;
typedef enum {
    RECORDER_TAB_RECORD,
    RECORDER_TAB_SETUP,
    RECORDER_TAB_COUNT,
} recorder_tab_t;
typedef enum {
    RECORDER_VISUALIZER_CASSETTE,
    RECORDER_VISUALIZER_SCOPE,
    RECORDER_VISUALIZER_SPECTRUM,
    RECORDER_VISUALIZER_COUNT,
} recorder_visualizer_t;
typedef enum {
    RECORDER_IDLE,
    RECORDER_RECORDING,
    RECORDER_MONITORING,
    RECORDER_PAUSED,
    RECORDER_PLAYING,
    RECORDER_ERROR,
} recorder_operation_state_t;
typedef enum {
    RECORDER_WORK_RECORD,
    RECORDER_WORK_MONITOR,
    RECORDER_WORK_PLAY,
} recorder_work_t;
typedef enum {
    RECORDER_BROWSER_NONE,
    RECORDER_BROWSER_DIRECTORY,
} recorder_browser_mode_t;

typedef struct {
    solar_os_stream_info_t stream;
    solar_os_audio_device_info_t device;
    bool has_device;
} recorder_input_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sample_rate;
    float input_gain_db;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
    uint8_t visualizer;
    uint8_t input_gain_valid;
    uint8_t reserved[3];
    char input_id[SOLAR_OS_STREAM_ID_MAX];
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
} recorder_settings_store_t;

typedef struct {
    recorder_mode_t mode;
    recorder_tab_t tab;
    recorder_visualizer_t visualizer;
    recorder_operation_state_t operation_state;
    recorder_work_t work;
    recorder_browser_mode_t browser_mode;
    solar_os_tui_t tui;
    solar_os_shell_io_t fallback_io;
    solar_os_storage_browser_t *browser;
    solar_os_cassette_widget_t *cassette;
    solar_os_oscilloscope_widget_t *scope;
    solar_os_spectrum_widget_t *spectrum;
    recorder_input_t inputs[RECORDER_INPUT_MAX];
    size_t input_count;
    size_t input_index;
    size_t setup_cursor;
    size_t browser_top;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
    float input_gain_db;
    bool input_gain_available;
    bool editing_filename;
    bool ui_started;
    bool suspended;
    bool high_refresh_active;
    bool redraw;
    bool natural_completion;
    bool saved_input_gain_valid;
    volatile bool stop_requested;
    volatile bool paused;
    volatile bool monitor_enabled;
    volatile bool task_done;
    TaskHandle_t task;
    uint32_t elapsed_ms;
    uint32_t data_bytes;
    uint32_t last_visualizer_ms;
    uint32_t last_input_refresh_ms;
    uint32_t internal_free_before;
    float saved_input_gain_db;
    esp_err_t worker_error;
    char filename[RECORDER_FILENAME_MAX];
    char edit_filename[RECORDER_FILENAME_MAX];
    size_t edit_filename_len;
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    char preferred_input_id[SOLAR_OS_STREAM_ID_MAX];
    char active_path[SOLAR_OS_STORAGE_PATH_MAX];
    char active_output_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    uint32_t active_output_capabilities;
    char message[96];
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
} recorder_state_t;

static const char *TAG = "solar_os_recorder";
static recorder_state_t *recorder_state;
#define recorder (*recorder_state)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("audio worker callback spinlock")
static portMUX_TYPE recorder_lock = portMUX_INITIALIZER_UNLOCKED;

static const uint32_t recorder_sample_rates[] = {
    8000U, 16000U, 22050U, 32000U, 44100U, 48000U,
};

static void recorder_log_internal_memory(const char *phase, uint32_t baseline)
{
    const uint32_t free_now = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    SOLAR_OS_LOGI(TAG,
                  "memory %s internal_free=%" PRIu32
                  " delta=%" PRId32 " largest=%" PRIu32,
                  phase, free_now, (int32_t)free_now - (int32_t)baseline,
                  largest);
}

static bool recorder_sample_rate_valid(uint32_t sample_rate)
{
    for (size_t i = 0U; i < sizeof(recorder_sample_rates) /
         sizeof(recorder_sample_rates[0]); i++) {
        if (recorder_sample_rates[i] == sample_rate) {
            return true;
        }
    }
    return false;
}

static esp_err_t recorder_settings_path(bool create_directory,
                                        char *path, size_t path_len)
{
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_default_path(
        RECORDER_SETTINGS_DIR, directory, sizeof(directory));
    if (err != ESP_OK) {
        return err;
    }
    if (create_directory &&
        solar_os_storage_mkdir(directory) != ESP_OK && errno != EEXIST) {
        return ESP_FAIL;
    }
    return solar_os_storage_join_path(
        directory, RECORDER_SETTINGS_FILE, path, path_len);
}

static bool recorder_settings_directory_valid(const char *directory)
{
    if (directory == NULL || directory[0] == '\0') {
        return false;
    }
    char stored_root[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    struct stat info;
    return solar_os_storage_path_mount_point(
               directory, stored_root, sizeof(stored_root)) == ESP_OK &&
        strcmp(stored_root, solar_os_storage_mount_point()) == 0 &&
        stat(directory, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool recorder_load_settings(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (recorder_settings_path(false, path, sizeof(path)) != ESP_OK) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    recorder_settings_store_t store;
    const bool valid = fread(&store, sizeof(store), 1U, file) == 1U &&
        store.magic == RECORDER_SETTINGS_MAGIC &&
        store.version == RECORDER_SETTINGS_VERSION &&
        store.size == (uint16_t)sizeof(store) &&
        recorder_sample_rate_valid(store.sample_rate) &&
        (store.channels == 1U || store.channels == 2U) &&
        (store.bits_per_sample == 8U || store.bits_per_sample == 16U) &&
        store.volume <= 100U &&
        store.visualizer < RECORDER_VISUALIZER_COUNT &&
        memchr(store.input_id, '\0', sizeof(store.input_id)) != NULL &&
        memchr(store.directory, '\0', sizeof(store.directory)) != NULL;
    fclose(file);
    if (!valid) {
        SOLAR_OS_LOGW(TAG, "settings ignored: invalid %s", path);
        return false;
    }
    recorder.sample_rate = store.sample_rate;
    recorder.channels = store.channels;
    recorder.bits_per_sample = store.bits_per_sample;
    recorder.volume = store.volume;
    recorder.visualizer = (recorder_visualizer_t)store.visualizer;
    recorder.saved_input_gain_valid = store.input_gain_valid != 0U;
    recorder.saved_input_gain_db = store.input_gain_db;
    strlcpy(recorder.preferred_input_id, store.input_id,
            sizeof(recorder.preferred_input_id));
    if (recorder_settings_directory_valid(store.directory)) {
        strlcpy(recorder.directory, store.directory,
                sizeof(recorder.directory));
    }
    SOLAR_OS_LOGI(TAG, "settings loaded %s", path);
    return true;
}

static esp_err_t recorder_save_settings(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = recorder_settings_path(true, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    recorder_settings_store_t store = {
        .magic = RECORDER_SETTINGS_MAGIC,
        .version = RECORDER_SETTINGS_VERSION,
        .size = (uint16_t)sizeof(store),
        .sample_rate = recorder.sample_rate,
        .input_gain_db = recorder.saved_input_gain_valid ?
            recorder.saved_input_gain_db : recorder.input_gain_db,
        .channels = recorder.channels,
        .bits_per_sample = recorder.bits_per_sample,
        .volume = recorder.volume,
        .visualizer = recorder.visualizer,
        .input_gain_valid = recorder.saved_input_gain_valid ||
            recorder.input_gain_available ? 1U : 0U,
    };
    strlcpy(store.input_id,
            recorder.preferred_input_id[0] != '\0' ?
                recorder.preferred_input_id :
                (recorder.input_index < recorder.input_count ?
                    recorder.inputs[recorder.input_index].stream.id : ""),
            sizeof(store.input_id));
    strlcpy(store.directory, recorder.directory, sizeof(store.directory));

    char temporary[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
            (int)sizeof(temporary) ||
        snprintf(backup, sizeof(backup), "%s.bak", path) >=
            (int)sizeof(backup)) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    err = fwrite(&store, sizeof(store), 1U, file) == 1U &&
        fflush(file) == 0 && fsync(fileno(file)) == 0 ? ESP_OK : ESP_FAIL;
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        struct stat info;
        const bool had_active = stat(path, &info) == 0;
        (void)solar_os_storage_remove(backup);
        if (had_active) {
            err = solar_os_storage_rename(path, backup);
        }
        if (err == ESP_OK) {
            err = solar_os_storage_rename(temporary, path);
        }
        if (err != ESP_OK && had_active) {
            (void)solar_os_storage_rename(backup, path);
        }
        if (err == ESP_OK) {
            (void)solar_os_storage_remove(backup);
        }
    }
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(temporary);
    } else {
        SOLAR_OS_LOGI(TAG, "settings saved %s", path);
    }
    return err;
}

static const char *recorder_basename(const char *path)
{
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    return slash != NULL ? slash + 1U : (path != NULL ? path : "");
}

static bool recorder_wav_file(const char *name, void *user)
{
    (void)user;
    const char *dot = name != NULL ? strrchr(name, '.') : NULL;
    return dot != NULL && strcasecmp(dot, ".wav") == 0;
}

static solar_os_shell_io_t *recorder_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&recorder.fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &recorder.fallback_io);
        io = &recorder.fallback_io;
    }
    return io;
}

static bool recorder_graphical_session(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = recorder_io(ctx);
    return solar_os_context_gfx(ctx) != NULL &&
        (io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT);
}

static void recorder_set_message(const char *message)
{
    strlcpy(recorder.message, message != NULL ? message : "",
            sizeof(recorder.message));
    recorder.redraw = true;
}

static void recorder_enable_high_refresh(solar_os_context_t *ctx)
{
    if (recorder.high_refresh_active) {
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || !solar_os_gfx_display_target_name(
            gfx, recorder.display_target, sizeof(recorder.display_target))) {
        return;
    }
    if (solar_os_display_set_high_refresh_override(
            recorder.display_target, true,
            RECORDER_DISPLAY_HPM_HZ_TENTHS) == ESP_OK) {
        recorder.high_refresh_active = true;
    }
}

static void recorder_disable_high_refresh(void)
{
    if (recorder.high_refresh_active) {
        (void)solar_os_display_set_high_refresh_override(
            recorder.display_target, false,
            RECORDER_DISPLAY_HPM_HZ_TENTHS);
        recorder.high_refresh_active = false;
    }
}

static void recorder_refresh_gain(void)
{
    recorder.input_gain_available = false;
    if (recorder.input_index >= recorder.input_count ||
        !recorder.inputs[recorder.input_index].has_device) {
        return;
    }
    const recorder_input_t *input = &recorder.inputs[recorder.input_index];
    if ((input->device.capabilities &
         SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN) != 0U &&
        solar_os_audio_get_device_input_gain(
            input->device.id, &recorder.input_gain_db) == ESP_OK) {
        recorder.input_gain_available = true;
    }
}

static void recorder_refresh_inputs(void)
{
    char selected[SOLAR_OS_STREAM_ID_MAX] = "";
    if (recorder.preferred_input_id[0] != '\0') {
        strlcpy(selected, recorder.preferred_input_id, sizeof(selected));
    } else if (recorder.input_index < recorder.input_count) {
        strlcpy(selected, recorder.inputs[recorder.input_index].stream.id,
                sizeof(selected));
    }
    recorder.input_count = 0U;
    const size_t count = solar_os_stream_count();
    for (size_t i = 0U; i < count &&
         recorder.input_count < RECORDER_INPUT_MAX; i++) {
        solar_os_stream_info_t stream;
        if (!solar_os_stream_get(i, &stream) ||
            stream.type != SOLAR_OS_STREAM_TYPE_AUDIO ||
            (stream.direction != SOLAR_OS_STREAM_DIRECTION_SOURCE &&
             stream.direction != SOLAR_OS_STREAM_DIRECTION_DUPLEX)) {
            continue;
        }
        recorder_input_t *input = &recorder.inputs[recorder.input_count++];
        memset(input, 0, sizeof(*input));
        input->stream = stream;
        const size_t device_count = solar_os_audio_device_count();
        for (size_t device_index = 0U; device_index < device_count;
             device_index++) {
            solar_os_audio_device_info_t device;
            if (solar_os_audio_device_get(device_index, &device) &&
                strcmp(device.capture_stream, stream.id) == 0) {
                input->device = device;
                input->has_device = true;
                break;
            }
        }
    }
    recorder.input_index = 0U;
    for (size_t i = 0U; i < recorder.input_count; i++) {
        if ((selected[0] != '\0' &&
             strcmp(recorder.inputs[i].stream.id, selected) == 0) ||
            (selected[0] == '\0' &&
             strcmp(recorder.inputs[i].stream.id, "audio0.capture") == 0)) {
            recorder.input_index = i;
            break;
        }
    }
    if (recorder.input_index < recorder.input_count &&
        (selected[0] == '\0' ||
         strcmp(recorder.inputs[recorder.input_index].stream.id, selected) == 0)) {
        strlcpy(recorder.preferred_input_id,
                recorder.inputs[recorder.input_index].stream.id,
                sizeof(recorder.preferred_input_id));
    }
    recorder_refresh_gain();
}

static bool recorder_build_generated_name(char *name, size_t name_len,
                                          unsigned suffix)
{
    const time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) == NULL) {
        return false;
    }
    const int written = suffix == 0U ?
        snprintf(name, name_len, "rec-%02d%02d%02d-%02d%02d%02d.wav",
                 (local.tm_year + 1900) % 100, local.tm_mon + 1,
                 local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec) :
        snprintf(name, name_len, "rec-%02d%02d%02d-%02d%02d%02d-%u.wav",
                 (local.tm_year + 1900) % 100, local.tm_mon + 1,
                 local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
                 suffix);
    return written > 0 && (size_t)written < name_len;
}

static esp_err_t recorder_build_recording_path(char *path, size_t path_len)
{
    if (recorder.filename[0] != '\0') {
        char filename[RECORDER_FILENAME_MAX];
        strlcpy(filename, recorder.filename, sizeof(filename));
        if (strrchr(filename, '.') == NULL) {
            strlcat(filename, ".wav", sizeof(filename));
        }
        return solar_os_storage_join_path(recorder.directory, filename,
                                          path, path_len);
    }
    struct stat status;
    for (unsigned suffix = 0U; suffix < 100U; suffix++) {
        char filename[RECORDER_FILENAME_MAX];
        if (!recorder_build_generated_name(filename, sizeof(filename), suffix)) {
            return ESP_FAIL;
        }
        esp_err_t err = solar_os_storage_join_path(
            recorder.directory, filename, path, path_len);
        if (err != ESP_OK) {
            return err;
        }
        if (stat(path, &status) != 0) {
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

static bool recorder_cancel_callback(void *user)
{
    (void)user;
    return recorder.stop_requested;
}

static bool recorder_pause_callback(void *user)
{
    (void)user;
    return recorder.paused;
}

static bool recorder_monitor_callback(void *user)
{
    (void)user;
    return recorder.monitor_enabled;
}

static const char *recorder_work_name(recorder_work_t work)
{
    switch (work) {
    case RECORDER_WORK_RECORD: return "record";
    case RECORDER_WORK_MONITOR: return "monitor";
    case RECORDER_WORK_PLAY: return "playback";
    default: return "unknown";
    }
}

static void recorder_progress_callback(
    const solar_os_audio_wav_progress_t *progress, void *user)
{
    (void)user;
    if (progress == NULL) {
        return;
    }
    portENTER_CRITICAL(&recorder_lock);
    recorder.elapsed_ms = progress->info.duration_ms;
    recorder.data_bytes = progress->info.data_bytes;
    portEXIT_CRITICAL(&recorder_lock);
}

static void recorder_samples_callback(const int16_t *samples,
                                      size_t sample_count,
                                      uint8_t channels,
                                      void *user)
{
    (void)user;
    if (samples == NULL || channels == 0U) {
        return;
    }
    const size_t frames = sample_count / channels;
    if (recorder.scope != NULL) {
        (void)solar_os_oscilloscope_widget_submit_s16(
            recorder.scope, samples, frames, channels);
    }
    if (recorder.spectrum != NULL) {
        (void)solar_os_spectrum_widget_submit_s16(
            recorder.spectrum, samples, frames, channels);
    }
}

static void recorder_device_callback(
    const solar_os_audio_device_info_t *device, void *user)
{
    (void)user;
    if (device == NULL) {
        return;
    }
    portENTER_CRITICAL(&recorder_lock);
    strlcpy(recorder.active_output_id, device->id,
            sizeof(recorder.active_output_id));
    recorder.active_output_capabilities = device->capabilities;
    portEXIT_CRITICAL(&recorder_lock);
}

static void recorder_worker(void *arg)
{
    (void)arg;
    const recorder_work_t work = recorder.work;
    const char *capture_id = recorder.input_index < recorder.input_count ?
        recorder.inputs[recorder.input_index].stream.id : "-";
    SOLAR_OS_LOGI(TAG,
                  "%s worker started input=%s path=%s format=%" PRIu32
                  "Hz/%uch/%ubit volume=%u",
                  recorder_work_name(work), capture_id,
                  recorder.active_path[0] != '\0' ? recorder.active_path : "-",
                  recorder.sample_rate, (unsigned)recorder.channels,
                  (unsigned)recorder.bits_per_sample,
                  (unsigned)recorder.volume);
    solar_os_audio_wav_info_t info = {0};
    const solar_os_audio_wav_options_t options = {
        .owner = "recorder",
        .capture_stream = recorder.input_index < recorder.input_count ?
            recorder.inputs[recorder.input_index].stream.id : NULL,
        .record_format = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .sample_rate = recorder.sample_rate,
            .channels = recorder.channels,
            .bits_per_sample = recorder.bits_per_sample,
        },
        .monitor = recorder.monitor_enabled,
        .monitor_volume = recorder.volume,
        .should_monitor = recorder_monitor_callback,
        .should_cancel = recorder_cancel_callback,
        .progress = recorder_progress_callback,
        .should_pause = recorder_pause_callback,
        .samples = recorder_samples_callback,
        .device = recorder_device_callback,
        .user = NULL,
        .progress_interval_ms = 250U,
    };
    esp_err_t err;
    if (work == RECORDER_WORK_RECORD) {
        err = solar_os_audio_record_wav(
            recorder.active_path, 0U, &options, &info);
    } else if (work == RECORDER_WORK_MONITOR) {
        err = solar_os_audio_monitor_stream(recorder.volume, &options, &info);
    } else {
        err = solar_os_audio_play_wav(
            recorder.active_path, recorder.volume, &options, &info);
    }
    const uint32_t stack_free =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    const bool stopped = recorder.stop_requested || err == ESP_ERR_TIMEOUT;
    if (err == ESP_OK || stopped) {
        SOLAR_OS_LOGI(TAG,
                      "%s worker ended ret=%s bytes=%" PRIu32
                      " ms=%" PRIu32 " stack_free=%" PRIu32,
                      recorder_work_name(work), esp_err_to_name(err),
                      info.data_bytes, info.duration_ms, stack_free);
    } else {
        SOLAR_OS_LOGW(TAG,
                      "%s worker failed ret=%s bytes=%" PRIu32
                      " ms=%" PRIu32 " stack_free=%" PRIu32,
                      recorder_work_name(work), esp_err_to_name(err),
                      info.data_bytes, info.duration_ms, stack_free);
    }
    portENTER_CRITICAL(&recorder_lock);
    recorder.elapsed_ms = info.duration_ms;
    recorder.data_bytes = info.data_bytes;
    recorder.worker_error = err;
    recorder.natural_completion = err == ESP_OK && !recorder.stop_requested;
    recorder.operation_state =
        err == ESP_OK || recorder.stop_requested || err == ESP_ERR_TIMEOUT ?
        RECORDER_IDLE : RECORDER_ERROR;
    if (recorder.operation_state == RECORDER_ERROR) {
        const char *operation = work == RECORDER_WORK_RECORD ?
            "Recording" : (work == RECORDER_WORK_MONITOR ?
                "Monitor" : "Playback");
        snprintf(recorder.message, sizeof(recorder.message), "%s failed: %s",
                 operation,
                 esp_err_to_name(err));
    }
    recorder.task_done = true;
    recorder.redraw = true;
    portEXIT_CRITICAL(&recorder_lock);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void recorder_reap_finished(void)
{
    if (recorder.task == NULL || !recorder.task_done) {
        return;
    }
    solar_os_task_delete_internal(recorder.task);
    recorder.task = NULL;
    recorder.stop_requested = false;
    recorder.paused = false;
    if (recorder.work == RECORDER_WORK_MONITOR) {
        recorder.monitor_enabled = false;
    }
    if (recorder.worker_error == ESP_OK ||
        recorder.worker_error == ESP_ERR_TIMEOUT) {
        recorder_set_message(recorder.work == RECORDER_WORK_RECORD ?
            "Recording saved" : (recorder.work == RECORDER_WORK_MONITOR ?
                "Monitor ended" : "Playback complete"));
    }
    if (recorder.browser != NULL) {
        (void)solar_os_storage_browser_refresh(recorder.browser);
    }
}

static void recorder_stop_worker(void)
{
    recorder.stop_requested = true;
    recorder.paused = false;
    TaskHandle_t task = recorder.task;
    if (task != NULL && !solar_os_task_wait_done(
            task, &recorder.task_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "recorder task did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        while (!recorder.task_done) {
            vTaskDelay(pdMS_TO_TICKS(RECORDER_WORKER_POLL_MS));
        }
    }
    if (task != NULL) {
        solar_os_task_delete_internal(task);
    }
    recorder.task = NULL;
    recorder.task_done = true;
    recorder.stop_requested = false;
    recorder.paused = false;
    recorder.operation_state = RECORDER_IDLE;
    recorder.redraw = true;
    if (recorder.browser != NULL) {
        (void)solar_os_storage_browser_refresh(recorder.browser);
    }
}

static esp_err_t recorder_start_worker(recorder_work_t work,
                                       const char *path)
{
    if (recorder.task != NULL ||
        (work != RECORDER_WORK_MONITOR &&
         (path == NULL || path[0] == '\0'))) {
        SOLAR_OS_LOGW(TAG, "%s start rejected: invalid state",
                      recorder_work_name(work));
        return ESP_ERR_INVALID_STATE;
    }
    if ((work == RECORDER_WORK_RECORD || work == RECORDER_WORK_MONITOR) &&
        recorder.input_count == 0U) {
        recorder_set_message("No audio input stream");
        SOLAR_OS_LOGW(TAG, "%s start rejected: no audio input stream",
                      recorder_work_name(work));
        return ESP_ERR_NOT_FOUND;
    }
    if (path != NULL) {
        strlcpy(recorder.active_path, path, sizeof(recorder.active_path));
    }
    recorder.work = work;
    recorder.elapsed_ms = 0U;
    recorder.data_bytes = 0U;
    recorder.worker_error = ESP_OK;
    recorder.stop_requested = false;
    recorder.paused = false;
    recorder.task_done = false;
    recorder.natural_completion = false;
    recorder.operation_state = work == RECORDER_WORK_RECORD ?
        RECORDER_RECORDING : (work == RECORDER_WORK_MONITOR ?
            RECORDER_MONITORING : RECORDER_PLAYING);
    recorder.active_output_id[0] = '\0';
    recorder.active_output_capabilities = 0U;
    solar_os_cassette_widget_reset(recorder.cassette);
    solar_os_oscilloscope_widget_reset(recorder.scope);
    solar_os_spectrum_widget_reset(recorder.spectrum);
    recorder_set_message(NULL);
    SOLAR_OS_LOGI(TAG, "%s start requested input=%s path=%s",
                  recorder_work_name(work),
                  recorder.input_index < recorder.input_count ?
                    recorder.inputs[recorder.input_index].stream.id : "-",
                  path != NULL ? path : "-");
    /* Capture and file I/O can cross cache-disabled regions, so this resumable
     * transport keeps its stack in internal SRAM. It is user-started foreground
     * work for admission purposes even though it continues while the UI is
     * suspended. Durable app, browser, widget, and PCM-buffer state remains
     * PSRAM-preferred. */
    const BaseType_t created = solar_os_task_create_pinned_internal(
        recorder_worker,
        work == RECORDER_WORK_RECORD ? "recorder_capture" :
            (work == RECORDER_WORK_MONITOR ? "recorder_monitor" :
                                             "recorder_play"),
        RECORDER_TASK_STACK, NULL, RECORDER_TASK_PRIORITY, &recorder.task,
        tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        recorder.task = NULL;
        recorder.task_done = true;
        recorder.operation_state = RECORDER_ERROR;
        recorder_set_message("Cannot start audio worker");
        SOLAR_OS_LOGW(TAG, "%s task creation failed",
                      recorder_work_name(work));
        return ESP_ERR_NO_MEM;
    }
    SOLAR_OS_LOGI(TAG, "%s task accepted stack=%u internal",
                  recorder_work_name(work), (unsigned)RECORDER_TASK_STACK);
    return ESP_OK;
}

static void recorder_start_recording(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    const esp_err_t err = recorder_build_recording_path(path, sizeof(path));
    if (err != ESP_OK) {
        recorder_set_message("Cannot create recording name");
        return;
    }
    (void)recorder_start_worker(RECORDER_WORK_RECORD, path);
}

static void recorder_start_playback(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        recorder_set_message("No recording selected");
        return;
    }
    solar_os_audio_wav_info_t info;
    if (solar_os_audio_get_wav_info(path, &info) != ESP_OK) {
        recorder_set_message("Cannot open WAV recording");
        return;
    }
    (void)recorder_start_worker(RECORDER_WORK_PLAY, path);
}

static void recorder_toggle_monitor(void)
{
    if (recorder.task != NULL) {
        if (recorder.work == RECORDER_WORK_MONITOR) {
            recorder.monitor_enabled = false;
            recorder_stop_worker();
        } else if (recorder.work == RECORDER_WORK_RECORD) {
            recorder.monitor_enabled = !recorder.monitor_enabled;
            recorder_set_message(recorder.monitor_enabled ?
                "Recording monitor on" : "Recording monitor off");
            SOLAR_OS_LOGI(TAG, "record monitor requested %s",
                          recorder.monitor_enabled ? "on" : "off");
        } else {
            recorder_set_message("Monitor unavailable during playback");
        }
        return;
    }
    recorder.monitor_enabled = true;
    if (recorder_start_worker(RECORDER_WORK_MONITOR, NULL) != ESP_OK) {
        recorder.monitor_enabled = false;
    }
}

static void recorder_toggle_pause(void)
{
    if (recorder.task == NULL || recorder.work == RECORDER_WORK_MONITOR) {
        return;
    }
    recorder.paused = !recorder.paused;
    recorder.operation_state = recorder.paused ? RECORDER_PAUSED :
        (recorder.work == RECORDER_WORK_RECORD ?
            RECORDER_RECORDING : RECORDER_PLAYING);
    recorder.redraw = true;
}

static void recorder_adjust_volume(int direction)
{
    int volume = (int)recorder.volume + direction * RECORDER_VOLUME_STEP;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    esp_err_t err = solar_os_audio_set_volume((uint8_t)volume);
    if (err == ESP_ERR_NOT_SUPPORTED && recorder.active_output_id[0] != '\0' &&
        (recorder.active_output_capabilities &
         SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0U) {
        err = solar_os_audio_set_device_volume(recorder.active_output_id,
                                               (uint8_t)volume);
    }
    if (err == ESP_OK || recorder.task == NULL) {
        recorder.volume = (uint8_t)volume;
        recorder_set_message(NULL);
    } else {
        recorder_set_message("Volume control unavailable");
    }
}

static void recorder_adjust_gain(int direction)
{
    if (!recorder.input_gain_available ||
        recorder.input_index >= recorder.input_count) {
        recorder_set_message("Input gain unavailable");
        return;
    }
    const recorder_input_t *input = &recorder.inputs[recorder.input_index];
    float step = input->device.input_gain_step_db;
    if (step <= 0.0f) step = 1.0f;
    float gain = recorder.input_gain_db + (direction < 0 ? -step : step);
    if (gain < input->device.input_gain_min_db) {
        gain = input->device.input_gain_min_db;
    }
    if (input->device.input_gain_max_db > input->device.input_gain_min_db &&
        gain > input->device.input_gain_max_db) {
        gain = input->device.input_gain_max_db;
    }
    const esp_err_t err = solar_os_audio_set_device_input_gain(
        input->device.id, gain);
    if (err == ESP_OK) {
        recorder.input_gain_db = gain;
        recorder.saved_input_gain_db = gain;
        recorder.saved_input_gain_valid = true;
        recorder_set_message(NULL);
    } else {
        recorder_set_message("Input gain unavailable");
    }
}

static void recorder_cycle_input(int direction)
{
    if (recorder.task != NULL || recorder.input_count == 0U) {
        return;
    }
    if (direction < 0) {
        recorder.input_index = recorder.input_index == 0U ?
            recorder.input_count - 1U : recorder.input_index - 1U;
    } else {
        recorder.input_index = (recorder.input_index + 1U) %
            recorder.input_count;
    }
    recorder_refresh_gain();
    strlcpy(recorder.preferred_input_id,
            recorder.inputs[recorder.input_index].stream.id,
            sizeof(recorder.preferred_input_id));
    recorder.saved_input_gain_valid = false;
}

static void recorder_cycle_rate(int direction)
{
    if (recorder.task != NULL) return;
    size_t index = 0U;
    for (size_t i = 0U; i < sizeof(recorder_sample_rates) /
         sizeof(recorder_sample_rates[0]); i++) {
        if (recorder_sample_rates[i] == recorder.sample_rate) {
            index = i;
            break;
        }
    }
    const size_t count = sizeof(recorder_sample_rates) /
        sizeof(recorder_sample_rates[0]);
    index = direction < 0 ? (index == 0U ? count - 1U : index - 1U) :
        (index + 1U) % count;
    recorder.sample_rate = recorder_sample_rates[index];
}

static void recorder_setup_adjust(int direction)
{
    switch (recorder.setup_cursor) {
    case 2U: recorder_cycle_input(direction); break;
    case 3U:
        if (recorder.task == NULL) recorder.channels = recorder.channels == 1U ? 2U : 1U;
        break;
    case 4U: recorder_cycle_rate(direction); break;
    case 5U:
        if (recorder.task == NULL) recorder.bits_per_sample =
            recorder.bits_per_sample == 8U ? 16U : 8U;
        break;
    case 6U: recorder_adjust_gain(direction); break;
    case 7U: recorder_adjust_volume(direction); break;
    default: break;
    }
    recorder.redraw = true;
}

static void recorder_begin_filename_edit(void)
{
    if (recorder.task != NULL) return;
    strlcpy(recorder.edit_filename, recorder.filename,
            sizeof(recorder.edit_filename));
    recorder.edit_filename_len = strlen(recorder.edit_filename);
    recorder.editing_filename = true;
    recorder.redraw = true;
}

static bool recorder_handle_filename_key(uint8_t key)
{
    if (key == SOLAR_OS_KEY_ESCAPE) {
        recorder.editing_filename = false;
    } else if (key == '\r' || key == '\n') {
        strlcpy(recorder.filename, recorder.edit_filename,
                sizeof(recorder.filename));
        recorder.editing_filename = false;
    } else if ((key == '\b' || key == 0x7fU ||
                key == SOLAR_OS_KEY_DELETE) &&
               recorder.edit_filename_len > 0U) {
        recorder.edit_filename[--recorder.edit_filename_len] = '\0';
    } else if (key >= 0x20U && key < 0x7fU && key != '/' && key != '\\' &&
               recorder.edit_filename_len + 1U <
                   sizeof(recorder.edit_filename)) {
        recorder.edit_filename[recorder.edit_filename_len++] = (char)key;
        recorder.edit_filename[recorder.edit_filename_len] = '\0';
    }
    recorder.redraw = true;
    return true;
}

static void recorder_open_browser(recorder_browser_mode_t mode)
{
    if (recorder.browser == NULL || recorder.task != NULL) {
        return;
    }
    const char *path = recorder.directory[0] != '\0' ?
        recorder.directory : solar_os_storage_mount_point();
    if (solar_os_storage_browser_open(recorder.browser, path) == ESP_OK) {
        recorder.browser_mode = mode;
        recorder.browser_top = 0U;
        recorder.tab = RECORDER_TAB_SETUP;
        recorder_set_message(NULL);
    } else {
        recorder_set_message("Cannot open storage");
    }
}

static void recorder_activate_browser(void)
{
    char selected[SOLAR_OS_STORAGE_PATH_MAX];
    bool file_selected = false;
    const esp_err_t err = solar_os_storage_browser_activate(
        recorder.browser, selected, sizeof(selected), &file_selected);
    if (err != ESP_OK) {
        recorder_set_message("Cannot open selection");
        return;
    }
    if (file_selected) {
        recorder.browser_mode = RECORDER_BROWSER_NONE;
        strlcpy(recorder.active_path, selected, sizeof(recorder.active_path));
        recorder_start_playback(selected);
        recorder.tab = RECORDER_TAB_RECORD;
    }
}

static void recorder_select_browser_directory(void)
{
    if (recorder.browser_mode != RECORDER_BROWSER_DIRECTORY) {
        return;
    }
    char selected[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_browser_select_directory(
            recorder.browser, selected, sizeof(selected)) == ESP_OK) {
        strlcpy(recorder.directory, selected, sizeof(recorder.directory));
        recorder.browser_mode = RECORDER_BROWSER_NONE;
        recorder_set_message("Recording folder selected");
    } else {
        recorder_set_message("Cannot select folder");
    }
}

static const char *recorder_state_name(void)
{
    switch (recorder.operation_state) {
    case RECORDER_RECORDING: return "recording";
    case RECORDER_MONITORING: return "monitoring";
    case RECORDER_PAUSED: return "paused";
    case RECORDER_PLAYING: return "playing";
    case RECORDER_ERROR: return "error";
    default: return "stopped";
    }
}

static void recorder_format_time(uint32_t duration_ms,
                                 char *text, size_t text_len)
{
    const uint32_t seconds = duration_ms / 1000U;
    snprintf(text, text_len, "%02" PRIu32 ":%02" PRIu32,
             seconds / 60U, seconds % 60U);
}

static void recorder_clip(char *destination, size_t destination_len,
                          const char *source, size_t width)
{
    if (destination == NULL || destination_len == 0U) return;
    if (source == NULL) source = "";
    const size_t limit = width < destination_len - 1U ?
        width : destination_len - 1U;
    const size_t length = strnlen(source, limit);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void recorder_setup_value(size_t row, char *label, size_t label_len,
                                 char *value, size_t value_len)
{
    switch (row) {
    case 0U:
        strlcpy(label, "Filename", label_len);
        strlcpy(value, recorder.filename[0] != '\0' ?
            recorder.filename : "automatic", value_len);
        break;
    case 1U:
        strlcpy(label, "Folder", label_len);
        strlcpy(value, recorder.directory, value_len);
        break;
    case 2U:
        strlcpy(label, "Input", label_len);
        strlcpy(value, recorder.input_index < recorder.input_count ?
            recorder.inputs[recorder.input_index].stream.id : "none", value_len);
        break;
    case 3U:
        strlcpy(label, "Channels", label_len);
        strlcpy(value, recorder.channels == 1U ? "mono" : "stereo", value_len);
        break;
    case 4U:
        strlcpy(label, "Sample rate", label_len);
        snprintf(value, value_len, "%" PRIu32 " Hz", recorder.sample_rate);
        break;
    case 5U:
        strlcpy(label, "Resolution", label_len);
        snprintf(value, value_len, "%u bit", (unsigned)recorder.bits_per_sample);
        break;
    case 6U:
        strlcpy(label, "Input gain", label_len);
        if (recorder.input_gain_available) {
            snprintf(value, value_len, "%.1f dB", (double)recorder.input_gain_db);
        } else {
            strlcpy(value, "n/a", value_len);
        }
        break;
    case 7U:
        strlcpy(label, "Output volume", label_len);
        snprintf(value, value_len, "%u%%", (unsigned)recorder.volume);
        break;
    default:
        label[0] = '\0';
        value[0] = '\0';
        break;
    }
}

static void recorder_render_browser_tui(size_t rows, size_t cols)
{
    const size_t count = solar_os_storage_browser_count(recorder.browser);
    const size_t cursor = solar_os_storage_browser_cursor(recorder.browser);
    const size_t list_rows = rows > 3U ? rows - 3U : 0U;
    if (cursor < recorder.browser_top) recorder.browser_top = cursor;
    if (list_rows > 0U && cursor >= recorder.browser_top + list_rows) {
        recorder.browser_top = cursor - list_rows + 1U;
    }
    char clipped[192];
    for (size_t row = 0U; row < list_rows; row++) {
        const size_t index = recorder.browser_top + row;
        if (index >= count) break;
        solar_os_storage_browser_entry_t entry;
        if (!solar_os_storage_browser_entry(recorder.browser, index, &entry)) continue;
        char line[192];
        snprintf(line, sizeof(line), "%s%s",
                 entry.is_directory ? "[DIR] " : "      ", entry.name);
        recorder_clip(clipped, sizeof(clipped), line,
                      cols > 2U ? cols - 2U : 0U);
        solar_os_tui_addstr(&recorder.tui, row + 1U, 1U, clipped,
                            index == cursor ? SOLAR_OS_TUI_ATTR_INVERSE :
                                              SOLAR_OS_TUI_ATTR_NORMAL);
    }
    char footer[192];
    snprintf(footer, sizeof(footer),
             "%s | Enter open/play WAV  D select folder  Esc cancel",
             solar_os_storage_browser_path(recorder.browser));
    solar_os_tui_draw_help(&recorder.tui, footer);
}

static void recorder_render_tui(void)
{
    const size_t rows = solar_os_tui_rows(&recorder.tui);
    const size_t cols = solar_os_tui_cols(&recorder.tui);
    if (rows < 5U || cols < 20U) {
        solar_os_tui_draw_too_small(&recorder.tui, "recorder");
        solar_os_tui_refresh(&recorder.tui);
        return;
    }
    solar_os_tui_clear(&recorder.tui);
    solar_os_tui_draw_title(
        &recorder.tui,
        recorder.browser_mode != RECORDER_BROWSER_NONE ?
            "Recorder - Folder" : "Recorder",
        NULL);
    if (recorder.browser_mode != RECORDER_BROWSER_NONE) {
        recorder_render_browser_tui(rows, cols);
    } else if (recorder.tab == RECORDER_TAB_SETUP) {
        for (size_t row = 0U; row < RECORDER_SETUP_ROWS && row + 2U < rows;
             row++) {
            char label[32], value[96], line[144], clipped[144];
            recorder_setup_value(row, label, sizeof(label), value, sizeof(value));
            snprintf(line, sizeof(line), "%-14s %s", label, value);
            recorder_clip(clipped, sizeof(clipped), line,
                          cols > 2U ? cols - 2U : 0U);
            solar_os_tui_addstr(&recorder.tui, row + 1U, 1U, clipped,
                                row == recorder.setup_cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE :
                                    SOLAR_OS_TUI_ATTR_NORMAL);
        }
        const char *footer = recorder.editing_filename ?
            "Filename: type, Enter save, Esc cancel" :
            "Up/Down select  Left/Right change  Enter edit/browse";
        solar_os_tui_draw_help(&recorder.tui, footer);
    } else {
        char elapsed[16], line[192], clipped[192];
        recorder_format_time(recorder.elapsed_ms, elapsed, sizeof(elapsed));
        snprintf(line, sizeof(line), "%s  %s  %s",
                 recorder_state_name(), elapsed,
                 recorder.active_path[0] != '\0' ?
                    recorder_basename(recorder.active_path) : "no recording");
        recorder_clip(clipped, sizeof(clipped), line,
                      cols > 2U ? cols - 2U : 0U);
        solar_os_tui_addstr(&recorder.tui, 2U, 1U, clipped,
                            SOLAR_OS_TUI_ATTR_BOLD);
        snprintf(line, sizeof(line),
                 "%" PRIu32 " Hz, %s, %u bit | input %s | volume %u%%",
                 recorder.sample_rate, recorder.channels == 1U ? "mono" : "stereo",
                 (unsigned)recorder.bits_per_sample,
                 recorder.input_index < recorder.input_count ?
                    recorder.inputs[recorder.input_index].stream.id : "none",
                 (unsigned)recorder.volume);
        recorder_clip(clipped, sizeof(clipped), line,
                      cols > 2U ? cols - 2U : 0U);
        solar_os_tui_addstr(&recorder.tui, 3U, 1U, clipped,
                            SOLAR_OS_TUI_ATTR_NORMAL);
        if (recorder.message[0] != '\0' && rows > 6U) {
            recorder_clip(clipped, sizeof(clipped), recorder.message,
                          cols > 2U ? cols - 2U : 0U);
            solar_os_tui_addstr(&recorder.tui, 5U, 1U, clipped,
                                SOLAR_OS_TUI_ATTR_NORMAL);
        }
        solar_os_tui_draw_help(
            &recorder.tui,
            "R record  M monitor  Space pause  S stop  P play  Tab setup  Esc exit");
    }
    if (recorder.editing_filename) {
        char edit[144];
        snprintf(edit, sizeof(edit), "Filename: %s_", recorder.edit_filename);
        solar_os_tui_fill(&recorder.tui, rows - 3U, 0U, 1U, cols, ' ',
                          SOLAR_OS_TUI_ATTR_INVERSE);
        solar_os_tui_addstr(&recorder.tui, rows - 3U, 1U, edit,
                            SOLAR_OS_TUI_ATTR_INVERSE);
    }
    solar_os_tui_set_cursor_visible(&recorder.tui, false);
    solar_os_tui_refresh(&recorder.tui);
}

static void recorder_draw_centered(solar_os_gfx_t *gfx, int width,
                                   int baseline, const char *text)
{
    solar_os_gfx_text(gfx,
        (width - (int)solar_os_gfx_text_width(gfx, text)) / 2,
        baseline, text);
}

static void recorder_draw_header(solar_os_gfx_t *gfx, int width)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, RECORDER_HEADER_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, 7, 19, "Recorder");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    static const char *tabs[] = {"[REC] SET", "REC [SET]"};
    const char *text = tabs[recorder.tab];
    solar_os_gfx_text(gfx,
        width - (int)solar_os_gfx_text_width(gfx, text) - 7, 18, text);
}

static void recorder_draw_visualizer(solar_os_gfx_t *gfx,
                                     int width, int height)
{
    const int media_top = height * 2 / 3;
    const int visual_y = RECORDER_HEADER_HEIGHT + 4;
    const int visual_height = media_top - visual_y - 4;
    if (recorder.visualizer == RECORDER_VISUALIZER_CASSETTE) {
        solar_os_cassette_widget_draw(recorder.cassette, gfx, 5, visual_y,
                                      width - 10, visual_height);
    } else if (recorder.visualizer == RECORDER_VISUALIZER_SCOPE) {
        solar_os_oscilloscope_widget_draw(recorder.scope, gfx, 5, visual_y,
                                          width - 10, visual_height);
    } else {
        solar_os_spectrum_widget_draw(recorder.spectrum, gfx, 5, visual_y,
                                      width - 10, visual_height);
    }
    static const char *labels[] = {
        "CASSETTE  V", "SCOPE  V", "SPECTRUM  V",
    };
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 9, visual_y + 4, 86, 15);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 13, visual_y + 15, labels[recorder.visualizer]);
}

static void recorder_draw_progress(solar_os_gfx_t *gfx,
                                   int width, int height,
                                   bool clear_background)
{
    const int media_top = height * 2 / 3;
    if (clear_background) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, 1, media_top + 23, width - 2, 17);
    }
    char elapsed[16], status[96];
    recorder_format_time(recorder.elapsed_ms, elapsed, sizeof(elapsed));
    snprintf(status, sizeof(status), "%s  %s  %" PRIu32 " Hz  %s  %u bit",
             recorder_state_name(), elapsed, recorder.sample_rate,
             recorder.channels == 1U ? "mono" : "stereo",
             (unsigned)recorder.bits_per_sample);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    recorder_draw_centered(gfx, width, media_top + 37, status);
}

static void recorder_render_record_graphics(solar_os_gfx_t *gfx,
                                            int width, int height)
{
    const int media_top = height * 2 / 3;
    recorder_draw_visualizer(gfx, width, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 0, media_top, width - 1, media_top);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    recorder_draw_centered(gfx, width, media_top + 20,
        recorder.active_path[0] != '\0' ?
            recorder_basename(recorder.active_path) :
            (recorder.filename[0] != '\0' ? recorder.filename : "Automatic name"));
    recorder_draw_progress(gfx, width, height, false);
    const int volume_width = width / 2;
    const int volume_x = (width - volume_width) / 2;
    const int volume_y = media_top + 43;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, volume_x - 29, volume_y + 9, "VOL");
    solar_os_gfx_text(gfx, volume_x + volume_width + 8, volume_y + 9,
        recorder.monitor_enabled ?
            "MON M ON" : "MON M OFF");
    solar_os_gfx_rect(gfx, volume_x, volume_y, volume_width, 10);
    if (recorder.volume > 0U) {
        solar_os_gfx_fill_rect(gfx, volume_x + 2, volume_y + 2,
            (volume_width - 4) * recorder.volume / 100, 6);
    }
    const int gap = 5;
    const int button_width = (width - 5 * gap) / 4;
    const int button_y = height - 25;
    solar_os_media_transport_button_draw(
        gfx, gap, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_RECORD,
        recorder.operation_state == RECORDER_RECORDING);
    solar_os_media_transport_button_draw(
        gfx, gap * 2 + button_width, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_PLAY,
        recorder.operation_state == RECORDER_PLAYING);
    solar_os_media_transport_button_draw(
        gfx, gap * 3 + button_width * 2, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_PAUSE,
        recorder.operation_state == RECORDER_PAUSED);
    solar_os_media_transport_button_draw(
        gfx, gap * 4 + button_width * 3, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_STOP,
        recorder.operation_state == RECORDER_IDLE ||
            recorder.operation_state == RECORDER_ERROR);
}

static void recorder_render_setup_graphics(solar_os_gfx_t *gfx,
                                           int width, int height)
{
    const int list_y = RECORDER_HEADER_HEIGHT + 6;
    const int row_height = (height - list_y - RECORDER_FOOTER_HEIGHT) /
        (int)RECORDER_SETUP_ROWS;
    for (size_t row = 0U; row < RECORDER_SETUP_ROWS; row++) {
        const int y = list_y + (int)row * row_height;
        char label[32], value[96];
        recorder_setup_value(row, label, sizeof(label), value, sizeof(value));
        if (row == recorder.setup_cursor) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8, row_height - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_text(gfx, 9, y + row_height - 7, label);
        const int value_width = (int)solar_os_gfx_text_width(gfx, value);
        solar_os_gfx_text(gfx, width - value_width - 9,
                          y + row_height - 7, value);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 6, height - 6,
                      "Up/Down select  Left/Right change  Enter edit/folder");
}

static void recorder_render_browser_graphics(solar_os_gfx_t *gfx,
                                             int width, int height)
{
    const int list_y = RECORDER_HEADER_HEIGHT + 5;
    const int bottom = height - RECORDER_FOOTER_HEIGHT;
    const size_t visible = bottom > list_y ?
        (size_t)((bottom - list_y) / RECORDER_ROW_HEIGHT) : 0U;
    const size_t count = solar_os_storage_browser_count(recorder.browser);
    const size_t cursor = solar_os_storage_browser_cursor(recorder.browser);
    if (cursor < recorder.browser_top) recorder.browser_top = cursor;
    if (visible > 0U && cursor >= recorder.browser_top + visible) {
        recorder.browser_top = cursor - visible + 1U;
    }
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = recorder.browser_top + row;
        if (index >= count) break;
        solar_os_storage_browser_entry_t entry;
        if (!solar_os_storage_browser_entry(recorder.browser, index, &entry)) continue;
        const int y = list_y + (int)row * RECORDER_ROW_HEIGHT;
        char label[192];
        snprintf(label, sizeof(label), "%s%s",
                 entry.is_directory ? "[DIR] " : "", entry.name);
        if (index == cursor) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8,
                                   RECORDER_ROW_HEIGHT - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_text(gfx, 9, y + 17, label);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 6, height - 6,
        "Enter open/play WAV  D select folder  Esc cancel");
}

static void recorder_draw_filename_dialog(solar_os_gfx_t *gfx,
                                          int width, int height)
{
    const int dialog_width = width * 4 / 5;
    const int dialog_height = 76;
    const int x = (width - dialog_width) / 2;
    const int y = (height - dialog_height) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, dialog_width, dialog_height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, dialog_width, dialog_height);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, x + 8, y + 20, "Recording filename");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
    char value[RECORDER_FILENAME_MAX + 2U];
    snprintf(value, sizeof(value), "%s_", recorder.edit_filename);
    solar_os_gfx_text(gfx, x + 8, y + 45, value);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, x + 8, y + 67,
                      "Enter save  Esc cancel  Empty = automatic");
}

static void recorder_render(solar_os_context_t *ctx)
{
    if (!recorder.ui_started || recorder.suspended) return;
    if (recorder.mode == RECORDER_MODE_TUI) {
        recorder_render_tui();
        recorder.redraw = false;
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    recorder_draw_header(gfx, width);
    if (recorder.browser_mode != RECORDER_BROWSER_NONE) {
        recorder_render_browser_graphics(gfx, width, height);
    } else if (recorder.tab == RECORDER_TAB_RECORD) {
        recorder_render_record_graphics(gfx, width, height);
    } else {
        recorder_render_setup_graphics(gfx, width, height);
    }
    if (recorder.editing_filename) {
        recorder_draw_filename_dialog(gfx, width, height);
    }
    solar_os_gfx_present(gfx);
    recorder.redraw = false;
}

static void recorder_render_dynamic(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || recorder.suspended ||
        recorder.mode != RECORDER_MODE_GRAPHICS ||
        recorder.tab != RECORDER_TAB_RECORD ||
        recorder.browser_mode != RECORDER_BROWSER_NONE ||
        recorder.editing_filename) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    recorder_draw_visualizer(gfx, width, height);
    recorder_draw_progress(gfx, width, height, true);
    solar_os_gfx_present(gfx);
}

static bool recorder_handle_browser_key(uint8_t key)
{
    if (key == SOLAR_OS_KEY_ESCAPE) {
        recorder.tab = RECORDER_TAB_SETUP;
        recorder.browser_mode = RECORDER_BROWSER_NONE;
    } else if (key == SOLAR_OS_KEY_UP || key == 'k') {
        solar_os_storage_browser_move(recorder.browser, -1);
    } else if (key == SOLAR_OS_KEY_DOWN || key == 'j') {
        solar_os_storage_browser_move(recorder.browser, 1);
    } else if (key == '\r' || key == '\n') {
        recorder_activate_browser();
    } else if ((key == 'd' || key == 'D') &&
               recorder.browser_mode == RECORDER_BROWSER_DIRECTORY) {
        recorder_select_browser_directory();
    }
    recorder.redraw = true;
    return true;
}

static bool recorder_handle_key(solar_os_context_t *ctx, uint8_t key)
{
    if (recorder.editing_filename) {
        (void)recorder_handle_filename_key(key);
        recorder_render(ctx);
        return true;
    }
    if (recorder.browser_mode != RECORDER_BROWSER_NONE) {
        (void)recorder_handle_browser_key(key);
        recorder_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE ||
        key == 'q' || key == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (key == '\t') {
        recorder.tab = (recorder_tab_t)((recorder.tab + 1U) %
                                         RECORDER_TAB_COUNT);
    } else if (key == 'r' || key == 'R') {
        const bool start = recorder.task == NULL ||
            recorder.work != RECORDER_WORK_RECORD;
        if (recorder.task != NULL) recorder_stop_worker();
        if (start) {
            recorder_start_recording();
        } else {
            recorder.monitor_enabled = false;
        }
        recorder.tab = RECORDER_TAB_RECORD;
    } else if (key == 's' || key == 'S') {
        recorder.monitor_enabled = false;
        if (recorder.task != NULL) recorder_stop_worker();
    } else if (key == 'p' || key == 'P') {
        const bool start = recorder.task == NULL ||
            recorder.work != RECORDER_WORK_PLAY;
        recorder.monitor_enabled = false;
        if (recorder.task != NULL) recorder_stop_worker();
        if (start) recorder_start_playback(recorder.active_path);
        recorder.tab = RECORDER_TAB_RECORD;
    } else if (key == ' ') {
        recorder_toggle_pause();
    } else if (recorder.tab == RECORDER_TAB_RECORD) {
        if (key == SOLAR_OS_KEY_UP) recorder_adjust_volume(1);
        else if (key == SOLAR_OS_KEY_DOWN) recorder_adjust_volume(-1);
        else if (key == 'm' || key == 'M') recorder_toggle_monitor();
        else if (key == 'v' || key == 'V') {
            recorder.visualizer = (recorder_visualizer_t)(
                (recorder.visualizer + 1U) % RECORDER_VISUALIZER_COUNT);
        } else if (key == '\r' || key == '\n') {
            if (recorder.task == NULL) {
                recorder_start_recording();
            } else if (recorder.work == RECORDER_WORK_MONITOR) {
                recorder_stop_worker();
                recorder_start_recording();
            } else {
                recorder.monitor_enabled = false;
                recorder_stop_worker();
            }
        }
    } else if (recorder.tab == RECORDER_TAB_SETUP) {
        if (key == SOLAR_OS_KEY_UP || key == 'k') {
            recorder.setup_cursor = recorder.setup_cursor == 0U ?
                RECORDER_SETUP_ROWS - 1U : recorder.setup_cursor - 1U;
        } else if (key == SOLAR_OS_KEY_DOWN || key == 'j') {
            recorder.setup_cursor = (recorder.setup_cursor + 1U) %
                RECORDER_SETUP_ROWS;
        } else if (key == SOLAR_OS_KEY_LEFT) {
            recorder_setup_adjust(-1);
        } else if (key == SOLAR_OS_KEY_RIGHT) {
            recorder_setup_adjust(1);
        } else if (key == '\r' || key == '\n') {
            if (recorder.setup_cursor == 0U) recorder_begin_filename_edit();
            else if (recorder.setup_cursor == 1U)
                recorder_open_browser(RECORDER_BROWSER_DIRECTORY);
            else recorder_setup_adjust(1);
        } else if (key == 'd' || key == 'D') {
            recorder_open_browser(RECORDER_BROWSER_DIRECTORY);
        }
    }
    recorder.redraw = true;
    recorder_render(ctx);
    return true;
}

static esp_err_t recorder_set_initial_path(const char *argument)
{
    if (recorder.directory[0] == '\0') {
        strlcpy(recorder.directory, solar_os_storage_mount_point(),
                sizeof(recorder.directory));
    }
    if (argument == NULL || argument[0] == '\0') {
        return ESP_OK;
    }
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_resolve_path(argument, path, sizeof(path));
    if (err != ESP_OK) return err;
    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(recorder.filename, slash + 1U, sizeof(recorder.filename));
    if (slash == path) {
        strlcpy(recorder.directory, "/", sizeof(recorder.directory));
    } else {
        *slash = '\0';
        strlcpy(recorder.directory, path, sizeof(recorder.directory));
    }
    return ESP_OK;
}

static esp_err_t recorder_start(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    bool force_tui = false;
    const char *path_arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "--tui") == 0) {
            force_tui = true;
        } else if (path_arg == NULL) {
            path_arg = arg;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    const recorder_mode_t launch_mode =
        !force_tui && recorder_graphical_session(ctx) ?
            RECORDER_MODE_GRAPHICS : RECORDER_MODE_TUI;
    solar_os_context_set_app_class(
        ctx,
        launch_mode == RECORDER_MODE_GRAPHICS ?
            SOLAR_OS_APP_CLASS_GUI : SOLAR_OS_APP_CLASS_TUI);
    if (!solar_os_storage_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (recorder_state != NULL) return ESP_ERR_INVALID_STATE;
    const uint32_t internal_before = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if SOLAR_OS_BOARD_HAS_PSRAM
    const solar_os_memory_class_t state_memory =
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED;
#else
    const solar_os_memory_class_t state_memory =
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED;
#endif
    recorder_state = solar_os_memory_calloc(
        1U, sizeof(*recorder_state), state_memory, "recorder.state");
    if (recorder_state == NULL) return ESP_ERR_NO_MEM;
    recorder.internal_free_before = internal_before;
    SOLAR_OS_LOGI(TAG, "state allocated bytes=%u memory=%s",
                  (unsigned)sizeof(*recorder_state),
#if SOLAR_OS_BOARD_HAS_PSRAM
                  "external-required"
#else
                  "external-preferred"
#endif
    );
    recorder.task_done = true;
    recorder.sample_rate = 16000U;
    recorder.channels = 2U;
    recorder.bits_per_sample = 16U;
    solar_os_audio_status_t status;
    solar_os_audio_get_status(&status);
    recorder.volume = status.volume <= 100U ? status.volume : 50U;
    recorder.visualizer = RECORDER_VISUALIZER_CASSETTE;
    const bool settings_loaded = recorder_load_settings();
    esp_err_t err = recorder_set_initial_path(path_arg);
    if (err != ESP_OK) goto fail_state;
    recorder_refresh_inputs();
    SOLAR_OS_LOGI(TAG, "inputs discovered count=%u selected=%s",
                  (unsigned)recorder.input_count,
                  recorder.input_index < recorder.input_count ?
                      recorder.inputs[recorder.input_index].stream.id : "-");
    if (!settings_loaded && recorder.input_count > 0U) {
        const solar_os_stream_audio_format_t *format =
            &recorder.inputs[recorder.input_index].stream.audio;
        if (format->sample_rate != 0U) recorder.sample_rate = format->sample_rate;
        if (format->channels > 0U && format->channels <= 2U) {
            recorder.channels = format->channels;
        }
    }
    if (settings_loaded && recorder.saved_input_gain_valid &&
        recorder.input_gain_available &&
        recorder.input_index < recorder.input_count &&
        strcmp(recorder.inputs[recorder.input_index].stream.id,
               recorder.preferred_input_id) == 0) {
        const recorder_input_t *input = &recorder.inputs[recorder.input_index];
        err = solar_os_audio_set_device_input_gain(
            input->device.id, recorder.saved_input_gain_db);
        if (err == ESP_OK) {
            recorder.input_gain_db = recorder.saved_input_gain_db;
        } else {
            SOLAR_OS_LOGW(TAG, "saved input gain unavailable device=%s: %s",
                          input->device.id, esp_err_to_name(err));
        }
    }
    err = solar_os_storage_browser_create(
        recorder_wav_file, NULL, &recorder.browser);
    if (err != ESP_OK) goto fail_state;
    recorder_log_internal_memory("browser", internal_before);
    recorder.mode = launch_mode;
    if (recorder.mode == RECORDER_MODE_TUI) {
        err = solar_os_tui_screen_begin(&recorder.tui, ctx);
    } else {
        err = solar_os_cassette_widget_create(&recorder.cassette);
        if (err == ESP_OK) err = solar_os_oscilloscope_widget_create(
            RECORDER_SCOPE_SAMPLES, &recorder.scope);
        if (err == ESP_OK) err = solar_os_spectrum_widget_create(
            RECORDER_SPECTRUM_FFT_SIZE, &recorder.spectrum);
        if (err == ESP_OK) {
            recorder_enable_high_refresh(ctx);
            solar_os_context_set_graphics_active(ctx, true);
        }
    }
    if (err != ESP_OK) {
        solar_os_cassette_widget_destroy(recorder.cassette);
        solar_os_oscilloscope_widget_destroy(recorder.scope);
        solar_os_spectrum_widget_destroy(recorder.spectrum);
        solar_os_storage_browser_destroy(recorder.browser);
        recorder.browser = NULL;
        goto fail_state;
    }
    recorder.ui_started = true;
    recorder.redraw = true;
    recorder_render(ctx);
    recorder_log_internal_memory("ready", internal_before);
    return ESP_OK;

fail_state:
    solar_os_memory_free(recorder_state);
    recorder_state = NULL;
    return err;
}

static void recorder_stop(solar_os_context_t *ctx)
{
    if (recorder_state == NULL) return;
    const uint32_t internal_before = recorder.internal_free_before;
    recorder_stop_worker();
    const esp_err_t settings_error = recorder_save_settings();
    if (settings_error != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "settings save failed: %s",
                      esp_err_to_name(settings_error));
    }
    if (recorder.mode == RECORDER_MODE_GRAPHICS) {
        recorder_disable_high_refresh();
        solar_os_cassette_widget_destroy(recorder.cassette);
        solar_os_oscilloscope_widget_destroy(recorder.scope);
        solar_os_spectrum_widget_destroy(recorder.spectrum);
        solar_os_context_set_graphics_active(ctx, false);
    } else if (recorder.ui_started) {
        solar_os_tui_set_cursor_visible(&recorder.tui, true);
        solar_os_tui_refresh(&recorder.tui);
        solar_os_tui_end(&recorder.tui);
    }
    solar_os_storage_browser_destroy(recorder.browser);
    recorder.browser = NULL;
    recorder.ui_started = false;
    solar_os_memory_free(recorder_state);
    recorder_state = NULL;
    recorder_log_internal_memory("closed", internal_before);
}

static void recorder_suspend(solar_os_context_t *ctx)
{
    recorder.suspended = true;
    if (recorder.mode == RECORDER_MODE_GRAPHICS) {
        recorder_disable_high_refresh();
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static void recorder_resume(solar_os_context_t *ctx)
{
    recorder.suspended = false;
    if (recorder.mode == RECORDER_MODE_GRAPHICS) {
        recorder_enable_high_refresh(ctx);
        solar_os_context_set_graphics_active(ctx, true);
    }
    recorder.redraw = true;
    recorder_render(ctx);
}

static bool recorder_event(solar_os_context_t *ctx,
                           const solar_os_event_t *event)
{
    if (event == NULL) return false;
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        recorder_resume(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        recorder_reap_finished();
        if (recorder.task == NULL &&
            event->data.tick_ms - recorder.last_input_refresh_ms >=
                RECORDER_INPUT_REFRESH_MS) {
            recorder.last_input_refresh_ms = event->data.tick_ms;
            recorder_refresh_inputs();
        }
        solar_os_cassette_widget_update(
            recorder.cassette,
            recorder.operation_state == RECORDER_RECORDING ||
                recorder.operation_state == RECORDER_PLAYING,
            recorder.elapsed_ms, 0U, event->data.tick_ms);
        const bool refresh_due = recorder.task != NULL && !recorder.paused &&
            event->data.tick_ms - recorder.last_visualizer_ms >=
                RECORDER_VISUAL_REFRESH_MS;
        if (!recorder.suspended && recorder.redraw) {
            recorder_render(ctx);
        } else if (!recorder.suspended && refresh_due) {
            recorder.last_visualizer_ms = event->data.tick_ms;
            if (recorder.mode == RECORDER_MODE_GRAPHICS &&
                recorder.tab == RECORDER_TAB_RECORD &&
                recorder.browser_mode == RECORDER_BROWSER_NONE &&
                !recorder.editing_filename) {
                recorder_render_dynamic(ctx);
            } else if (recorder.mode == RECORDER_MODE_TUI) {
                recorder_render(ctx);
            }
        }
        return true;
    }
    return event->type == SOLAR_OS_EVENT_CHAR ?
        recorder_handle_key(ctx, (uint8_t)event->data.ch) : false;
}

static void recorder_title(solar_os_context_t *ctx,
                           char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        if (recorder_state == NULL) {
            strlcpy(buffer, "recorder", buffer_len);
            return;
        }
        snprintf(buffer, buffer_len, "recorder%s%s",
                 recorder.active_path[0] != '\0' ? ": " : "",
                 recorder.active_path[0] != '\0' ?
                    recorder_basename(recorder.active_path) : "");
    }
}

const solar_os_app_t solar_os_recorder_app = {
    .name = "recorder",
    .summary = "interactive WAV recorder",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = recorder_start,
    .suspend = recorder_suspend,
    .resume = recorder_resume,
    .stop = recorder_stop,
    .event = recorder_event,
    .title = recorder_title,
    .tick_interval_ms = RECORDER_TICK_MS,
    .tick_deadline_ms = RECORDER_TICK_DEADLINE_MS,
    .worker_stack_bytes = RECORDER_TASK_STACK,
    .worker_stack_external = false,
};
