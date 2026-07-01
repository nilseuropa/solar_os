#include "solar_os_python.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "port/micropython_embed.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mpprint.h"
#include "py/nlr.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/qstr.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "solar_os_adc.h"
#include "solar_os_app_registry.h"
#include "solar_os_audio.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_clipboard.h"
#include "solar_os_gfx.h"
#include "solar_os_gpio.h"
#include "solar_os_i2c.h"
#include "solar_os_identity.h"
#include "solar_os_jobs.h"
#include "solar_os_mqtt.h"
#include "solar_os_net.h"
#include "solar_os_pwm.h"
#include "solar_os_sensors.h"
#include "solar_os_shell_io.h"
#include "solar_os_ssh_keys.h"
#include "solar_os_status_led.h"
#include "solar_os_storage.h"
#include "solar_os_terminal.h"
#include "solar_os_time.h"
#include "solar_os_tui.h"
#include "solar_os_uart.h"
#include "solar_os_wifi.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define PYTHON_HEAP_SIZE (512U * 1024U)
#define PYTHON_SCRIPT_MAX_BYTES (512U * 1024U)
#define PYTHON_TASK_STACK 16384
#define PYTHON_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define PYTHON_EVENT_QUEUE_LEN 32
#define PYTHON_EVENT_DATA_MAX 192
#define PYTHON_INPUT_QUEUE_LEN 4
#define PYTHON_KEY_QUEUE_LEN 32
#define PYTHON_REPL_LINE_MAX 192
#define PYTHON_REPL_SOURCE_MAX (2U * 1024U)
#define PYTHON_STOP_WAIT_MS 1500
#define PYTHON_DRAIN_EVENTS_PER_TICK 8U

typedef enum {
    PYTHON_EVENT_OUTPUT,
    PYTHON_EVENT_STATUS,
    PYTHON_EVENT_ERROR,
    PYTHON_EVENT_PROMPT,
    PYTHON_EVENT_TUI_CLEAR,
    PYTHON_EVENT_TUI_REFRESH,
    PYTHON_EVENT_TUI_MOVE,
    PYTHON_EVENT_TUI_WRITE,
    PYTHON_EVENT_TUI_PUTCH,
    PYTHON_EVENT_TUI_HLINE,
    PYTHON_EVENT_TUI_VLINE,
    PYTHON_EVENT_TUI_VRULE,
    PYTHON_EVENT_TUI_BOX,
    PYTHON_EVENT_TUI_FILL,
    PYTHON_EVENT_GFX_BEGIN,
    PYTHON_EVENT_GFX_END,
    PYTHON_EVENT_GFX_CLEAR,
    PYTHON_EVENT_GFX_COLOR,
    PYTHON_EVENT_GFX_FONT,
    PYTHON_EVENT_GFX_PRESENT,
    PYTHON_EVENT_GFX_PIXEL,
    PYTHON_EVENT_GFX_LINE,
    PYTHON_EVENT_GFX_RECT,
    PYTHON_EVENT_GFX_FILL_RECT,
    PYTHON_EVENT_GFX_CIRCLE,
    PYTHON_EVENT_GFX_FILL_CIRCLE,
    PYTHON_EVENT_GFX_TEXT,
    PYTHON_EVENT_DONE,
} python_event_type_t;

typedef enum {
    PYTHON_MODE_SCRIPT,
    PYTHON_MODE_REPL,
} python_mode_t;

typedef struct {
    python_event_type_t type;
    size_t data_len;
    bool success;
    uint16_t row;
    uint16_t col;
    uint16_t height;
    uint16_t width;
    uint32_t codepoint;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint8_t attr;
    char data[PYTHON_EVENT_DATA_MAX];
} python_event_t;

typedef struct {
    char line[PYTHON_REPL_LINE_MAX];
} python_input_t;

typedef struct {
    QueueHandle_t events;
    QueueHandle_t input;
    QueueHandle_t key_input;
    TaskHandle_t task;
    solar_os_context_t *ctx;
    volatile bool stop_requested;
    volatile bool task_done;
    volatile bool vm_active;
    volatile bool repl_executing;
    volatile bool repl_exit_requested;
    python_mode_t mode;
    bool running;
    bool done;
    bool interrupted;
    bool repl_input_active;
    size_t repl_input_row;
    size_t repl_input_col;
    size_t repl_input_len;
    size_t repl_input_cursor;
    char repl_input[PYTHON_REPL_LINE_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    int argc;
    char argv[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
} python_app_state_t;

static const char *TAG = "solar_os_python";
static EXT_RAM_BSS_ATTR python_app_state_t python_app;
static solar_os_shell_io_t python_fallback_io;

static solar_os_shell_io_t *python_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&python_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &python_fallback_io);
        io = &python_fallback_io;
    }
    return io;
}

static solar_os_shell_io_t *python_current_io(void)
{
    return python_app.ctx != NULL ? python_io(python_app.ctx) : NULL;
}

static solar_os_terminal_t *python_display_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
}

static bool python_send_event(const python_event_t *event)
{
    if (event == NULL || python_app.events == NULL) {
        return false;
    }

    while (!python_app.stop_requested) {
        if (xQueueSend(python_app.events, event, pdMS_TO_TICKS(50)) == pdPASS) {
            return true;
        }
    }

    return xQueueSend(python_app.events, event, 0) == pdPASS;
}

static void python_send_message(python_event_type_t type, const char *message)
{
    python_event_t event = {
        .type = type,
    };
    if (message != NULL) {
        strlcpy(event.data, message, sizeof(event.data));
        event.data_len = strlen(event.data);
    }
    (void)python_send_event(&event);
}

static void python_send_prompt(const char *prompt)
{
    python_send_message(PYTHON_EVENT_PROMPT, prompt != NULL ? prompt : ">>> ");
}

static bool python_send_output(const char *data, size_t len)
{
    while (len > 0) {
        python_event_t event = {
            .type = PYTHON_EVENT_OUTPUT,
        };
        event.data_len = len > sizeof(event.data) ? sizeof(event.data) : len;
        memcpy(event.data, data, event.data_len);
        if (!python_send_event(&event)) {
            return false;
        }

        data += event.data_len;
        len -= event.data_len;
    }

    return true;
}

void solar_os_micropython_stdout(const char *str, size_t len)
{
    if (str == NULL || len == 0) {
        return;
    }

    if (!python_send_output(str, len)) {
        fwrite(str, 1, len, stdout);
    }
}

bool solar_os_micropython_stop_requested(void)
{
    return python_app.stop_requested;
}

static const char *python_mode_name(void)
{
    return python_app.mode == PYTHON_MODE_REPL ? "repl" : "script";
}

static bool python_is_printable_char(char ch)
{
    const unsigned char value = (unsigned char)ch;
    return isprint(value) || value >= 0xa0;
}

static uint8_t *python_alloc_psram_first(size_t len)
{
    uint8_t *data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    return data;
}

static bool python_path_has_suffix(const char *path, const char *suffix)
{
    if (path == NULL || suffix == NULL) {
        return false;
    }

    const size_t path_len = strlen(path);
    const size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
        strcmp(path + path_len - suffix_len, suffix) == 0;
}

static esp_err_t python_load_file(const char *path, uint8_t **out_data, size_t *out_len, bool nul_terminate)
{
    if (path == NULL || out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_len = 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > PYTHON_SCRIPT_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    const size_t len = (size_t)st.st_size;
    uint8_t *data = python_alloc_psram_first(len + (nul_terminate ? 1U : 0U));
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_len = fread(data, 1, len, file);
    const int read_errno = errno;
    fclose(file);

    if (read_len != len) {
        heap_caps_free(data);
        errno = read_errno != 0 ? read_errno : EIO;
        return ESP_FAIL;
    }
    if (nul_terminate) {
        data[len] = '\0';
    }

    *out_data = data;
    *out_len = len;
    return ESP_OK;
}

static mp_obj_t python_key(const char *name)
{
    return MP_OBJ_NEW_QSTR(qstr_from_str(name));
}

static void python_module_store(mp_obj_t module, const char *name, mp_obj_t value);

static void python_dict_store_cstr(mp_obj_t dict, const char *key, const char *value)
{
    mp_obj_dict_store(dict,
                      python_key(key),
                      value != NULL ? mp_obj_new_str_from_cstr(value) : mp_const_none);
}

static void python_dict_store_bool(mp_obj_t dict, const char *key, bool value)
{
    mp_obj_dict_store(dict, python_key(key), mp_obj_new_bool(value));
}

static void python_dict_store_int(mp_obj_t dict, const char *key, mp_int_t value)
{
    mp_obj_dict_store(dict, python_key(key), mp_obj_new_int(value));
}

static void python_dict_store_uint(mp_obj_t dict, const char *key, mp_uint_t value)
{
    mp_obj_dict_store(dict, python_key(key), mp_obj_new_int_from_uint(value));
}

static void python_dict_store_u64(mp_obj_t dict, const char *key, uint64_t value)
{
    mp_obj_dict_store(dict, python_key(key), mp_obj_new_int_from_ull(value));
}

static void python_dict_store_float(mp_obj_t dict, const char *key, float value)
{
    mp_obj_dict_store(dict, python_key(key), mp_obj_new_float(value));
}

static void python_raise_esp(esp_err_t err)
{
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s"), esp_err_to_name(err));
}

static void python_check_esp(esp_err_t err)
{
    if (err != ESP_OK) {
        python_raise_esp(err);
    }
}

static const char *python_optional_str(size_t n_args,
                                       const mp_obj_t *args,
                                       size_t index,
                                       const char *fallback)
{
    if (index >= n_args || args[index] == mp_const_none) {
        return fallback;
    }
    return mp_obj_str_get_str(args[index]);
}

static uint32_t python_optional_u32(size_t n_args,
                                    const mp_obj_t *args,
                                    size_t index,
                                    uint32_t fallback)
{
    if (index >= n_args || args[index] == mp_const_none) {
        return fallback;
    }
    const mp_int_t value = mp_obj_get_int(args[index]);
    if (value < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected non-negative integer"));
    }
    return (uint32_t)value;
}

static uint32_t python_u32_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected non-negative integer"));
    }
    return (uint32_t)value;
}

static int32_t python_i32_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < INT32_MIN || value > INT32_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("integer out of range"));
    }
    return (int32_t)value;
}

static uint8_t python_u8_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < 0 || value > UINT8_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected 0..255"));
    }
    return (uint8_t)value;
}

static uint8_t python_optional_u8(size_t n_args,
                                  const mp_obj_t *args,
                                  size_t index,
                                  uint8_t fallback)
{
    if (index >= n_args || args[index] == mp_const_none) {
        return fallback;
    }
    return python_u8_from_obj(args[index]);
}

static size_t python_size_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected non-negative integer"));
    }
    return (size_t)value;
}

static uint16_t python_u16_from_size(size_t value)
{
    if (value > UINT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("value too large"));
    }
    return (uint16_t)value;
}

static uint8_t python_optional_tui_attr(size_t n_args, const mp_obj_t *args, size_t index)
{
    return python_optional_u8(n_args, args, index, SOLAR_OS_TUI_ATTR_NORMAL);
}

static uint32_t python_codepoint_from_obj(mp_obj_t obj)
{
    if (mp_obj_is_int(obj)) {
        return python_u32_from_obj(obj);
    }

    size_t len = 0;
    const char *text = mp_obj_str_get_data(obj, &len);
    if (len == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected a character"));
    }
    return (uint8_t)text[0];
}

static solar_os_gfx_color_t python_gfx_color_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < 0 || value > UINT8_MAX ||
        !solar_os_gfx_color_is_valid((solar_os_gfx_color_t)value)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected gfx color"));
    }
    return (solar_os_gfx_color_t)value;
}

static solar_os_gfx_font_t python_gfx_font_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < SOLAR_OS_GFX_FONT_SMALL || value >= SOLAR_OS_GFX_FONT_COUNT) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected gfx font"));
    }
    return (solar_os_gfx_font_t)value;
}

static void python_resolve_path_obj(mp_obj_t obj, char *path, size_t path_len)
{
    python_check_esp(solar_os_storage_resolve_path(mp_obj_str_get_str(obj), path, path_len));
}

static bool python_get_dict_int(mp_obj_t dict_obj, const char *key, int *out, bool required)
{
    if (!mp_obj_is_type(dict_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected dict"));
    }

    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(dict_obj);
    mp_map_elem_t *elem = mp_map_lookup(&dict->map, python_key(key), MP_MAP_LOOKUP);
    if (elem == NULL) {
        if (required) {
            mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("missing %s"), key);
        }
        return false;
    }

    *out = mp_obj_get_int(elem->value);
    return true;
}

static solar_os_datetime_t python_datetime_from_args(size_t n_args, const mp_obj_t *args)
{
    solar_os_datetime_t datetime = {0};

    if (n_args == 1) {
        int value = 0;
        python_get_dict_int(args[0], "year", &value, true);
        datetime.year = (uint16_t)value;
        python_get_dict_int(args[0], "month", &value, true);
        datetime.month = (uint8_t)value;
        python_get_dict_int(args[0], "day", &value, true);
        datetime.day = (uint8_t)value;
        python_get_dict_int(args[0], "hour", &value, true);
        datetime.hour = (uint8_t)value;
        python_get_dict_int(args[0], "minute", &value, true);
        datetime.minute = (uint8_t)value;
        python_get_dict_int(args[0], "second", &value, false);
        datetime.second = (uint8_t)value;
        datetime.clock_integrity = true;
        return datetime;
    }

    if (n_args < 5 || n_args > 6) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected dict or year,month,day,hour,minute[,second]"));
    }

    datetime.year = (uint16_t)mp_obj_get_int(args[0]);
    datetime.month = (uint8_t)mp_obj_get_int(args[1]);
    datetime.day = (uint8_t)mp_obj_get_int(args[2]);
    datetime.hour = (uint8_t)mp_obj_get_int(args[3]);
    datetime.minute = (uint8_t)mp_obj_get_int(args[4]);
    datetime.second = n_args >= 6 ? (uint8_t)mp_obj_get_int(args[5]) : 0;
    datetime.clock_integrity = true;
    return datetime;
}

static mp_obj_t python_datetime_to_dict(const solar_os_datetime_t *datetime)
{
    mp_obj_t dict = mp_obj_new_dict(8);
    python_dict_store_int(dict, "year", datetime->year);
    python_dict_store_int(dict, "month", datetime->month);
    python_dict_store_int(dict, "day", datetime->day);
    python_dict_store_int(dict, "hour", datetime->hour);
    python_dict_store_int(dict, "minute", datetime->minute);
    python_dict_store_int(dict, "second", datetime->second);
    python_dict_store_int(dict, "weekday", datetime->weekday);
    python_dict_store_bool(dict, "clock_integrity", datetime->clock_integrity);
    return dict;
}

static mp_obj_t python_storage_usage_to_dict(const solar_os_storage_usage_t *usage)
{
    mp_obj_t dict = mp_obj_new_dict(3);
    python_dict_store_u64(dict, "total_bytes", usage->total_bytes);
    python_dict_store_u64(dict, "used_bytes", usage->used_bytes);
    python_dict_store_u64(dict, "free_bytes", usage->free_bytes);
    return dict;
}

static mp_obj_t python_storage_block_to_dict(const solar_os_storage_block_t *block)
{
    mp_obj_t dict = mp_obj_new_dict(14);
    python_dict_store_cstr(dict, "name", block->name);
    python_dict_store_cstr(dict,
                           "type",
                           block->type == SOLAR_OS_STORAGE_BLOCK_DISK ? "disk" : "partition");
    python_dict_store_int(dict, "partition_number", block->partition_number);
    python_dict_store_int(dict, "mbr_type", block->mbr_type);
    python_dict_store_bool(dict, "bootable", block->bootable);
    python_dict_store_bool(dict, "mountable", block->mountable);
    python_dict_store_bool(dict, "mounted", block->mounted);
    python_dict_store_bool(dict, "whole_disk_filesystem", block->whole_disk_filesystem);
    python_dict_store_int(dict, "logical_volume", block->logical_volume);
    python_dict_store_u64(dict, "start_sector", block->start_sector);
    python_dict_store_u64(dict, "sector_count", block->sector_count);
    python_dict_store_uint(dict, "sector_size", block->sector_size);
    python_dict_store_u64(dict, "size_bytes", block->size_bytes);
    python_dict_store_cstr(dict, "fs", block->fs);
    python_dict_store_cstr(dict, "type_name", block->type_name);
    python_dict_store_cstr(dict, "mount_point", block->mount_point);
    return dict;
}

static mp_obj_t python_wifi_status_to_dict(const solar_os_wifi_status_t *status)
{
    mp_obj_t dict = mp_obj_new_dict(24);
    python_dict_store_cstr(dict, "state", solar_os_wifi_state_name(status->state));
    python_dict_store_bool(dict, "initialized", status->initialized);
    python_dict_store_bool(dict, "started", status->started);
    python_dict_store_bool(dict, "connected", status->connected);
    python_dict_store_bool(dict, "has_ip", status->has_ip);
    python_dict_store_bool(dict, "has_saved_config", status->has_saved_config);
    python_dict_store_bool(dict, "has_saved_ap_config", status->has_saved_ap_config);
    python_dict_store_bool(dict, "nat_enabled", status->nat_enabled);
    python_dict_store_bool(dict, "nat_active", status->nat_active);
    python_dict_store_bool(dict, "ap_enabled", status->ap_enabled);
    python_dict_store_bool(dict, "ap_running", status->ap_running);
    python_dict_store_cstr(dict, "ssid", status->ssid);
    python_dict_store_cstr(dict, "saved_ssid", status->saved_ssid);
    python_dict_store_cstr(dict, "saved_ap_ssid", status->saved_ap_ssid);
    python_dict_store_cstr(dict, "saved_ap_auth", status->saved_ap_auth);
    python_dict_store_cstr(dict, "ip", status->ip);
    python_dict_store_cstr(dict, "gateway", status->gateway);
    python_dict_store_cstr(dict, "netmask", status->netmask);
    python_dict_store_cstr(dict, "ap_ssid", status->ap_ssid);
    python_dict_store_cstr(dict, "ap_auth", status->ap_auth);
    python_dict_store_cstr(dict, "ap_ip", status->ap_ip);
    python_dict_store_int(dict, "rssi", status->rssi);
    python_dict_store_int(dict, "channel", status->channel);
    python_dict_store_int(dict, "disconnect_reason", status->disconnect_reason);
    python_dict_store_int(dict, "ap_channel", status->ap_channel);
    python_dict_store_int(dict, "ap_station_count", status->ap_station_count);
    python_dict_store_int(dict, "ap_max_connections", status->ap_max_connections);
    python_dict_store_int(dict, "saved_profile_count", status->saved_profile_count);
    python_dict_store_int(dict, "nat_last_error", status->nat_last_error);
    python_dict_store_cstr(dict, "nat_last_error_name", esp_err_to_name(status->nat_last_error));
    return dict;
}

static mp_obj_t python_audio_status_to_dict(const solar_os_audio_status_t *status)
{
    mp_obj_t dict = mp_obj_new_dict(14);
    python_dict_store_bool(dict, "initialized", status->initialized);
    python_dict_store_uint(dict, "sample_rate", status->sample_rate);
    python_dict_store_int(dict, "channels", status->channels);
    python_dict_store_int(dict, "bits_per_sample", status->bits_per_sample);
    python_dict_store_int(dict, "volume", status->volume);
    python_dict_store_float(dict, "mic_gain_db", status->mic_gain_db);
    python_dict_store_int(dict, "i2s_port", status->i2s_port);
    python_dict_store_int(dict, "mclk_pin", status->mclk_pin);
    python_dict_store_int(dict, "bclk_pin", status->bclk_pin);
    python_dict_store_int(dict, "ws_pin", status->ws_pin);
    python_dict_store_int(dict, "din_pin", status->din_pin);
    python_dict_store_int(dict, "dout_pin", status->dout_pin);
    python_dict_store_int(dict, "pa_pin", status->pa_pin);
    python_dict_store_cstr(dict, "output_codec", status->output_codec);
    python_dict_store_cstr(dict, "input_codec", status->input_codec);
    return dict;
}

static mp_obj_t python_mqtt_status_to_dict(const solar_os_mqtt_status_t *status)
{
    mp_obj_t dict = mp_obj_new_dict(16);
    python_dict_store_bool(dict, "initialized", status->initialized);
    python_dict_store_bool(dict, "configured", status->configured);
    python_dict_store_bool(dict, "running", status->running);
    python_dict_store_bool(dict, "connected", status->connected);
    python_dict_store_bool(dict, "username_set", status->username_set);
    python_dict_store_bool(dict, "password_set", status->password_set);
    python_dict_store_cstr(dict, "url", status->url);
    python_dict_store_cstr(dict, "username", status->username);
    python_dict_store_cstr(dict, "client_id", status->client_id);
    python_dict_store_cstr(dict, "last_error", status->last_error);
    python_dict_store_int(dict, "last_esp_error", status->last_esp_error);
    python_dict_store_int(dict, "last_msg_id", status->last_msg_id);
    python_dict_store_uint(dict, "rx_count", status->rx_count);
    python_dict_store_uint(dict, "tx_count", status->tx_count);
    python_dict_store_uint(dict, "dropped_count", status->dropped_count);
    python_dict_store_uint(dict, "queued_messages", status->queued_messages);
    return dict;
}

static mp_obj_t python_mqtt_message_to_dict(const solar_os_mqtt_message_t *message)
{
    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_cstr(dict, "topic", message->topic);
    mp_obj_dict_store(dict,
                      python_key("payload"),
                      mp_obj_new_bytes((const byte *)message->payload, message->payload_len));
    python_dict_store_uint(dict, "payload_len", message->payload_len);
    python_dict_store_int(dict, "qos", message->qos);
    python_dict_store_bool(dict, "retain", message->retain);
    python_dict_store_bool(dict, "truncated", message->truncated);
    return dict;
}

static mp_obj_t python_wav_info_to_dict(const solar_os_audio_wav_info_t *info)
{
    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_uint(dict, "sample_rate", info->sample_rate);
    python_dict_store_uint(dict, "data_bytes", info->data_bytes);
    python_dict_store_uint(dict, "duration_ms", info->duration_ms);
    python_dict_store_uint(dict, "block_align", info->block_align);
    python_dict_store_int(dict, "channels", info->channels);
    python_dict_store_int(dict, "bits_per_sample", info->bits_per_sample);
    return dict;
}

static bool python_should_cancel(void *user)
{
    (void)user;
    return python_app.stop_requested;
}

static mp_obj_t python_new_submodule(mp_obj_t parent, const char *name)
{
    char full_name[48];
    snprintf(full_name, sizeof(full_name), "solaros.%s", name);
    mp_obj_t module = mp_obj_new_module(qstr_from_str(full_name));
    python_module_store(parent, name, module);
    return module;
}

static mp_obj_t solaros_write(mp_obj_t text_obj)
{
    size_t len = 0;
    const char *text = mp_obj_str_get_data(text_obj, &len);
    (void)python_send_output(text, len);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_write_obj, solaros_write);

static mp_obj_t solaros_version(void)
{
    return mp_obj_new_str_from_cstr(SOLAR_OS_VERSION);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_version_obj, solaros_version);

static mp_obj_t solaros_should_exit(void)
{
    return mp_obj_new_bool(python_app.stop_requested);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_should_exit_obj, solaros_should_exit);

static mp_obj_t python_builtin_exit(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) {
        mp_raise_type(&mp_type_SystemExit);
    }

    mp_raise_type_arg(&mp_type_SystemExit, args[0]);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(python_builtin_exit_obj, 0, 1, python_builtin_exit);

static mp_obj_t solaros_battery(void)
{
    solar_os_battery_status_t status;
    if (solar_os_battery_get_status(&status) != ESP_OK) {
        return mp_const_none;
    }

    mp_obj_t dict = mp_obj_new_dict(5);
    python_dict_store_int(dict, "voltage_mv", status.voltage_mv);
    python_dict_store_int(dict, "percent", status.percent);
    python_dict_store_bool(dict, "percent_estimated", status.percent_estimated);
    python_dict_store_bool(dict, "adc_calibrated", status.adc_calibrated);
    python_dict_store_bool(dict, "external_power", status.external_power);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_battery_obj, solaros_battery);

static mp_obj_t solaros_wifi(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    mp_obj_t dict = mp_obj_new_dict(12);
    python_dict_store_cstr(dict, "state", solar_os_wifi_state_name(status.state));
    python_dict_store_bool(dict, "started", status.started);
    python_dict_store_bool(dict, "connected", status.connected);
    python_dict_store_bool(dict, "has_ip", status.has_ip);
    python_dict_store_cstr(dict, "ssid", status.ssid);
    python_dict_store_cstr(dict, "ip", status.ip);
    python_dict_store_int(dict, "rssi", status.rssi);
    python_dict_store_bool(dict, "ap_running", status.ap_running);
    python_dict_store_cstr(dict, "ap_ssid", status.ap_ssid);
    python_dict_store_cstr(dict, "ap_ip", status.ap_ip);
    python_dict_store_bool(dict, "nat_enabled", status.nat_enabled);
    python_dict_store_bool(dict, "nat_active", status.nat_active);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_obj, solaros_wifi);

static mp_obj_t solaros_environment(void)
{
    solar_os_environment_t environment;
    if (solar_os_sensors_read_environment(&environment) != ESP_OK) {
        return mp_const_none;
    }

    mp_obj_t dict = mp_obj_new_dict(2);
    python_dict_store_float(dict, "temperature_c", environment.temperature_c);
    python_dict_store_float(dict, "humidity_percent", environment.humidity_percent);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_environment_obj, solaros_environment);

static mp_obj_t solaros_storage_status(void)
{
    char status[96];
    solar_os_storage_get_status(status, sizeof(status));
    return mp_obj_new_str_from_cstr(status);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_status_obj, solaros_storage_status);

static mp_obj_t solaros_storage_is_mounted(void)
{
    return mp_obj_new_bool(solar_os_storage_is_mounted());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_is_mounted_obj, solaros_storage_is_mounted);

static mp_obj_t solaros_storage_mount(void)
{
    python_check_esp(solar_os_storage_mount());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_mount_obj, solaros_storage_mount);

static mp_obj_t solaros_storage_unmount(void)
{
    python_check_esp(solar_os_storage_unmount());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_unmount_obj, solaros_storage_unmount);

static mp_obj_t solaros_storage_mount_point(void)
{
    return mp_obj_new_str_from_cstr(solar_os_storage_mount_point());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_mount_point_obj, solaros_storage_mount_point);

static mp_obj_t solaros_storage_usage(size_t n_args, const mp_obj_t *args)
{
    solar_os_storage_usage_t usage;
    esp_err_t err;
    if (n_args == 0) {
        err = solar_os_storage_get_usage(&usage);
    } else {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        python_resolve_path_obj(args[0], path, sizeof(path));
        err = solar_os_storage_get_usage_for_path(path, &usage);
    }
    python_check_esp(err);
    return python_storage_usage_to_dict(&usage);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_usage_obj, 0, 1, solaros_storage_usage);

static mp_obj_t solaros_storage_resolve(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    return mp_obj_new_str_from_cstr(path);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_resolve_obj, solaros_storage_resolve);

static mp_obj_t solaros_storage_rescan(void)
{
    python_check_esp(solar_os_storage_rescan());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_rescan_obj, solaros_storage_rescan);

static mp_obj_t solaros_storage_blocks(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    const size_t count = solar_os_storage_block_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (solar_os_storage_get_block(i, &block)) {
            mp_obj_list_append(list, python_storage_block_to_dict(&block));
        }
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_blocks_obj, solaros_storage_blocks);

static mp_obj_t solaros_storage_block_count(void)
{
    return mp_obj_new_int_from_uint(solar_os_storage_block_count());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_block_count_obj, solaros_storage_block_count);

static mp_obj_t solaros_storage_block(mp_obj_t index_obj)
{
    const mp_int_t index = mp_obj_get_int(index_obj);
    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected non-negative index"));
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        python_raise_esp(ESP_ERR_NOT_FOUND);
    }
    return python_storage_block_to_dict(&block);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_block_obj, solaros_storage_block);

static mp_obj_t solaros_storage_usage_for_block(mp_obj_t index_obj)
{
    const mp_int_t index = mp_obj_get_int(index_obj);
    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected non-negative index"));
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        python_raise_esp(ESP_ERR_NOT_FOUND);
    }

    solar_os_storage_usage_t usage;
    python_check_esp(solar_os_storage_get_usage_for_block(&block, &usage));
    return python_storage_usage_to_dict(&usage);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_usage_for_block_obj, solaros_storage_usage_for_block);

static mp_obj_t solaros_storage_mkdir(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    python_check_esp(solar_os_storage_mkdir(path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_mkdir_obj, solaros_storage_mkdir);

static mp_obj_t solaros_storage_rmdir(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    python_check_esp(solar_os_storage_rmdir(path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_rmdir_obj, solaros_storage_rmdir);

static mp_obj_t solaros_storage_remove(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    python_check_esp(solar_os_storage_remove(path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_remove_obj, solaros_storage_remove);

static mp_obj_t solaros_storage_rename(mp_obj_t old_path_obj, mp_obj_t new_path_obj)
{
    char old_path[SOLAR_OS_STORAGE_PATH_MAX];
    char new_path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(old_path_obj, old_path, sizeof(old_path));
    python_resolve_path_obj(new_path_obj, new_path, sizeof(new_path));
    python_check_esp(solar_os_storage_rename(old_path, new_path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_storage_rename_obj, solaros_storage_rename);

static mp_obj_t solaros_storage_copy(mp_obj_t source_obj, mp_obj_t dest_obj)
{
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char dest[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(source_obj, source, sizeof(source));
    python_resolve_path_obj(dest_obj, dest, sizeof(dest));
    python_check_esp(solar_os_storage_copy_file(source, dest));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_storage_copy_obj, solaros_storage_copy);

static mp_obj_t solaros_storage_mount_volume(size_t n_args, const mp_obj_t *args)
{
    const char *name = mp_obj_str_get_str(args[0]);
    const char *mount_point = python_optional_str(n_args, args, 1, NULL);
    python_check_esp(solar_os_storage_mount_volume(name, mount_point));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_mount_volume_obj,
                                    1,
                                    2,
                                    solaros_storage_mount_volume);

static mp_obj_t solaros_storage_unmount_volume(mp_obj_t target_obj)
{
    python_check_esp(solar_os_storage_unmount_volume(mp_obj_str_get_str(target_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_unmount_volume_obj, solaros_storage_unmount_volume);

static mp_obj_t solaros_time_uptime_ms(void)
{
    return mp_obj_new_int_from_ull(solar_os_time_uptime_ms());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_time_uptime_ms_obj, solaros_time_uptime_ms);

static mp_obj_t solaros_time_uptime(void)
{
    char buffer[48];
    solar_os_time_format_uptime(solar_os_time_uptime_ms(), buffer, sizeof(buffer));
    return mp_obj_new_str_from_cstr(buffer);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_time_uptime_obj, solaros_time_uptime);

static mp_obj_t solaros_time_datetime(void)
{
    solar_os_datetime_t datetime;
    python_check_esp(solar_os_time_get_datetime(&datetime));
    return python_datetime_to_dict(&datetime);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_time_datetime_obj, solaros_time_datetime);

static mp_obj_t solaros_time_utc_datetime(void)
{
    solar_os_datetime_t datetime;
    python_check_esp(solar_os_time_get_utc_datetime(&datetime));
    return python_datetime_to_dict(&datetime);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_time_utc_datetime_obj, solaros_time_utc_datetime);

static mp_obj_t solaros_time_set_datetime(size_t n_args, const mp_obj_t *args)
{
    solar_os_datetime_t datetime = python_datetime_from_args(n_args, args);
    python_check_esp(solar_os_time_set_datetime(&datetime));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_time_set_datetime_obj,
                                    1,
                                    6,
                                    solaros_time_set_datetime);

static mp_obj_t solaros_time_set_utc_datetime(size_t n_args, const mp_obj_t *args)
{
    solar_os_datetime_t datetime = python_datetime_from_args(n_args, args);
    python_check_esp(solar_os_time_set_utc_datetime(&datetime));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_time_set_utc_datetime_obj,
                                    1,
                                    6,
                                    solaros_time_set_utc_datetime);

static mp_obj_t solaros_time_utc_to_local(size_t n_args, const mp_obj_t *args)
{
    solar_os_datetime_t utc = python_datetime_from_args(n_args, args);
    solar_os_datetime_t local;
    python_check_esp(solar_os_time_utc_to_local(&utc, &local));
    return python_datetime_to_dict(&local);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_time_utc_to_local_obj,
                                    1,
                                    6,
                                    solaros_time_utc_to_local);

static mp_obj_t solaros_time_local_to_utc(size_t n_args, const mp_obj_t *args)
{
    solar_os_datetime_t local = python_datetime_from_args(n_args, args);
    solar_os_datetime_t utc;
    python_check_esp(solar_os_time_local_to_utc(&local, &utc));
    return python_datetime_to_dict(&utc);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_time_local_to_utc_obj,
                                    1,
                                    6,
                                    solaros_time_local_to_utc);

static mp_obj_t solaros_time_is_valid(size_t n_args, const mp_obj_t *args)
{
    solar_os_datetime_t datetime = python_datetime_from_args(n_args, args);
    return mp_obj_new_bool(solar_os_time_datetime_is_valid(&datetime));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_time_is_valid_obj, 1, 6, solaros_time_is_valid);

static mp_obj_t solaros_time_timezone(void)
{
    char name[SOLAR_OS_TIMEZONE_NAME_MAX];
    char posix[SOLAR_OS_TIMEZONE_POSIX_MAX];
    solar_os_time_get_timezone(name, sizeof(name), posix, sizeof(posix));

    mp_obj_t dict = mp_obj_new_dict(2);
    python_dict_store_cstr(dict, "name", name);
    python_dict_store_cstr(dict, "posix", posix);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_time_timezone_obj, solaros_time_timezone);

static mp_obj_t solaros_time_set_timezone(mp_obj_t timezone_obj)
{
    python_check_esp(solar_os_time_set_timezone(mp_obj_str_get_str(timezone_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_time_set_timezone_obj, solaros_time_set_timezone);

static mp_obj_t solaros_time_ntp_sync(size_t n_args, const mp_obj_t *args)
{
    const char *server = python_optional_str(n_args, args, 0, SOLAR_OS_NTP_DEFAULT_SERVER);
    const uint32_t timeout_ms = python_optional_u32(n_args,
                                                    args,
                                                    1,
                                                    SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS);
    solar_os_datetime_t utc;
    solar_os_datetime_t local;
    python_check_esp(solar_os_time_ntp_sync(server, timeout_ms, &utc, &local));

    mp_obj_t dict = mp_obj_new_dict(2);
    mp_obj_dict_store(dict, python_key("utc"), python_datetime_to_dict(&utc));
    mp_obj_dict_store(dict, python_key("local"), python_datetime_to_dict(&local));
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_time_ntp_sync_obj, 0, 2, solaros_time_ntp_sync);

static mp_obj_t solaros_battery_status(void)
{
    return solaros_battery();
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_battery_status_obj, solaros_battery_status);

static mp_obj_t solaros_sensors_environment(void)
{
    return solaros_environment();
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_sensors_environment_obj, solaros_sensors_environment);

static mp_obj_t solaros_wifi_status(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    return python_wifi_status_to_dict(&status);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_status_obj, solaros_wifi_status);

static mp_obj_t solaros_wifi_status_text(void)
{
    char text[96];
    solar_os_wifi_get_status_text(text, sizeof(text));
    return mp_obj_new_str_from_cstr(text);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_status_text_obj, solaros_wifi_status_text);

static mp_obj_t solaros_wifi_start(void)
{
    python_check_esp(solar_os_wifi_start());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_start_obj, solaros_wifi_start);

static mp_obj_t solaros_wifi_stop(void)
{
    python_check_esp(solar_os_wifi_stop());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_stop_obj, solaros_wifi_stop);

static mp_obj_t solaros_wifi_connect(size_t n_args, const mp_obj_t *args)
{
    const char *ssid = mp_obj_str_get_str(args[0]);
    const char *password = python_optional_str(n_args, args, 1, "");
    python_check_esp(solar_os_wifi_connect(ssid, password));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_wifi_connect_obj, 1, 2, solaros_wifi_connect);

static mp_obj_t solaros_wifi_connect_saved(void)
{
    python_check_esp(solar_os_wifi_connect_saved());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_connect_saved_obj, solaros_wifi_connect_saved);

static mp_obj_t solaros_wifi_disconnect(void)
{
    python_check_esp(solar_os_wifi_disconnect());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_disconnect_obj, solaros_wifi_disconnect);

static mp_obj_t solaros_wifi_forget(void)
{
    python_check_esp(solar_os_wifi_forget());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_forget_obj, solaros_wifi_forget);

static mp_obj_t solaros_wifi_forget_ssid(mp_obj_t ssid_obj)
{
    python_check_esp(solar_os_wifi_forget_ssid(mp_obj_str_get_str(ssid_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_wifi_forget_ssid_obj, solaros_wifi_forget_ssid);

static mp_obj_t solaros_wifi_forget_all(void)
{
    python_check_esp(solar_os_wifi_forget_all());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_forget_all_obj, solaros_wifi_forget_all);

static mp_obj_t solaros_wifi_scan(void)
{
    solar_os_wifi_ap_t aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t found = 0;
    python_check_esp(solar_os_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found));

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < found; i++) {
        mp_obj_t dict = mp_obj_new_dict(5);
        python_dict_store_cstr(dict, "ssid", aps[i].ssid);
        python_dict_store_cstr(dict, "auth", aps[i].auth);
        python_dict_store_int(dict, "rssi", aps[i].rssi);
        python_dict_store_int(dict, "channel", aps[i].channel);
        python_dict_store_bool(dict, "hidden", aps[i].hidden);
        mp_obj_list_append(list, dict);
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_scan_obj, solaros_wifi_scan);

static mp_obj_t solaros_wifi_known(void)
{
    solar_os_wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;
    python_check_esp(solar_os_wifi_known(profiles,
                                         sizeof(profiles) / sizeof(profiles[0]),
                                         &count));

    mp_obj_t list = mp_obj_new_list(0, NULL);
    const size_t shown = count < SOLAR_OS_WIFI_PROFILE_MAX ? count : SOLAR_OS_WIFI_PROFILE_MAX;
    for (size_t i = 0; i < shown; i++) {
        mp_obj_t dict = mp_obj_new_dict(2);
        python_dict_store_cstr(dict, "ssid", profiles[i].ssid);
        python_dict_store_bool(dict, "preferred", profiles[i].preferred);
        mp_obj_list_append(list, dict);
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_known_obj, solaros_wifi_known);

static mp_obj_t solaros_wifi_ap_start(size_t n_args, const mp_obj_t *args)
{
    const char *ssid = python_optional_str(n_args, args, 0, NULL);
    const char *password = python_optional_str(n_args, args, 1, NULL);
    const char *auth = python_optional_str(n_args, args, 2, NULL);
    python_check_esp(solar_os_wifi_ap_start(ssid, password, auth));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_wifi_ap_start_obj, 0, 3, solaros_wifi_ap_start);

static mp_obj_t solaros_wifi_ap_stop(void)
{
    python_check_esp(solar_os_wifi_ap_stop());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_wifi_ap_stop_obj, solaros_wifi_ap_stop);

static mp_obj_t solaros_wifi_nat(mp_obj_t enabled_obj)
{
    python_check_esp(solar_os_wifi_nat_set(mp_obj_is_true(enabled_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_wifi_nat_obj, solaros_wifi_nat);

static mp_obj_t solaros_mqtt_status(void)
{
    solar_os_mqtt_status_t status;
    python_check_esp(solar_os_mqtt_get_status(&status));
    return python_mqtt_status_to_dict(&status);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_mqtt_status_obj, solaros_mqtt_status);

static mp_obj_t solaros_mqtt_connect(size_t n_args, const mp_obj_t *args)
{
    const char *url = python_optional_str(n_args, args, 0, NULL);
    const char *username = python_optional_str(n_args, args, 1, NULL);
    const char *password = python_optional_str(n_args, args, 2, NULL);
    python_check_esp(solar_os_mqtt_connect(url, username, password));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_mqtt_connect_obj, 0, 3, solaros_mqtt_connect);

static mp_obj_t solaros_mqtt_disconnect(void)
{
    python_check_esp(solar_os_mqtt_disconnect());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_mqtt_disconnect_obj, solaros_mqtt_disconnect);

static mp_obj_t solaros_mqtt_publish(size_t n_args, const mp_obj_t *args)
{
    const char *topic = mp_obj_str_get_str(args[0]);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[1], &bufinfo, MP_BUFFER_READ);
    const int qos = n_args >= 3 && args[2] != mp_const_none ? mp_obj_get_int(args[2]) : 0;
    const bool retain = n_args >= 4 && args[3] != mp_const_none ? mp_obj_is_true(args[3]) : false;

    int msg_id = 0;
    python_check_esp(solar_os_mqtt_publish(topic,
                                           bufinfo.buf,
                                           bufinfo.len,
                                           qos,
                                           retain,
                                           &msg_id));
    return mp_obj_new_int(msg_id);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_mqtt_publish_obj, 2, 4, solaros_mqtt_publish);

static mp_obj_t solaros_mqtt_subscribe(size_t n_args, const mp_obj_t *args)
{
    const char *topic = mp_obj_str_get_str(args[0]);
    const int qos = n_args >= 2 && args[1] != mp_const_none ? mp_obj_get_int(args[1]) : 0;
    int msg_id = 0;
    python_check_esp(solar_os_mqtt_subscribe(topic, qos, &msg_id));
    return mp_obj_new_int(msg_id);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_mqtt_subscribe_obj, 1, 2, solaros_mqtt_subscribe);

static mp_obj_t solaros_mqtt_read(size_t n_args, const mp_obj_t *args)
{
    const uint32_t timeout_ms = python_optional_u32(n_args, args, 0, 0);
    solar_os_mqtt_message_t message;
    const esp_err_t err = solar_os_mqtt_read_message(&message, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        return mp_const_none;
    }
    python_check_esp(err);
    return python_mqtt_message_to_dict(&message);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_mqtt_read_obj, 0, 1, solaros_mqtt_read);

static int python_gpio_pin_from_obj(mp_obj_t obj)
{
    const mp_int_t pin = mp_obj_get_int(obj);
    if (pin < 0 || pin > 48) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected GPIO pin 0..48"));
    }
    return (int)pin;
}

static solar_os_gpio_mode_t python_gpio_mode_from_obj(mp_obj_t obj)
{
    if (mp_obj_is_int(obj)) {
        const mp_int_t value = mp_obj_get_int(obj);
        if (value == SOLAR_OS_GPIO_MODE_INPUT || value == SOLAR_OS_GPIO_MODE_OUTPUT) {
            return (solar_os_gpio_mode_t)value;
        }
        mp_raise_ValueError(MP_ERROR_TEXT("expected GPIO mode"));
    }

    solar_os_gpio_mode_t mode;
    if (!solar_os_gpio_parse_mode(mp_obj_str_get_str(obj), &mode)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected input or output"));
    }
    return mode;
}

static solar_os_gpio_pull_t python_gpio_pull_from_obj(mp_obj_t obj)
{
    if (mp_obj_is_int(obj)) {
        const mp_int_t value = mp_obj_get_int(obj);
        if (value >= SOLAR_OS_GPIO_PULL_NONE && value <= SOLAR_OS_GPIO_PULL_DOWN) {
            return (solar_os_gpio_pull_t)value;
        }
        mp_raise_ValueError(MP_ERROR_TEXT("expected GPIO pull"));
    }

    solar_os_gpio_pull_t pull;
    if (!solar_os_gpio_parse_pull(mp_obj_str_get_str(obj), &pull)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected none, up, or down"));
    }
    return pull;
}

static mp_obj_t python_gpio_info_to_dict(const solar_os_gpio_pin_info_t *info)
{
    mp_obj_t dict = mp_obj_new_dict(8);
    python_dict_store_int(dict, "pin", info->pin);
    python_dict_store_bool(dict, "allowed", info->runtime_allowed);
    python_dict_store_cstr(dict, "role", info->role);
    python_dict_store_bool(dict, "configured", info->configured);
    python_dict_store_cstr(dict,
                           "mode",
                           info->configured ? solar_os_gpio_mode_name(info->mode) : NULL);
    python_dict_store_cstr(dict,
                           "pull",
                           info->configured ? solar_os_gpio_pull_name(info->pull) : NULL);
    python_dict_store_int(dict, "level", info->level ? 1 : 0);
    python_dict_store_bool(dict, "level_valid", info->level_valid);
    return dict;
}

static mp_obj_t solaros_gpio_pins(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (solar_os_gpio_get_pin_info(i, &info)) {
            mp_obj_list_append(list, python_gpio_info_to_dict(&info));
        }
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_gpio_pins_obj, solaros_gpio_pins);

static mp_obj_t solaros_gpio_allowed(mp_obj_t pin_obj)
{
    return mp_obj_new_bool(solar_os_gpio_is_runtime_allowed(python_gpio_pin_from_obj(pin_obj)));
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_gpio_allowed_obj, solaros_gpio_allowed);

static mp_obj_t solaros_gpio_mode(size_t n_args, const mp_obj_t *args)
{
    const int pin = python_gpio_pin_from_obj(args[0]);

    if (n_args == 1) {
        solar_os_gpio_pin_info_t info;
        if (!solar_os_gpio_get_pin_info_by_pin(pin, &info)) {
            mp_raise_ValueError(MP_ERROR_TEXT("not an expansion GPIO"));
        }
        return python_gpio_info_to_dict(&info);
    }

    const solar_os_gpio_mode_t mode = python_gpio_mode_from_obj(args[1]);
    const solar_os_gpio_pull_t pull =
        n_args >= 3 ? python_gpio_pull_from_obj(args[2]) : SOLAR_OS_GPIO_PULL_NONE;
    python_check_esp(solar_os_gpio_configure(pin, mode, pull));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gpio_mode_obj, 1, 3, solaros_gpio_mode);

static mp_obj_t solaros_gpio_read(mp_obj_t pin_obj)
{
    bool level = false;
    python_check_esp(solar_os_gpio_read(python_gpio_pin_from_obj(pin_obj), &level));
    return mp_obj_new_int(level ? 1 : 0);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_gpio_read_obj, solaros_gpio_read);

static mp_obj_t solaros_gpio_write(mp_obj_t pin_obj, mp_obj_t level_obj)
{
    python_check_esp(solar_os_gpio_write(python_gpio_pin_from_obj(pin_obj),
                                         mp_obj_is_true(level_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_gpio_write_obj, solaros_gpio_write);

static mp_obj_t solaros_led_status(void)
{
    bool on = false;
    python_check_esp(solar_os_status_led_get(&on));
    return mp_obj_new_bool(on);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_led_status_obj, solaros_led_status);

static mp_obj_t solaros_led_set(mp_obj_t on_obj)
{
    const bool on = mp_obj_is_true(on_obj);
    python_check_esp(solar_os_status_led_set(on));
    return mp_obj_new_bool(on);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_led_set_obj, solaros_led_set);

static mp_obj_t solaros_led_on(void)
{
    python_check_esp(solar_os_status_led_set(true));
    return mp_const_true;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_led_on_obj, solaros_led_on);

static mp_obj_t solaros_led_off(void)
{
    python_check_esp(solar_os_status_led_set(false));
    return mp_const_false;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_led_off_obj, solaros_led_off);

static mp_obj_t solaros_led_toggle(void)
{
    bool on = false;
    python_check_esp(solar_os_status_led_toggle(&on));
    return mp_obj_new_bool(on);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_led_toggle_obj, solaros_led_toggle);

static mp_obj_t python_adc_info_to_dict(const solar_os_adc_pin_info_t *info)
{
    mp_obj_t dict = mp_obj_new_dict(5);
    python_dict_store_int(dict, "pin", info->pin);
    python_dict_store_bool(dict, "allowed", info->runtime_allowed);
    python_dict_store_bool(dict, "adc_capable", info->adc_capable);
    python_dict_store_int(dict, "unit", info->unit);
    python_dict_store_int(dict, "channel", info->channel);
    return dict;
}

static mp_obj_t python_adc_sample_to_dict(const solar_os_adc_sample_t *sample)
{
    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_int(dict, "pin", sample->pin);
    python_dict_store_int(dict, "raw", sample->raw);
    python_dict_store_int(dict, "voltage_mv", sample->voltage_mv);
    python_dict_store_int(dict, "unit", sample->unit);
    python_dict_store_int(dict, "channel", sample->channel);
    python_dict_store_bool(dict, "calibrated", sample->calibrated);
    return dict;
}

static mp_obj_t solaros_adc_pins(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < solar_os_adc_pin_count(); i++) {
        solar_os_adc_pin_info_t info;
        if (solar_os_adc_get_pin_info(i, &info)) {
            mp_obj_list_append(list, python_adc_info_to_dict(&info));
        }
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_adc_pins_obj, solaros_adc_pins);

static mp_obj_t solaros_adc_read(mp_obj_t pin_obj)
{
    solar_os_adc_sample_t sample;
    python_check_esp(solar_os_adc_read(python_gpio_pin_from_obj(pin_obj), &sample));
    return python_adc_sample_to_dict(&sample);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_adc_read_obj, solaros_adc_read);

static mp_obj_t python_pwm_info_to_dict(const solar_os_pwm_pin_info_t *info)
{
    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_int(dict, "pin", info->pin);
    python_dict_store_bool(dict, "allowed", info->runtime_allowed);
    python_dict_store_bool(dict, "active", info->active);
    python_dict_store_int(dict, "channel", info->channel);
    python_dict_store_uint(dict, "freq_hz", info->freq_hz);
    python_dict_store_int(dict, "duty_percent", info->duty_percent);
    return dict;
}

static mp_obj_t solaros_pwm_status(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < solar_os_pwm_pin_count(); i++) {
        solar_os_pwm_pin_info_t info;
        if (solar_os_pwm_get_pin_info(i, &info)) {
            mp_obj_list_append(list, python_pwm_info_to_dict(&info));
        }
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_pwm_status_obj, solaros_pwm_status);

static mp_obj_t solaros_pwm_set(mp_obj_t pin_obj, mp_obj_t freq_obj, mp_obj_t duty_obj)
{
    const uint32_t freq_hz = python_u32_from_obj(freq_obj);
    const uint32_t duty_percent = python_u32_from_obj(duty_obj);
    if (duty_percent > SOLAR_OS_PWM_DUTY_MAX_PERCENT) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected duty 0..100"));
    }
    python_check_esp(solar_os_pwm_set(python_gpio_pin_from_obj(pin_obj),
                                      freq_hz,
                                      (uint8_t)duty_percent));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_pwm_set_obj, solaros_pwm_set);

static mp_obj_t solaros_pwm_off(mp_obj_t pin_obj)
{
    python_check_esp(solar_os_pwm_stop(python_gpio_pin_from_obj(pin_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_pwm_off_obj, solaros_pwm_off);

static mp_obj_t solaros_i2c_info(void)
{
    mp_obj_t dict = mp_obj_new_dict(3);
    python_dict_store_uint(dict, "speed_hz", solar_os_i2c_get_speed_hz());
    python_dict_store_int(dict, "sda_pin", solar_os_i2c_get_sda_pin());
    python_dict_store_int(dict, "scl_pin", solar_os_i2c_get_scl_pin());
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_i2c_info_obj, solaros_i2c_info);

static mp_obj_t solaros_i2c_probe(mp_obj_t address_obj)
{
    python_check_esp(solar_os_i2c_probe(python_u8_from_obj(address_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_i2c_probe_obj, solaros_i2c_probe);

static mp_obj_t solaros_i2c_scan(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (uint8_t address = SOLAR_OS_I2C_SCAN_MIN_ADDR;
         address <= SOLAR_OS_I2C_SCAN_MAX_ADDR;
         address++) {
        if (solar_os_i2c_probe(address) == ESP_OK) {
            mp_obj_list_append(list, mp_obj_new_int(address));
        }
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_i2c_scan_obj, solaros_i2c_scan);

static mp_obj_t solaros_i2c_read_reg(mp_obj_t address_obj, mp_obj_t reg_obj, mp_obj_t len_obj)
{
    const uint8_t address = python_u8_from_obj(address_obj);
    const uint8_t reg = python_u8_from_obj(reg_obj);
    const mp_int_t len = mp_obj_get_int(len_obj);
    if (len <= 0 || len > 256) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected length 1..256"));
    }

    uint8_t data[256];
    python_check_esp(solar_os_i2c_read_reg(address, reg, data, (size_t)len));
    return mp_obj_new_bytes(data, (size_t)len);
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_i2c_read_reg_obj, solaros_i2c_read_reg);

static mp_obj_t solaros_i2c_write_reg(mp_obj_t address_obj, mp_obj_t reg_obj, mp_obj_t data_obj)
{
    const uint8_t address = python_u8_from_obj(address_obj);
    const uint8_t reg = python_u8_from_obj(reg_obj);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    python_check_esp(solar_os_i2c_write_reg(address, reg, bufinfo.buf, bufinfo.len));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_i2c_write_reg_obj, solaros_i2c_write_reg);

static mp_obj_t solaros_uart_status(void)
{
    solar_os_uart_status_t status;
    solar_os_uart_get_status(&status);

    mp_obj_t dict = mp_obj_new_dict(8);
    python_dict_store_bool(dict, "initialized", status.initialized);
    python_dict_store_int(dict, "port_num", status.port_num);
    python_dict_store_int(dict, "tx_pin", status.tx_pin);
    python_dict_store_int(dict, "rx_pin", status.rx_pin);
    python_dict_store_uint(dict, "baud_rate", status.baud_rate);
    python_dict_store_cstr(dict, "mode", solar_os_uart_mode_name(status.mode));
    python_dict_store_uint(dict, "rx_buffered", status.rx_buffered);
    python_dict_store_bool(dict, "rx_buffered_valid", status.rx_buffered_valid);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_uart_status_obj, solaros_uart_status);

static mp_obj_t solaros_uart_baud(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) {
        solar_os_uart_status_t status;
        solar_os_uart_get_status(&status);
        return mp_obj_new_int_from_uint(status.baud_rate);
    }

    const uint32_t baud_rate = python_optional_u32(n_args, args, 0, 0);
    python_check_esp(solar_os_uart_set_baud_rate(baud_rate));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_uart_baud_obj, 0, 1, solaros_uart_baud);

static mp_obj_t solaros_uart_is_valid_baud(mp_obj_t baud_obj)
{
    const mp_int_t baud_rate = mp_obj_get_int(baud_obj);
    return mp_obj_new_bool(baud_rate >= 0 &&
                           solar_os_uart_is_valid_baud_rate((uint32_t)baud_rate));
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_uart_is_valid_baud_obj, solaros_uart_is_valid_baud);

static mp_obj_t solaros_uart_mode(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) {
        solar_os_uart_status_t status;
        solar_os_uart_get_status(&status);
        return mp_obj_new_str_from_cstr(solar_os_uart_mode_name(status.mode));
    }

    solar_os_uart_mode_t mode;
    if (!solar_os_uart_parse_mode(mp_obj_str_get_str(args[0]), &mode)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected raw or line"));
    }
    python_check_esp(solar_os_uart_set_mode(mode));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_uart_mode_obj, 0, 1, solaros_uart_mode);

static mp_obj_t solaros_uart_write(mp_obj_t data_obj)
{
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    size_t written = 0;
    python_check_esp(solar_os_uart_write(bufinfo.buf, bufinfo.len, &written));
    return mp_obj_new_int_from_uint(written);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_uart_write_obj, solaros_uart_write);

static mp_obj_t solaros_uart_read(size_t n_args, const mp_obj_t *args)
{
    const uint32_t len = python_optional_u32(n_args, args, 0, 64);
    const uint32_t timeout_ms = python_optional_u32(n_args, args, 1, 0);
    if (len == 0 || len > 512) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected length 1..512"));
    }

    uint8_t data[512];
    size_t read_len = 0;
    python_check_esp(solar_os_uart_read(data, len, timeout_ms, &read_len));
    return mp_obj_new_bytes(data, read_len);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_uart_read_obj, 0, 2, solaros_uart_read);

static mp_obj_t solaros_audio_status(void)
{
    solar_os_audio_status_t status;
    solar_os_audio_get_status(&status);
    return python_audio_status_to_dict(&status);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_audio_status_obj, solaros_audio_status);

static mp_obj_t solaros_audio_deinit(void)
{
    solar_os_audio_deinit();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_audio_deinit_obj, solaros_audio_deinit);

static mp_obj_t solaros_audio_set_volume(mp_obj_t volume_obj)
{
    python_check_esp(solar_os_audio_set_volume(python_u8_from_obj(volume_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_audio_set_volume_obj, solaros_audio_set_volume);

static mp_obj_t solaros_audio_set_mic_gain(mp_obj_t gain_obj)
{
    python_check_esp(solar_os_audio_set_mic_gain((float)mp_obj_get_float(gain_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_audio_set_mic_gain_obj, solaros_audio_set_mic_gain);

static mp_obj_t solaros_audio_tone(size_t n_args, const mp_obj_t *args)
{
    const uint32_t frequency_hz = python_optional_u32(n_args, args, 0, 0);
    const uint32_t duration_ms = python_optional_u32(n_args, args, 1, 0);
    const uint8_t volume = python_optional_u8(n_args,
                                              args,
                                              2,
                                              SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    python_check_esp(solar_os_audio_play_tone(frequency_hz, duration_ms, volume));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_audio_tone_obj, 2, 3, solaros_audio_tone);

static mp_obj_t solaros_audio_level(mp_obj_t duration_obj)
{
    solar_os_audio_level_t level;
    python_check_esp(solar_os_audio_measure_level(python_u32_from_obj(duration_obj), &level));

    mp_obj_t dict = mp_obj_new_dict(3);
    python_dict_store_uint(dict, "samples", level.samples);
    python_dict_store_int(dict, "peak_percent", level.peak_percent);
    python_dict_store_int(dict, "average_percent", level.average_percent);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_audio_level_obj, solaros_audio_level);

static mp_obj_t solaros_audio_loopback(size_t n_args, const mp_obj_t *args)
{
    const uint32_t duration_ms = python_optional_u32(n_args, args, 0, 0);
    const uint8_t volume = python_optional_u8(n_args,
                                              args,
                                              1,
                                              SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    python_check_esp(solar_os_audio_loopback(duration_ms, volume));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_audio_loopback_obj, 1, 2, solaros_audio_loopback);

static mp_obj_t solaros_audio_wav_info(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));

    solar_os_audio_wav_info_t info;
    python_check_esp(solar_os_audio_get_wav_info(path, &info));
    return python_wav_info_to_dict(&info);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_audio_wav_info_obj, solaros_audio_wav_info);

static mp_obj_t solaros_audio_record_wav(mp_obj_t path_obj, mp_obj_t duration_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));

    solar_os_audio_wav_info_t info;
    const solar_os_audio_wav_options_t options = {
        .should_cancel = python_should_cancel,
        .progress = NULL,
        .user = NULL,
        .progress_interval_ms = SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS,
    };
    python_check_esp(solar_os_audio_record_wav(path,
                                               python_u32_from_obj(duration_obj),
                                               &options,
                                               &info));
    return python_wav_info_to_dict(&info);
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_audio_record_wav_obj, solaros_audio_record_wav);

static mp_obj_t solaros_audio_play_wav(size_t n_args, const mp_obj_t *args)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[0], path, sizeof(path));
    const uint8_t volume = python_optional_u8(n_args,
                                              args,
                                              1,
                                              SOLAR_OS_AUDIO_VOLUME_GLOBAL);

    solar_os_audio_wav_info_t info;
    const solar_os_audio_wav_options_t options = {
        .should_cancel = python_should_cancel,
        .progress = NULL,
        .user = NULL,
        .progress_interval_ms = SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS,
    };
    python_check_esp(solar_os_audio_play_wav(path, volume, &options, &info));
    return python_wav_info_to_dict(&info);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_audio_play_wav_obj, 1, 2, solaros_audio_play_wav);

static mp_obj_t solaros_ble_status(void)
{
    char status[96];
    solar_os_ble_keyboard_get_status(status, sizeof(status));
    return mp_obj_new_str_from_cstr(status);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_status_obj, solaros_ble_status);

static mp_obj_t solaros_ble_connected(void)
{
    return mp_obj_new_bool(solar_os_ble_keyboard_is_connected());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_connected_obj, solaros_ble_connected);

static mp_obj_t solaros_ble_pair(void)
{
    python_check_esp(solar_os_ble_keyboard_start_pairing());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_pair_obj, solaros_ble_pair);

static mp_obj_t solaros_ble_forget(void)
{
    python_check_esp(solar_os_ble_keyboard_forget());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_forget_obj, solaros_ble_forget);

static mp_obj_t solaros_ble_layout(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) {
        return mp_obj_new_str_from_cstr(
            solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout()));
    }

    solar_os_ble_keyboard_layout_t layout;
    if (!solar_os_ble_keyboard_parse_layout(mp_obj_str_get_str(args[0]), &layout)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected us or de"));
    }
    python_check_esp(solar_os_ble_keyboard_set_layout(layout));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_layout_obj, 0, 1, solaros_ble_layout);

static mp_obj_t solaros_ble_read(size_t n_args, const mp_obj_t *args)
{
    uint32_t len = python_optional_u32(n_args, args, 0, 64);
    if (len == 0 || len > 256) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected length 1..256"));
    }

    char buffer[256];
    const size_t read_len = solar_os_ble_keyboard_read_chars(buffer, len);
    return mp_obj_new_bytes((const byte *)buffer, read_len);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_read_obj, 0, 1, solaros_ble_read);

static mp_obj_t solaros_clipboard_set(mp_obj_t data_obj)
{
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    python_check_esp(solar_os_clipboard_set(bufinfo.buf, bufinfo.len));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_clipboard_set_obj, solaros_clipboard_set);

static mp_obj_t solaros_clipboard_get(void)
{
    size_t len = 0;
    const char *data = solar_os_clipboard_data(&len);
    if (data == NULL) {
        return mp_obj_new_bytes((const byte *)"", 0);
    }
    return mp_obj_new_bytes((const byte *)data, len);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_clipboard_get_obj, solaros_clipboard_get);

static mp_obj_t solaros_clipboard_size(void)
{
    return mp_obj_new_int_from_uint(solar_os_clipboard_size());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_clipboard_size_obj, solaros_clipboard_size);

static mp_obj_t solaros_clipboard_clear(void)
{
    solar_os_clipboard_clear();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_clipboard_clear_obj, solaros_clipboard_clear);

static mp_obj_t solaros_identity_user(void)
{
    char buffer[SOLAR_OS_IDENTITY_USER_MAX + 1];
    solar_os_identity_get_user(buffer, sizeof(buffer));
    return mp_obj_new_str_from_cstr(buffer);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_identity_user_obj, solaros_identity_user);

static mp_obj_t solaros_identity_hostname(void)
{
    char buffer[SOLAR_OS_IDENTITY_HOSTNAME_MAX + 1];
    solar_os_identity_get_hostname(buffer, sizeof(buffer));
    return mp_obj_new_str_from_cstr(buffer);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_identity_hostname_obj, solaros_identity_hostname);

static mp_obj_t solaros_identity_format(void)
{
    char buffer[SOLAR_OS_IDENTITY_USER_MAX + SOLAR_OS_IDENTITY_HOSTNAME_MAX + 2];
    solar_os_identity_format(buffer, sizeof(buffer));
    return mp_obj_new_str_from_cstr(buffer);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_identity_format_obj, solaros_identity_format);

static mp_obj_t solaros_net_ping(size_t n_args, const mp_obj_t *args)
{
    const char *host = mp_obj_str_get_str(args[0]);
    solar_os_net_ping_options_t options = {
        .count = python_optional_u32(n_args, args, 1, 4),
        .timeout_ms = python_optional_u32(n_args, args, 2, 1000),
        .interval_ms = python_optional_u32(n_args, args, 3, 1000),
        .data_size = python_optional_u32(n_args, args, 4, 32),
    };
    solar_os_net_ping_result_t result;
    python_check_esp(solar_os_net_ping(host,
                                       &options,
                                       NULL,
                                       NULL,
                                       python_should_cancel,
                                       NULL,
                                       &result));

    mp_obj_t dict = mp_obj_new_dict(9);
    python_dict_store_cstr(dict, "resolved_ip", result.resolved_ip);
    python_dict_store_bool(dict, "interrupted", result.interrupted);
    python_dict_store_uint(dict, "transmitted", result.transmitted);
    python_dict_store_uint(dict, "received", result.received);
    python_dict_store_uint(dict, "loss_percent", result.loss_percent);
    python_dict_store_uint(dict, "total_time_ms", result.total_time_ms);
    python_dict_store_uint(dict, "min_time_ms", result.min_time_ms);
    python_dict_store_uint(dict, "avg_time_ms", result.avg_time_ms);
    python_dict_store_uint(dict, "max_time_ms", result.max_time_ms);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_net_ping_obj, 1, 5, solaros_net_ping);

static mp_obj_t solaros_ssh_keys_default_paths(void)
{
    char private_path[SOLAR_OS_STORAGE_PATH_MAX];
    char public_path[SOLAR_OS_STORAGE_PATH_MAX];
    python_check_esp(solar_os_ssh_keys_default_paths(private_path,
                                                     sizeof(private_path),
                                                     public_path,
                                                     sizeof(public_path)));

    mp_obj_t dict = mp_obj_new_dict(2);
    python_dict_store_cstr(dict, "private", private_path);
    python_dict_store_cstr(dict, "public", public_path);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ssh_keys_default_paths_obj, solaros_ssh_keys_default_paths);

static mp_obj_t solaros_ssh_keys_default_exists(void)
{
    return mp_obj_new_bool(solar_os_ssh_keys_default_exists());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ssh_keys_default_exists_obj, solaros_ssh_keys_default_exists);

static mp_obj_t solaros_ssh_keys_status(void)
{
    solar_os_ssh_key_status_t status;
    python_check_esp(solar_os_ssh_keys_get_status(&status));

    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_bool(dict, "private_key_exists", status.private_key_exists);
    python_dict_store_bool(dict, "public_key_exists", status.public_key_exists);
    python_dict_store_uint(dict, "private_key_size", status.private_key_size);
    python_dict_store_uint(dict, "public_key_size", status.public_key_size);
    python_dict_store_cstr(dict, "private_key_path", status.private_key_path);
    python_dict_store_cstr(dict, "public_key_path", status.public_key_path);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ssh_keys_status_obj, solaros_ssh_keys_status);

static mp_obj_t solaros_ssh_keys_generate(size_t n_args, const mp_obj_t *args)
{
    const uint32_t bits = python_optional_u32(n_args,
                                              args,
                                              0,
                                              SOLAR_OS_SSH_KEY_DEFAULT_BITS);
    const bool overwrite = n_args >= 2 ? mp_obj_is_true(args[1]) : false;
    python_check_esp(solar_os_ssh_keys_generate_rsa(bits, overwrite));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ssh_keys_generate_obj,
                                    0,
                                    2,
                                    solaros_ssh_keys_generate);

static mp_obj_t solaros_ssh_keys_remove(void)
{
    python_check_esp(solar_os_ssh_keys_remove_default());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ssh_keys_remove_obj, solaros_ssh_keys_remove);

static mp_obj_t python_job_status_to_dict(const solar_os_job_status_t *status)
{
    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_cstr(dict, "name", status->name);
    python_dict_store_cstr(dict, "summary", status->summary);
    python_dict_store_cstr(dict, "state", solar_os_job_state_name(status->state));
    python_dict_store_int(dict, "last_error", status->last_error);
    python_dict_store_cstr(dict, "last_error_name", esp_err_to_name(status->last_error));
    python_dict_store_uint(dict, "tick_count", status->tick_count);
    python_dict_store_uint(dict, "last_tick_ms", status->last_tick_ms);
    return dict;
}

static mp_obj_t solaros_jobs_list(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    const size_t count = solar_os_jobs_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_job_status_t status;
        if (solar_os_jobs_get(i, &status)) {
            mp_obj_list_append(list, python_job_status_to_dict(&status));
        }
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_jobs_list_obj, solaros_jobs_list);

static mp_obj_t solaros_jobs_count(void)
{
    return mp_obj_new_int_from_uint(solar_os_jobs_count());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_jobs_count_obj, solaros_jobs_count);

static mp_obj_t solaros_jobs_status(mp_obj_t name_obj)
{
    solar_os_job_status_t status;
    if (!solar_os_jobs_get_by_name(mp_obj_str_get_str(name_obj), &status)) {
        python_raise_esp(ESP_ERR_NOT_FOUND);
    }
    return python_job_status_to_dict(&status);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jobs_status_obj, solaros_jobs_status);

static mp_obj_t solaros_jobs_start(size_t n_args, const mp_obj_t *args)
{
    if (python_app.ctx == NULL) {
        python_raise_esp(ESP_ERR_INVALID_STATE);
    }

    const char *name = mp_obj_str_get_str(args[0]);
    int argc = 0;
    char arg_storage[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char *argv[SOLAR_OS_APP_ARG_MAX];

    if (n_args >= 2 && args[1] != mp_const_none) {
        size_t item_count = 0;
        mp_obj_t *items = NULL;
        mp_obj_get_array(args[1], &item_count, &items);
        if (item_count > SOLAR_OS_APP_ARG_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("too many job arguments"));
        }
        argc = (int)item_count;
        for (size_t i = 0; i < item_count; i++) {
            const char *arg = mp_obj_str_get_str(items[i]);
            if (strlen(arg) >= SOLAR_OS_APP_ARG_LEN) {
                mp_raise_ValueError(MP_ERROR_TEXT("job argument too long"));
            }
            strlcpy(arg_storage[i], arg, sizeof(arg_storage[i]));
            argv[i] = arg_storage[i];
        }
    }

    python_check_esp(solar_os_jobs_start(python_app.ctx, name, argc, argv));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_jobs_start_obj, 1, 2, solaros_jobs_start);

static mp_obj_t solaros_jobs_stop(mp_obj_t name_obj)
{
    if (python_app.ctx == NULL) {
        python_raise_esp(ESP_ERR_INVALID_STATE);
    }
    python_check_esp(solar_os_jobs_stop(python_app.ctx, mp_obj_str_get_str(name_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jobs_stop_obj, solaros_jobs_stop);

static mp_obj_t solaros_apps_list(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    const size_t count = solar_os_app_registry_count();
    for (size_t i = 0; i < count; i++) {
        const solar_os_app_registry_entry_t *entry = solar_os_app_registry_get(i);
        if (entry == NULL) {
            continue;
        }

        mp_obj_t dict = mp_obj_new_dict(2);
        python_dict_store_cstr(dict, "name", entry->name);
        python_dict_store_cstr(dict, "summary", entry->summary);
        mp_obj_list_append(list, dict);
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_apps_list_obj, solaros_apps_list);

static mp_obj_t solaros_apps_find(mp_obj_t name_obj)
{
    const solar_os_app_registry_entry_t *entry =
        solar_os_app_registry_find(mp_obj_str_get_str(name_obj));
    if (entry == NULL) {
        return mp_const_none;
    }

    mp_obj_t dict = mp_obj_new_dict(2);
    python_dict_store_cstr(dict, "name", entry->name);
    python_dict_store_cstr(dict, "summary", entry->summary);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_apps_find_obj, solaros_apps_find);

static solar_os_terminal_t *python_current_terminal(void)
{
    if (python_app.ctx == NULL) {
        return NULL;
    }
    return python_display_terminal(python_app.ctx);
}

static solar_os_gfx_t *python_current_gfx(void)
{
    if (python_app.ctx == NULL) {
        return NULL;
    }
    return solar_os_context_gfx(python_app.ctx);
}

static void python_ui_send_event(const python_event_t *event)
{
    if (!python_send_event(event)) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ui event queue stopped"));
    }
}

static void python_tui_send_event(const python_event_t *event)
{
    python_ui_send_event(event);
}

static void python_tui_send_simple(python_event_type_t type)
{
    const python_event_t event = {
        .type = type,
    };
    python_tui_send_event(&event);
}

static void python_tui_send_write(const char *text, size_t len, uint8_t attr)
{
    while (len > 0) {
        python_event_t event = {
            .type = PYTHON_EVENT_TUI_WRITE,
            .attr = attr,
        };
        event.data_len = len > sizeof(event.data) - 1 ? sizeof(event.data) - 1 : len;
        memcpy(event.data, text, event.data_len);
        event.data[event.data_len] = '\0';
        python_tui_send_event(&event);
        text += event.data_len;
        len -= event.data_len;
    }
}

static mp_obj_t solaros_tui_rows(void)
{
    solar_os_shell_io_t *io = python_current_io();
    return mp_obj_new_int_from_uint(io != NULL ? solar_os_shell_io_rows(io) : 0);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_tui_rows_obj, solaros_tui_rows);

static mp_obj_t solaros_tui_cols(void)
{
    solar_os_shell_io_t *io = python_current_io();
    return mp_obj_new_int_from_uint(io != NULL ? solar_os_shell_io_cols(io) : 0);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_tui_cols_obj, solaros_tui_cols);

static mp_obj_t solaros_tui_size(void)
{
    solar_os_shell_io_t *io = python_current_io();
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(io != NULL ? solar_os_shell_io_rows(io) : 0),
        mp_obj_new_int_from_uint(io != NULL ? solar_os_shell_io_cols(io) : 0),
    };
    return mp_obj_new_tuple(2, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_tui_size_obj, solaros_tui_size);

static mp_obj_t solaros_tui_clear(void)
{
    python_tui_send_simple(PYTHON_EVENT_TUI_CLEAR);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_tui_clear_obj, solaros_tui_clear);

static mp_obj_t solaros_tui_refresh(void)
{
    python_tui_send_simple(PYTHON_EVENT_TUI_REFRESH);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_tui_refresh_obj, solaros_tui_refresh);

static mp_obj_t solaros_tui_move(mp_obj_t row_obj, mp_obj_t col_obj)
{
    const python_event_t event = {
        .type = PYTHON_EVENT_TUI_MOVE,
        .row = python_u16_from_size(python_size_from_obj(row_obj)),
        .col = python_u16_from_size(python_size_from_obj(col_obj)),
    };
    python_tui_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_tui_move_obj, solaros_tui_move);

static mp_obj_t solaros_tui_write(size_t n_args, const mp_obj_t *args)
{
    size_t len = 0;
    const char *text = mp_obj_str_get_data(args[0], &len);
    const uint8_t attr = python_optional_tui_attr(n_args, args, 1);
    python_tui_send_write(text, len, attr);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_write_obj, 1, 2, solaros_tui_write);

static mp_obj_t solaros_tui_addstr(size_t n_args, const mp_obj_t *args)
{
    if (n_args < 3 || n_args > 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected row,col,text[,attr]"));
    }

    solaros_tui_move(args[0], args[1]);

    size_t len = 0;
    const char *text = mp_obj_str_get_data(args[2], &len);
    const uint8_t attr = python_optional_tui_attr(n_args, args, 3);
    python_tui_send_write(text, len, attr);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_addstr_obj, 3, 4, solaros_tui_addstr);

static mp_obj_t solaros_tui_putch(size_t n_args, const mp_obj_t *args)
{
    if (n_args < 3 || n_args > 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected row,col,ch[,attr]"));
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_TUI_PUTCH,
        .row = python_u16_from_size(python_size_from_obj(args[0])),
        .col = python_u16_from_size(python_size_from_obj(args[1])),
        .codepoint = python_codepoint_from_obj(args[2]),
        .attr = python_optional_tui_attr(n_args, args, 3),
    };
    python_tui_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_putch_obj, 3, 4, solaros_tui_putch);

static mp_obj_t solaros_tui_hline(size_t n_args, const mp_obj_t *args)
{
    if (n_args < 3 || n_args > 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected row,col,width[,attr]"));
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_TUI_HLINE,
        .row = python_u16_from_size(python_size_from_obj(args[0])),
        .col = python_u16_from_size(python_size_from_obj(args[1])),
        .width = python_u16_from_size(python_size_from_obj(args[2])),
        .attr = python_optional_tui_attr(n_args, args, 3),
    };
    python_tui_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_hline_obj, 3, 4, solaros_tui_hline);

static mp_obj_t solaros_tui_vline(size_t n_args, const mp_obj_t *args)
{
    if (n_args < 3 || n_args > 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected row,col,height[,attr]"));
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_TUI_VLINE,
        .row = python_u16_from_size(python_size_from_obj(args[0])),
        .col = python_u16_from_size(python_size_from_obj(args[1])),
        .height = python_u16_from_size(python_size_from_obj(args[2])),
        .attr = python_optional_tui_attr(n_args, args, 3),
    };
    python_tui_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_vline_obj, 3, 4, solaros_tui_vline);

static mp_obj_t solaros_tui_vrule(size_t n_args, const mp_obj_t *args)
{
    if (n_args < 3 || n_args > 5) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected row,col,height[,width[,attr]]"));
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_TUI_VRULE,
        .row = python_u16_from_size(python_size_from_obj(args[0])),
        .col = python_u16_from_size(python_size_from_obj(args[1])),
        .height = python_u16_from_size(python_size_from_obj(args[2])),
        .width = n_args >= 4 ? python_u16_from_size(python_size_from_obj(args[3])) : 1,
        .attr = python_optional_tui_attr(n_args, args, 4),
    };
    python_tui_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_vrule_obj, 3, 5, solaros_tui_vrule);

static mp_obj_t solaros_tui_box(size_t n_args, const mp_obj_t *args)
{
    if (n_args < 4 || n_args > 5) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected row,col,height,width[,attr]"));
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_TUI_BOX,
        .row = python_u16_from_size(python_size_from_obj(args[0])),
        .col = python_u16_from_size(python_size_from_obj(args[1])),
        .height = python_u16_from_size(python_size_from_obj(args[2])),
        .width = python_u16_from_size(python_size_from_obj(args[3])),
        .attr = python_optional_tui_attr(n_args, args, 4),
    };
    python_tui_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_box_obj, 4, 5, solaros_tui_box);

static mp_obj_t solaros_tui_fill(size_t n_args, const mp_obj_t *args)
{
    if (n_args < 4 || n_args > 6) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected row,col,height,width[,ch[,attr]]"));
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_TUI_FILL,
        .row = python_u16_from_size(python_size_from_obj(args[0])),
        .col = python_u16_from_size(python_size_from_obj(args[1])),
        .height = python_u16_from_size(python_size_from_obj(args[2])),
        .width = python_u16_from_size(python_size_from_obj(args[3])),
        .codepoint = n_args >= 5 ? python_codepoint_from_obj(args[4]) : ' ',
        .attr = python_optional_tui_attr(n_args, args, 5),
    };
    python_tui_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_fill_obj, 4, 6, solaros_tui_fill);

static mp_obj_t solaros_tui_getch(size_t n_args, const mp_obj_t *args)
{
    const uint32_t timeout_ms = python_optional_u32(n_args, args, 0, 0);
    if (python_app.key_input == NULL) {
        return mp_const_none;
    }

    char ch = 0;
    if (timeout_ms == 0) {
        if (xQueueReceive(python_app.key_input, &ch, 0) == pdPASS) {
            return mp_obj_new_int_from_uint((uint8_t)ch);
        }
        return mp_const_none;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (!python_app.stop_requested &&
           (xTaskGetTickCount() - start) <= timeout_ticks) {
        if (xQueueReceive(python_app.key_input, &ch, pdMS_TO_TICKS(20)) == pdPASS) {
            return mp_obj_new_int_from_uint((uint8_t)ch);
        }
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_getch_obj, 0, 1, solaros_tui_getch);

static void python_gfx_send_event(const python_event_t *event)
{
    python_ui_send_event(event);
}

static void python_gfx_send_simple(python_event_type_t type)
{
    const python_event_t event = {
        .type = type,
    };
    python_gfx_send_event(&event);
}

static void python_gfx_send_text(int32_t x, int32_t y, const char *text, size_t len)
{
    if (len >= PYTHON_EVENT_DATA_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("text too long"));
    }

    python_event_t event = {
        .type = PYTHON_EVENT_GFX_TEXT,
        .x0 = x,
        .y0 = y,
        .data_len = len,
    };
    memcpy(event.data, text, len);
    event.data[len] = '\0';
    python_gfx_send_event(&event);
}

static mp_obj_t solaros_gfx_begin(void)
{
    python_gfx_send_simple(PYTHON_EVENT_GFX_BEGIN);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_gfx_begin_obj, solaros_gfx_begin);

static mp_obj_t solaros_gfx_end(void)
{
    python_gfx_send_simple(PYTHON_EVENT_GFX_END);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_gfx_end_obj, solaros_gfx_end);

static mp_obj_t solaros_gfx_width(void)
{
    solar_os_gfx_t *gfx = python_current_gfx();
    return mp_obj_new_int_from_uint(gfx != NULL ? solar_os_gfx_width(gfx) : 0);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_gfx_width_obj, solaros_gfx_width);

static mp_obj_t solaros_gfx_height(void)
{
    solar_os_gfx_t *gfx = python_current_gfx();
    return mp_obj_new_int_from_uint(gfx != NULL ? solar_os_gfx_height(gfx) : 0);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_gfx_height_obj, solaros_gfx_height);

static mp_obj_t solaros_gfx_size(void)
{
    solar_os_gfx_t *gfx = python_current_gfx();
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(gfx != NULL ? solar_os_gfx_width(gfx) : 0),
        mp_obj_new_int_from_uint(gfx != NULL ? solar_os_gfx_height(gfx) : 0),
    };
    return mp_obj_new_tuple(2, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_gfx_size_obj, solaros_gfx_size);

static mp_obj_t solaros_gfx_clear(size_t n_args, const mp_obj_t *args)
{
    const solar_os_gfx_color_t color =
        n_args >= 1 ? python_gfx_color_from_obj(args[0]) : SOLAR_OS_GFX_COLOR_WHITE;
    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_CLEAR,
        .attr = (uint8_t)color,
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_clear_obj, 0, 1, solaros_gfx_clear);

static mp_obj_t solaros_gfx_color(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) {
        solar_os_gfx_t *gfx = python_current_gfx();
        return mp_obj_new_int(gfx != NULL ? solar_os_gfx_color(gfx) : SOLAR_OS_GFX_COLOR_BLACK);
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_COLOR,
        .attr = (uint8_t)python_gfx_color_from_obj(args[0]),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_color_obj, 0, 1, solaros_gfx_color);

static mp_obj_t solaros_gfx_gray(mp_obj_t level_obj)
{
    mp_int_t level = mp_obj_get_int(level_obj);
    if (level < 0) {
        level = 0;
    } else if (level > SOLAR_OS_GFX_GRAY_MAX) {
        level = SOLAR_OS_GFX_GRAY_MAX;
    }

    return mp_obj_new_int(solar_os_gfx_gray((uint8_t)level));
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_gfx_gray_obj, solaros_gfx_gray);

static mp_obj_t solaros_gfx_font(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) {
        solar_os_gfx_t *gfx = python_current_gfx();
        return mp_obj_new_int(gfx != NULL ? solar_os_gfx_font(gfx) : SOLAR_OS_GFX_FONT_MONO);
    }

    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_FONT,
        .attr = (uint8_t)python_gfx_font_from_obj(args[0]),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_font_obj, 0, 1, solaros_gfx_font);

static mp_obj_t solaros_gfx_present(void)
{
    python_gfx_send_simple(PYTHON_EVENT_GFX_PRESENT);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_gfx_present_obj, solaros_gfx_present);

static mp_obj_t solaros_gfx_pixel(mp_obj_t x_obj, mp_obj_t y_obj)
{
    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_PIXEL,
        .x0 = python_i32_from_obj(x_obj),
        .y0 = python_i32_from_obj(y_obj),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_gfx_pixel_obj, solaros_gfx_pixel);

static mp_obj_t solaros_gfx_line(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_LINE,
        .x0 = python_i32_from_obj(args[0]),
        .y0 = python_i32_from_obj(args[1]),
        .x1 = python_i32_from_obj(args[2]),
        .y1 = python_i32_from_obj(args[3]),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_line_obj, 4, 4, solaros_gfx_line);

static mp_obj_t solaros_gfx_rect(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_RECT,
        .x0 = python_i32_from_obj(args[0]),
        .y0 = python_i32_from_obj(args[1]),
        .width = python_u16_from_size(python_size_from_obj(args[2])),
        .height = python_u16_from_size(python_size_from_obj(args[3])),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_rect_obj, 4, 4, solaros_gfx_rect);

static mp_obj_t solaros_gfx_fill_rect(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_FILL_RECT,
        .x0 = python_i32_from_obj(args[0]),
        .y0 = python_i32_from_obj(args[1]),
        .width = python_u16_from_size(python_size_from_obj(args[2])),
        .height = python_u16_from_size(python_size_from_obj(args[3])),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_fill_rect_obj, 4, 4, solaros_gfx_fill_rect);

static mp_obj_t solaros_gfx_circle(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_CIRCLE,
        .x0 = python_i32_from_obj(args[0]),
        .y0 = python_i32_from_obj(args[1]),
        .width = python_u16_from_size(python_size_from_obj(args[2])),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_circle_obj, 3, 3, solaros_gfx_circle);

static mp_obj_t solaros_gfx_fill_circle(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    const python_event_t event = {
        .type = PYTHON_EVENT_GFX_FILL_CIRCLE,
        .x0 = python_i32_from_obj(args[0]),
        .y0 = python_i32_from_obj(args[1]),
        .width = python_u16_from_size(python_size_from_obj(args[2])),
    };
    python_gfx_send_event(&event);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_fill_circle_obj, 3, 3, solaros_gfx_fill_circle);

static mp_obj_t solaros_gfx_text(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    size_t len = 0;
    const char *text = mp_obj_str_get_data(args[2], &len);
    python_gfx_send_text(python_i32_from_obj(args[0]), python_i32_from_obj(args[1]), text, len);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_gfx_text_obj, 3, 3, solaros_gfx_text);

static void python_module_store(mp_obj_t module, const char *name, mp_obj_t value)
{
    mp_store_attr(module, qstr_from_str(name), value);
}

static void python_register_solaros_module(void)
{
    mp_obj_t module = mp_obj_new_module(qstr_from_str("solaros"));
    python_module_store(module, "write", MP_OBJ_FROM_PTR(&solaros_write_obj));
    python_module_store(module, "version", MP_OBJ_FROM_PTR(&solaros_version_obj));
    python_module_store(module, "should_exit", MP_OBJ_FROM_PTR(&solaros_should_exit_obj));
    python_module_store(module, "battery_status", MP_OBJ_FROM_PTR(&solaros_battery_obj));
    python_module_store(module, "wifi_status", MP_OBJ_FROM_PTR(&solaros_wifi_obj));
    python_module_store(module, "environment", MP_OBJ_FROM_PTR(&solaros_environment_obj));

    mp_obj_t storage = python_new_submodule(module, "storage");
    python_module_store(storage, "status", MP_OBJ_FROM_PTR(&solaros_storage_status_obj));
    python_module_store(storage, "is_mounted", MP_OBJ_FROM_PTR(&solaros_storage_is_mounted_obj));
    python_module_store(storage, "mount", MP_OBJ_FROM_PTR(&solaros_storage_mount_obj));
    python_module_store(storage, "unmount", MP_OBJ_FROM_PTR(&solaros_storage_unmount_obj));
    python_module_store(storage, "mount_point", MP_OBJ_FROM_PTR(&solaros_storage_mount_point_obj));
    python_module_store(storage, "usage", MP_OBJ_FROM_PTR(&solaros_storage_usage_obj));
    python_module_store(storage, "resolve", MP_OBJ_FROM_PTR(&solaros_storage_resolve_obj));
    python_module_store(storage, "rescan", MP_OBJ_FROM_PTR(&solaros_storage_rescan_obj));
    python_module_store(storage, "blocks", MP_OBJ_FROM_PTR(&solaros_storage_blocks_obj));
    python_module_store(storage, "block_count", MP_OBJ_FROM_PTR(&solaros_storage_block_count_obj));
    python_module_store(storage, "block", MP_OBJ_FROM_PTR(&solaros_storage_block_obj));
    python_module_store(storage,
                        "usage_for_block",
                        MP_OBJ_FROM_PTR(&solaros_storage_usage_for_block_obj));
    python_module_store(storage, "mkdir", MP_OBJ_FROM_PTR(&solaros_storage_mkdir_obj));
    python_module_store(storage, "rmdir", MP_OBJ_FROM_PTR(&solaros_storage_rmdir_obj));
    python_module_store(storage, "remove", MP_OBJ_FROM_PTR(&solaros_storage_remove_obj));
    python_module_store(storage, "rename", MP_OBJ_FROM_PTR(&solaros_storage_rename_obj));
    python_module_store(storage, "copy", MP_OBJ_FROM_PTR(&solaros_storage_copy_obj));
    python_module_store(storage, "mount_volume", MP_OBJ_FROM_PTR(&solaros_storage_mount_volume_obj));
    python_module_store(storage,
                        "unmount_volume",
                        MP_OBJ_FROM_PTR(&solaros_storage_unmount_volume_obj));

    mp_obj_t time = python_new_submodule(module, "time");
    python_module_store(time, "uptime_ms", MP_OBJ_FROM_PTR(&solaros_time_uptime_ms_obj));
    python_module_store(time, "uptime", MP_OBJ_FROM_PTR(&solaros_time_uptime_obj));
    python_module_store(time, "datetime", MP_OBJ_FROM_PTR(&solaros_time_datetime_obj));
    python_module_store(time, "utc_datetime", MP_OBJ_FROM_PTR(&solaros_time_utc_datetime_obj));
    python_module_store(time, "set_datetime", MP_OBJ_FROM_PTR(&solaros_time_set_datetime_obj));
    python_module_store(time,
                        "set_utc_datetime",
                        MP_OBJ_FROM_PTR(&solaros_time_set_utc_datetime_obj));
    python_module_store(time, "utc_to_local", MP_OBJ_FROM_PTR(&solaros_time_utc_to_local_obj));
    python_module_store(time, "local_to_utc", MP_OBJ_FROM_PTR(&solaros_time_local_to_utc_obj));
    python_module_store(time, "is_valid", MP_OBJ_FROM_PTR(&solaros_time_is_valid_obj));
    python_module_store(time, "timezone", MP_OBJ_FROM_PTR(&solaros_time_timezone_obj));
    python_module_store(time, "set_timezone", MP_OBJ_FROM_PTR(&solaros_time_set_timezone_obj));
    python_module_store(time, "ntp_sync", MP_OBJ_FROM_PTR(&solaros_time_ntp_sync_obj));

    mp_obj_t battery = python_new_submodule(module, "battery");
    python_module_store(battery, "status", MP_OBJ_FROM_PTR(&solaros_battery_status_obj));

    mp_obj_t sensors = python_new_submodule(module, "sensors");
    python_module_store(sensors, "environment", MP_OBJ_FROM_PTR(&solaros_sensors_environment_obj));

    mp_obj_t wifi = python_new_submodule(module, "wifi");
    python_module_store(wifi, "status", MP_OBJ_FROM_PTR(&solaros_wifi_status_obj));
    python_module_store(wifi, "status_text", MP_OBJ_FROM_PTR(&solaros_wifi_status_text_obj));
    python_module_store(wifi, "start", MP_OBJ_FROM_PTR(&solaros_wifi_start_obj));
    python_module_store(wifi, "stop", MP_OBJ_FROM_PTR(&solaros_wifi_stop_obj));
    python_module_store(wifi, "connect", MP_OBJ_FROM_PTR(&solaros_wifi_connect_obj));
    python_module_store(wifi, "connect_saved", MP_OBJ_FROM_PTR(&solaros_wifi_connect_saved_obj));
    python_module_store(wifi, "disconnect", MP_OBJ_FROM_PTR(&solaros_wifi_disconnect_obj));
    python_module_store(wifi, "forget", MP_OBJ_FROM_PTR(&solaros_wifi_forget_obj));
    python_module_store(wifi, "forget_ssid", MP_OBJ_FROM_PTR(&solaros_wifi_forget_ssid_obj));
    python_module_store(wifi, "forget_all", MP_OBJ_FROM_PTR(&solaros_wifi_forget_all_obj));
    python_module_store(wifi, "known", MP_OBJ_FROM_PTR(&solaros_wifi_known_obj));
    python_module_store(wifi, "scan", MP_OBJ_FROM_PTR(&solaros_wifi_scan_obj));
    python_module_store(wifi, "ap_start", MP_OBJ_FROM_PTR(&solaros_wifi_ap_start_obj));
    python_module_store(wifi, "ap_stop", MP_OBJ_FROM_PTR(&solaros_wifi_ap_stop_obj));
    python_module_store(wifi, "nat", MP_OBJ_FROM_PTR(&solaros_wifi_nat_obj));

    mp_obj_t mqtt = python_new_submodule(module, "mqtt");
    python_module_store(mqtt, "status", MP_OBJ_FROM_PTR(&solaros_mqtt_status_obj));
    python_module_store(mqtt, "connect", MP_OBJ_FROM_PTR(&solaros_mqtt_connect_obj));
    python_module_store(mqtt, "disconnect", MP_OBJ_FROM_PTR(&solaros_mqtt_disconnect_obj));
    python_module_store(mqtt, "publish", MP_OBJ_FROM_PTR(&solaros_mqtt_publish_obj));
    python_module_store(mqtt, "subscribe", MP_OBJ_FROM_PTR(&solaros_mqtt_subscribe_obj));
    python_module_store(mqtt, "read", MP_OBJ_FROM_PTR(&solaros_mqtt_read_obj));

    mp_obj_t gpio = python_new_submodule(module, "gpio");
    python_module_store(gpio, "INPUT", mp_obj_new_int(SOLAR_OS_GPIO_MODE_INPUT));
    python_module_store(gpio, "OUTPUT", mp_obj_new_int(SOLAR_OS_GPIO_MODE_OUTPUT));
    python_module_store(gpio, "PULL_NONE", mp_obj_new_int(SOLAR_OS_GPIO_PULL_NONE));
    python_module_store(gpio, "PULL_UP", mp_obj_new_int(SOLAR_OS_GPIO_PULL_UP));
    python_module_store(gpio, "PULL_DOWN", mp_obj_new_int(SOLAR_OS_GPIO_PULL_DOWN));
    python_module_store(gpio, "pins", MP_OBJ_FROM_PTR(&solaros_gpio_pins_obj));
    python_module_store(gpio, "allowed", MP_OBJ_FROM_PTR(&solaros_gpio_allowed_obj));
    python_module_store(gpio, "mode", MP_OBJ_FROM_PTR(&solaros_gpio_mode_obj));
    python_module_store(gpio, "configure", MP_OBJ_FROM_PTR(&solaros_gpio_mode_obj));
    python_module_store(gpio, "read", MP_OBJ_FROM_PTR(&solaros_gpio_read_obj));
    python_module_store(gpio, "write", MP_OBJ_FROM_PTR(&solaros_gpio_write_obj));

    mp_obj_t led = python_new_submodule(module, "led");
    python_module_store(led, "status", MP_OBJ_FROM_PTR(&solaros_led_status_obj));
    python_module_store(led, "set", MP_OBJ_FROM_PTR(&solaros_led_set_obj));
    python_module_store(led, "on", MP_OBJ_FROM_PTR(&solaros_led_on_obj));
    python_module_store(led, "off", MP_OBJ_FROM_PTR(&solaros_led_off_obj));
    python_module_store(led, "toggle", MP_OBJ_FROM_PTR(&solaros_led_toggle_obj));

    mp_obj_t adc = python_new_submodule(module, "adc");
    python_module_store(adc, "pins", MP_OBJ_FROM_PTR(&solaros_adc_pins_obj));
    python_module_store(adc, "read", MP_OBJ_FROM_PTR(&solaros_adc_read_obj));

    mp_obj_t pwm = python_new_submodule(module, "pwm");
    python_module_store(pwm, "FREQ_MIN", mp_obj_new_int(SOLAR_OS_PWM_FREQ_MIN_HZ));
    python_module_store(pwm, "FREQ_MAX", mp_obj_new_int(SOLAR_OS_PWM_FREQ_MAX_HZ));
    python_module_store(pwm, "status", MP_OBJ_FROM_PTR(&solaros_pwm_status_obj));
    python_module_store(pwm, "set", MP_OBJ_FROM_PTR(&solaros_pwm_set_obj));
    python_module_store(pwm, "off", MP_OBJ_FROM_PTR(&solaros_pwm_off_obj));

    mp_obj_t i2c = python_new_submodule(module, "i2c");
    python_module_store(i2c, "info", MP_OBJ_FROM_PTR(&solaros_i2c_info_obj));
    python_module_store(i2c, "probe", MP_OBJ_FROM_PTR(&solaros_i2c_probe_obj));
    python_module_store(i2c, "scan", MP_OBJ_FROM_PTR(&solaros_i2c_scan_obj));
    python_module_store(i2c, "read_reg", MP_OBJ_FROM_PTR(&solaros_i2c_read_reg_obj));
    python_module_store(i2c, "write_reg", MP_OBJ_FROM_PTR(&solaros_i2c_write_reg_obj));

    mp_obj_t uart = python_new_submodule(module, "uart");
    python_module_store(uart, "status", MP_OBJ_FROM_PTR(&solaros_uart_status_obj));
    python_module_store(uart, "baud", MP_OBJ_FROM_PTR(&solaros_uart_baud_obj));
    python_module_store(uart, "is_valid_baud", MP_OBJ_FROM_PTR(&solaros_uart_is_valid_baud_obj));
    python_module_store(uart, "mode", MP_OBJ_FROM_PTR(&solaros_uart_mode_obj));
    python_module_store(uart, "write", MP_OBJ_FROM_PTR(&solaros_uart_write_obj));
    python_module_store(uart, "read", MP_OBJ_FROM_PTR(&solaros_uart_read_obj));

    mp_obj_t audio = python_new_submodule(module, "audio");
    python_module_store(audio, "status", MP_OBJ_FROM_PTR(&solaros_audio_status_obj));
    python_module_store(audio, "deinit", MP_OBJ_FROM_PTR(&solaros_audio_deinit_obj));
    python_module_store(audio, "off", MP_OBJ_FROM_PTR(&solaros_audio_deinit_obj));
    python_module_store(audio, "set_volume", MP_OBJ_FROM_PTR(&solaros_audio_set_volume_obj));
    python_module_store(audio, "set_mic_gain", MP_OBJ_FROM_PTR(&solaros_audio_set_mic_gain_obj));
    python_module_store(audio, "tone", MP_OBJ_FROM_PTR(&solaros_audio_tone_obj));
    python_module_store(audio, "level", MP_OBJ_FROM_PTR(&solaros_audio_level_obj));
    python_module_store(audio, "loopback", MP_OBJ_FROM_PTR(&solaros_audio_loopback_obj));
    python_module_store(audio, "wav_info", MP_OBJ_FROM_PTR(&solaros_audio_wav_info_obj));
    python_module_store(audio, "record_wav", MP_OBJ_FROM_PTR(&solaros_audio_record_wav_obj));
    python_module_store(audio, "play_wav", MP_OBJ_FROM_PTR(&solaros_audio_play_wav_obj));

    mp_obj_t ble = python_new_submodule(module, "ble");
    python_module_store(ble, "status", MP_OBJ_FROM_PTR(&solaros_ble_status_obj));
    python_module_store(ble, "connected", MP_OBJ_FROM_PTR(&solaros_ble_connected_obj));
    python_module_store(ble, "pair", MP_OBJ_FROM_PTR(&solaros_ble_pair_obj));
    python_module_store(ble, "forget", MP_OBJ_FROM_PTR(&solaros_ble_forget_obj));
    python_module_store(ble, "layout", MP_OBJ_FROM_PTR(&solaros_ble_layout_obj));
    python_module_store(ble, "read", MP_OBJ_FROM_PTR(&solaros_ble_read_obj));

    mp_obj_t clipboard = python_new_submodule(module, "clipboard");
    python_module_store(clipboard, "set", MP_OBJ_FROM_PTR(&solaros_clipboard_set_obj));
    python_module_store(clipboard, "get", MP_OBJ_FROM_PTR(&solaros_clipboard_get_obj));
    python_module_store(clipboard, "size", MP_OBJ_FROM_PTR(&solaros_clipboard_size_obj));
    python_module_store(clipboard, "clear", MP_OBJ_FROM_PTR(&solaros_clipboard_clear_obj));

    mp_obj_t identity = python_new_submodule(module, "identity");
    python_module_store(identity, "user", MP_OBJ_FROM_PTR(&solaros_identity_user_obj));
    python_module_store(identity, "hostname", MP_OBJ_FROM_PTR(&solaros_identity_hostname_obj));
    python_module_store(identity, "format", MP_OBJ_FROM_PTR(&solaros_identity_format_obj));

    mp_obj_t net = python_new_submodule(module, "net");
    python_module_store(net, "ping", MP_OBJ_FROM_PTR(&solaros_net_ping_obj));

    mp_obj_t ssh_keys = python_new_submodule(module, "ssh_keys");
    python_module_store(ssh_keys,
                        "default_paths",
                        MP_OBJ_FROM_PTR(&solaros_ssh_keys_default_paths_obj));
    python_module_store(ssh_keys,
                        "default_exists",
                        MP_OBJ_FROM_PTR(&solaros_ssh_keys_default_exists_obj));
    python_module_store(ssh_keys, "status", MP_OBJ_FROM_PTR(&solaros_ssh_keys_status_obj));
    python_module_store(ssh_keys, "generate", MP_OBJ_FROM_PTR(&solaros_ssh_keys_generate_obj));
    python_module_store(ssh_keys, "remove", MP_OBJ_FROM_PTR(&solaros_ssh_keys_remove_obj));

    mp_obj_t jobs = python_new_submodule(module, "jobs");
    python_module_store(jobs, "list", MP_OBJ_FROM_PTR(&solaros_jobs_list_obj));
    python_module_store(jobs, "count", MP_OBJ_FROM_PTR(&solaros_jobs_count_obj));
    python_module_store(jobs, "status", MP_OBJ_FROM_PTR(&solaros_jobs_status_obj));
    python_module_store(jobs, "start", MP_OBJ_FROM_PTR(&solaros_jobs_start_obj));
    python_module_store(jobs, "stop", MP_OBJ_FROM_PTR(&solaros_jobs_stop_obj));

    mp_obj_t apps = python_new_submodule(module, "apps");
    python_module_store(apps, "list", MP_OBJ_FROM_PTR(&solaros_apps_list_obj));
    python_module_store(apps, "find", MP_OBJ_FROM_PTR(&solaros_apps_find_obj));

    mp_obj_t tui = python_new_submodule(module, "tui");
    python_module_store(tui, "NORMAL", mp_obj_new_int(SOLAR_OS_TUI_ATTR_NORMAL));
    python_module_store(tui, "BOLD", mp_obj_new_int(SOLAR_OS_TUI_ATTR_BOLD));
    python_module_store(tui, "INVERSE", mp_obj_new_int(SOLAR_OS_TUI_ATTR_INVERSE));
    python_module_store(tui, "ITALIC", mp_obj_new_int(SOLAR_OS_TUI_ATTR_ITALIC));
    python_module_store(tui, "UNDERLINE", mp_obj_new_int(SOLAR_OS_TUI_ATTR_UNDERLINE));
    python_module_store(tui, "KEY_UP", mp_obj_new_int(SOLAR_OS_KEY_UP));
    python_module_store(tui, "KEY_DOWN", mp_obj_new_int(SOLAR_OS_KEY_DOWN));
    python_module_store(tui, "KEY_LEFT", mp_obj_new_int(SOLAR_OS_KEY_LEFT));
    python_module_store(tui, "KEY_RIGHT", mp_obj_new_int(SOLAR_OS_KEY_RIGHT));
    python_module_store(tui, "KEY_HOME", mp_obj_new_int(SOLAR_OS_KEY_HOME));
    python_module_store(tui, "KEY_END", mp_obj_new_int(SOLAR_OS_KEY_END));
    python_module_store(tui, "KEY_DELETE", mp_obj_new_int(SOLAR_OS_KEY_DELETE));
    python_module_store(tui, "KEY_ESCAPE", mp_obj_new_int(SOLAR_OS_KEY_ESCAPE));
    python_module_store(tui, "KEY_CTRL", mp_obj_new_int(SOLAR_OS_KEY_CTRL));
    python_module_store(tui, "KEY_AUDIO_MUTE_TOGGLE",
                        mp_obj_new_int(SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE));
    python_module_store(tui, "KEY_PAGE_UP", mp_obj_new_int(SOLAR_OS_KEY_PAGE_UP));
    python_module_store(tui, "KEY_PAGE_DOWN", mp_obj_new_int(SOLAR_OS_KEY_PAGE_DOWN));
    python_module_store(tui, "rows", MP_OBJ_FROM_PTR(&solaros_tui_rows_obj));
    python_module_store(tui, "cols", MP_OBJ_FROM_PTR(&solaros_tui_cols_obj));
    python_module_store(tui, "size", MP_OBJ_FROM_PTR(&solaros_tui_size_obj));
    python_module_store(tui, "clear", MP_OBJ_FROM_PTR(&solaros_tui_clear_obj));
    python_module_store(tui, "refresh", MP_OBJ_FROM_PTR(&solaros_tui_refresh_obj));
    python_module_store(tui, "move", MP_OBJ_FROM_PTR(&solaros_tui_move_obj));
    python_module_store(tui, "write", MP_OBJ_FROM_PTR(&solaros_tui_write_obj));
    python_module_store(tui, "addstr", MP_OBJ_FROM_PTR(&solaros_tui_addstr_obj));
    python_module_store(tui, "putch", MP_OBJ_FROM_PTR(&solaros_tui_putch_obj));
    python_module_store(tui, "hline", MP_OBJ_FROM_PTR(&solaros_tui_hline_obj));
    python_module_store(tui, "vline", MP_OBJ_FROM_PTR(&solaros_tui_vline_obj));
    python_module_store(tui, "vrule", MP_OBJ_FROM_PTR(&solaros_tui_vrule_obj));
    python_module_store(tui, "box", MP_OBJ_FROM_PTR(&solaros_tui_box_obj));
    python_module_store(tui, "fill", MP_OBJ_FROM_PTR(&solaros_tui_fill_obj));
    python_module_store(tui, "getch", MP_OBJ_FROM_PTR(&solaros_tui_getch_obj));

    mp_obj_t gfx = python_new_submodule(module, "gfx");
    python_module_store(gfx, "WHITE", mp_obj_new_int(SOLAR_OS_GFX_COLOR_WHITE));
    python_module_store(gfx, "LIGHT", mp_obj_new_int(SOLAR_OS_GFX_COLOR_LIGHT));
    python_module_store(gfx, "DARK", mp_obj_new_int(SOLAR_OS_GFX_COLOR_DARK));
    python_module_store(gfx, "BLACK", mp_obj_new_int(SOLAR_OS_GFX_COLOR_BLACK));
    python_module_store(gfx, "GRAY_MAX", mp_obj_new_int(SOLAR_OS_GFX_GRAY_MAX));
    python_module_store(gfx, "FONT_SMALL", mp_obj_new_int(SOLAR_OS_GFX_FONT_SMALL));
    python_module_store(gfx, "FONT_MONO", mp_obj_new_int(SOLAR_OS_GFX_FONT_MONO));
    python_module_store(gfx, "FONT_BOLD", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD));
    python_module_store(gfx, "FONT_MONO_12", mp_obj_new_int(SOLAR_OS_GFX_FONT_MONO_12));
    python_module_store(gfx, "FONT_MONO_14", mp_obj_new_int(SOLAR_OS_GFX_FONT_MONO_14));
    python_module_store(gfx, "FONT_MONO_16", mp_obj_new_int(SOLAR_OS_GFX_FONT_MONO_16));
    python_module_store(gfx, "FONT_MONO_18", mp_obj_new_int(SOLAR_OS_GFX_FONT_MONO_18));
    python_module_store(gfx, "FONT_MONO_20", mp_obj_new_int(SOLAR_OS_GFX_FONT_MONO_20));
    python_module_store(gfx, "FONT_BOLD_12", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_12));
    python_module_store(gfx, "FONT_BOLD_14", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_14));
    python_module_store(gfx, "FONT_BOLD_16", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_16));
    python_module_store(gfx, "FONT_BOLD_18", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_18));
    python_module_store(gfx, "FONT_BOLD_20", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_20));
    python_module_store(gfx, "FONT_ITALIC_12", mp_obj_new_int(SOLAR_OS_GFX_FONT_ITALIC_12));
    python_module_store(gfx, "FONT_ITALIC_14", mp_obj_new_int(SOLAR_OS_GFX_FONT_ITALIC_14));
    python_module_store(gfx, "FONT_ITALIC_16", mp_obj_new_int(SOLAR_OS_GFX_FONT_ITALIC_16));
    python_module_store(gfx, "FONT_ITALIC_18", mp_obj_new_int(SOLAR_OS_GFX_FONT_ITALIC_18));
    python_module_store(gfx, "FONT_ITALIC_20", mp_obj_new_int(SOLAR_OS_GFX_FONT_ITALIC_20));
    python_module_store(gfx, "FONT_BOLD_ITALIC_12", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_ITALIC_12));
    python_module_store(gfx, "FONT_BOLD_ITALIC_14", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_ITALIC_14));
    python_module_store(gfx, "FONT_BOLD_ITALIC_16", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_ITALIC_16));
    python_module_store(gfx, "FONT_BOLD_ITALIC_18", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_ITALIC_18));
    python_module_store(gfx, "FONT_BOLD_ITALIC_20", mp_obj_new_int(SOLAR_OS_GFX_FONT_BOLD_ITALIC_20));
    python_module_store(gfx, "KEY_UP", mp_obj_new_int(SOLAR_OS_KEY_UP));
    python_module_store(gfx, "KEY_DOWN", mp_obj_new_int(SOLAR_OS_KEY_DOWN));
    python_module_store(gfx, "KEY_LEFT", mp_obj_new_int(SOLAR_OS_KEY_LEFT));
    python_module_store(gfx, "KEY_RIGHT", mp_obj_new_int(SOLAR_OS_KEY_RIGHT));
    python_module_store(gfx, "KEY_HOME", mp_obj_new_int(SOLAR_OS_KEY_HOME));
    python_module_store(gfx, "KEY_END", mp_obj_new_int(SOLAR_OS_KEY_END));
    python_module_store(gfx, "KEY_DELETE", mp_obj_new_int(SOLAR_OS_KEY_DELETE));
    python_module_store(gfx, "KEY_ESCAPE", mp_obj_new_int(SOLAR_OS_KEY_ESCAPE));
    python_module_store(gfx, "KEY_CTRL", mp_obj_new_int(SOLAR_OS_KEY_CTRL));
    python_module_store(gfx, "KEY_AUDIO_MUTE_TOGGLE",
                        mp_obj_new_int(SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE));
    python_module_store(gfx, "KEY_PAGE_UP", mp_obj_new_int(SOLAR_OS_KEY_PAGE_UP));
    python_module_store(gfx, "KEY_PAGE_DOWN", mp_obj_new_int(SOLAR_OS_KEY_PAGE_DOWN));
    python_module_store(gfx, "begin", MP_OBJ_FROM_PTR(&solaros_gfx_begin_obj));
    python_module_store(gfx, "end", MP_OBJ_FROM_PTR(&solaros_gfx_end_obj));
    python_module_store(gfx, "width", MP_OBJ_FROM_PTR(&solaros_gfx_width_obj));
    python_module_store(gfx, "height", MP_OBJ_FROM_PTR(&solaros_gfx_height_obj));
    python_module_store(gfx, "size", MP_OBJ_FROM_PTR(&solaros_gfx_size_obj));
    python_module_store(gfx, "clear", MP_OBJ_FROM_PTR(&solaros_gfx_clear_obj));
    python_module_store(gfx, "gray", MP_OBJ_FROM_PTR(&solaros_gfx_gray_obj));
    python_module_store(gfx, "color", MP_OBJ_FROM_PTR(&solaros_gfx_color_obj));
    python_module_store(gfx, "set_color", MP_OBJ_FROM_PTR(&solaros_gfx_color_obj));
    python_module_store(gfx, "font", MP_OBJ_FROM_PTR(&solaros_gfx_font_obj));
    python_module_store(gfx, "set_font", MP_OBJ_FROM_PTR(&solaros_gfx_font_obj));
    python_module_store(gfx, "present", MP_OBJ_FROM_PTR(&solaros_gfx_present_obj));
    python_module_store(gfx, "refresh", MP_OBJ_FROM_PTR(&solaros_gfx_present_obj));
    python_module_store(gfx, "pixel", MP_OBJ_FROM_PTR(&solaros_gfx_pixel_obj));
    python_module_store(gfx, "line", MP_OBJ_FROM_PTR(&solaros_gfx_line_obj));
    python_module_store(gfx, "rect", MP_OBJ_FROM_PTR(&solaros_gfx_rect_obj));
    python_module_store(gfx, "fill_rect", MP_OBJ_FROM_PTR(&solaros_gfx_fill_rect_obj));
    python_module_store(gfx, "circle", MP_OBJ_FROM_PTR(&solaros_gfx_circle_obj));
    python_module_store(gfx, "fill_circle", MP_OBJ_FROM_PTR(&solaros_gfx_fill_circle_obj));
    python_module_store(gfx, "text", MP_OBJ_FROM_PTR(&solaros_gfx_text_obj));
    python_module_store(gfx, "getch", MP_OBJ_FROM_PTR(&solaros_tui_getch_obj));
}

static void python_setup_argv(void)
{
    for (int i = 0; i < python_app.argc; i++) {
        const char *arg = python_app.argv[i];
        mp_obj_list_append(mp_sys_argv, mp_obj_new_str(arg, strlen(arg)));
    }
}

static void python_setup_interactive_helpers(void)
{
    mp_obj_t exit_obj = MP_OBJ_FROM_PTR(&python_builtin_exit_obj);
    mp_store_global(qstr_from_str("exit"), exit_obj);
    mp_store_global(qstr_from_str("quit"), exit_obj);
}

static bool python_exec_repl_source(const char *source)
{
    if (source == NULL || source[0] == '\0') {
        return true;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                                                    source,
                                                    strlen(source),
                                                    0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_SINGLE_INPUT);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true);
        mp_call_function_0(module_fun);
        nlr_pop();
    } else {
        mp_obj_t exception = (mp_obj_t)nlr.ret_val;
        if (mp_obj_exception_match(exception, MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
            python_app.repl_exit_requested = true;
            return false;
        }

        mp_obj_print_exception(&mp_plat_print, exception);
    }

    return true;
}

static bool python_repl_append_line(char *source,
                                    size_t source_size,
                                    size_t *source_len,
                                    const char *line)
{
    if (source == NULL || source_len == NULL || line == NULL) {
        return false;
    }

    const size_t line_len = strlen(line);
    const size_t separator_len = *source_len > 0 ? 1U : 0U;
    if (*source_len + separator_len + line_len + 1U > source_size) {
        return false;
    }

    if (separator_len != 0) {
        source[(*source_len)++] = '\n';
    }
    memcpy(&source[*source_len], line, line_len);
    *source_len += line_len;
    source[*source_len] = '\0';
    return true;
}

static bool python_run_repl(void)
{
    char *source = (char *)python_alloc_psram_first(PYTHON_REPL_SOURCE_MAX);
    if (source == NULL) {
        python_send_message(PYTHON_EVENT_ERROR, "repl buffer allocation failed");
        return false;
    }

    size_t source_len = 0;
    bool more = false;
    bool success = true;
    source[0] = '\0';

    while (!python_app.stop_requested) {
        python_send_prompt(more ? mp_repl_get_ps2() : mp_repl_get_ps1());

        python_input_t input;
        while (!python_app.stop_requested &&
               xQueueReceive(python_app.input, &input, pdMS_TO_TICKS(100)) != pdPASS) {
        }
        if (python_app.stop_requested) {
            break;
        }

        if (!more && input.line[0] == '\0') {
            continue;
        }

        if (!python_repl_append_line(source,
                                     PYTHON_REPL_SOURCE_MAX,
                                     &source_len,
                                     input.line)) {
            python_send_message(PYTHON_EVENT_ERROR, "input block too large");
            source_len = 0;
            source[0] = '\0';
            more = false;
            continue;
        }

        more = mp_repl_continue_with_input(source);
        if (more) {
            continue;
        }

        python_app.repl_executing = true;
        const bool keep_running = python_exec_repl_source(source);
        python_app.repl_executing = false;

        source_len = 0;
        source[0] = '\0';
        gc_collect();

        if (!keep_running) {
            break;
        }
    }

    heap_caps_free(source);
    return success && (!python_app.stop_requested || python_app.repl_exit_requested);
}

static bool python_run_script(void)
{
    uint8_t *script = NULL;
    size_t script_len = 0;
    bool success = false;
    const bool is_mpy = python_path_has_suffix(python_app.path, ".mpy");

    esp_err_t err = python_load_file(python_app.path, &script, &script_len, !is_mpy);
    if (err != ESP_OK) {
        char message[PYTHON_EVENT_DATA_MAX];
        snprintf(message, sizeof(message), "load failed: %s", esp_err_to_name(err));
        python_send_message(PYTHON_EVENT_ERROR, message);
        goto cleanup;
    }

    if (is_mpy) {
        mp_embed_exec_mpy(script, script_len);
    } else {
        mp_embed_exec_str((const char *)script);
    }

    success = !python_app.stop_requested;

cleanup:
    heap_caps_free(script);
    return success;
}

static void python_task(void *arg)
{
    (void)arg;

    SOLAR_OS_LOGI(TAG,
             "task start: mode=%s task=%p",
             python_mode_name(),
             xTaskGetCurrentTaskHandle());

    uint8_t *heap = python_alloc_psram_first(PYTHON_HEAP_SIZE);
    bool success = false;
    if (heap == NULL) {
        python_send_message(PYTHON_EVENT_ERROR, "heap allocation failed");
        goto done;
    }

    int stack_top = 0;
    python_app.vm_active = true;
    mp_embed_init(heap, PYTHON_HEAP_SIZE, &stack_top);
    python_register_solaros_module();
    python_setup_argv();
    python_setup_interactive_helpers();

    if (python_app.stop_requested) {
        mp_sched_keyboard_interrupt();
    } else if (python_app.mode == PYTHON_MODE_REPL) {
        success = python_run_repl();
    } else {
        success = python_run_script();
    }

    if (python_app.vm_active) {
        mp_embed_deinit();
        python_app.vm_active = false;
    }
    heap_caps_free(heap);

done:
    SOLAR_OS_LOGI(TAG,
             "task done: success=%d stop_requested=%d interrupted=%d vm_active=%d",
             success,
             python_app.stop_requested,
             python_app.interrupted,
             python_app.vm_active);

    python_event_t event = {
        .type = PYTHON_EVENT_DONE,
        .success = success,
    };
    (void)python_send_event(&event);

    python_app.task_done = true;
    python_app.task = NULL;
    vTaskDelete(NULL);
}

static void python_render_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage: python [file.py|file.mpy] [args...]");
    solar_os_shell_io_writeln(io, "examples:");
    solar_os_shell_io_writeln(io, "  python");
    solar_os_shell_io_writeln(io, "  python hello.py");
    solar_os_shell_io_writeln(io, "  python /sdcard/apps/demo/main.py arg");
    solar_os_shell_io_flush(io);
}

static bool python_display_io_hidden_by_gfx(solar_os_context_t *ctx, solar_os_shell_io_t *io)
{
    return solar_os_context_graphics_active(ctx) &&
        solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL;
}

static void python_flush_io(solar_os_context_t *ctx, solar_os_shell_io_t *io)
{
    if (io == NULL || python_display_io_hidden_by_gfx(ctx, io)) {
        return;
    }
    solar_os_shell_io_flush(io);
}

static void python_finish_terminal_line(solar_os_context_t *ctx, solar_os_shell_io_t *io)
{
    if (io != NULL && solar_os_shell_io_cursor_col(io) != 0) {
        solar_os_shell_io_newline(io);
        python_flush_io(ctx, io);
    }
}

static void python_return_to_shell(solar_os_context_t *ctx)
{
    solar_os_context_request_terminal_preserve(ctx);
    solar_os_context_request_exit(ctx);
}

static size_t python_repl_max_input_len(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = python_io(ctx);
    const size_t cols = solar_os_shell_io_cols(io);
    const size_t visible_cols = cols > python_app.repl_input_col ?
        cols - python_app.repl_input_col :
        0;
    const size_t buffer_cols = sizeof(python_app.repl_input) - 1;

    return visible_cols < buffer_cols ? visible_cols : buffer_cols;
}

static void python_repl_render_input(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = python_io(ctx);

    solar_os_shell_io_clear_line_from(io,
                                      python_app.repl_input_row,
                                      python_app.repl_input_col);
    solar_os_shell_io_set_cursor(io,
                                 python_app.repl_input_row,
                                 python_app.repl_input_col);
    for (size_t i = 0; i < python_app.repl_input_len; i++) {
        const unsigned char ch = (unsigned char)python_app.repl_input[i];
        solar_os_shell_io_put_char(io, isprint(ch) || ch >= 0xa0 ? (char)ch : '.');
    }
    solar_os_shell_io_set_cursor(io,
                                 python_app.repl_input_row,
                                 python_app.repl_input_col + python_app.repl_input_cursor);
    solar_os_shell_io_flush(io);
}

static void python_repl_move_cursor_left(solar_os_context_t *ctx)
{
    if (python_app.repl_input_cursor == 0) {
        return;
    }

    python_app.repl_input_cursor--;
    python_repl_render_input(ctx);
}

static void python_repl_move_cursor_right(solar_os_context_t *ctx)
{
    if (python_app.repl_input_cursor >= python_app.repl_input_len) {
        return;
    }

    python_app.repl_input_cursor++;
    python_repl_render_input(ctx);
}

static void python_repl_move_cursor_home(solar_os_context_t *ctx)
{
    if (python_app.repl_input_cursor == 0) {
        return;
    }

    python_app.repl_input_cursor = 0;
    python_repl_render_input(ctx);
}

static void python_repl_move_cursor_end(solar_os_context_t *ctx)
{
    if (python_app.repl_input_cursor >= python_app.repl_input_len) {
        return;
    }

    python_app.repl_input_cursor = python_app.repl_input_len;
    python_repl_render_input(ctx);
}

static void python_repl_insert_char(solar_os_context_t *ctx, char ch)
{
    if (python_app.repl_input_len >= python_repl_max_input_len(ctx)) {
        return;
    }

    memmove(&python_app.repl_input[python_app.repl_input_cursor + 1],
            &python_app.repl_input[python_app.repl_input_cursor],
            python_app.repl_input_len - python_app.repl_input_cursor + 1);
    python_app.repl_input[python_app.repl_input_cursor++] = ch;
    python_app.repl_input_len++;
    python_repl_render_input(ctx);
}

static void python_repl_backspace(solar_os_context_t *ctx)
{
    if (python_app.repl_input_cursor == 0) {
        return;
    }

    memmove(&python_app.repl_input[python_app.repl_input_cursor - 1],
            &python_app.repl_input[python_app.repl_input_cursor],
            python_app.repl_input_len - python_app.repl_input_cursor + 1);
    python_app.repl_input_cursor--;
    python_app.repl_input_len--;
    python_repl_render_input(ctx);
}

static void python_repl_submit(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = python_io(ctx);
    solar_os_shell_io_newline(io);
    solar_os_shell_io_flush(io);

    python_input_t input = {0};
    strlcpy(input.line, python_app.repl_input, sizeof(input.line));
    python_app.repl_input_active = false;
    python_app.repl_input_len = 0;
    python_app.repl_input_cursor = 0;
    python_app.repl_input[0] = '\0';

    if (python_app.input == NULL ||
        xQueueSend(python_app.input, &input, 0) != pdPASS) {
        solar_os_shell_io_writeln(io, "python: input queue full");
        solar_os_shell_io_flush(io);
        python_app.repl_input_active = true;
    }
}

static esp_err_t python_start(solar_os_context_t *ctx)
{
    memset(&python_app, 0, sizeof(python_app));
    python_app.ctx = ctx;

    solar_os_shell_io_t *io = python_io(ctx);
    const int argc = solar_os_context_argc(ctx);
    if (argc > SOLAR_OS_APP_ARG_MAX) {
        solar_os_shell_io_writeln(io, "python: too many arguments");
        solar_os_shell_io_flush(io);
        python_return_to_shell(ctx);
        return ESP_OK;
    }

    const bool repl_mode = argc < 2;
    python_app.mode = repl_mode ? PYTHON_MODE_REPL : PYTHON_MODE_SCRIPT;
    python_app.argc = repl_mode ? 1 : argc - 1;
    strlcpy(python_app.argv[0],
            repl_mode ? "python" : solar_os_context_argv(ctx, 1),
            sizeof(python_app.argv[0]));

    if (repl_mode) {
        solar_os_shell_io_clear(io);
        solar_os_shell_io_write_bold(io, "MicroPython on SolarOS");
        solar_os_shell_io_newline(io);
        solar_os_shell_io_printf(io,
                                 "heap: %u KiB\n",
                                 (unsigned)(PYTHON_HEAP_SIZE / 1024U));
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
    }

    if (repl_mode) {
        goto start_task;
    }

    const char *script_arg = solar_os_context_argv(ctx, 1);
    if (script_arg == NULL || script_arg[0] == '\0') {
        python_render_usage(io);
        python_return_to_shell(ctx);
        return ESP_OK;
    }

    esp_err_t path_err = solar_os_storage_resolve_path(script_arg,
                                                       python_app.path,
                                                       sizeof(python_app.path));
    if (path_err != ESP_OK) {
        solar_os_shell_io_printf(io, "python: invalid path: %s\n", esp_err_to_name(path_err));
        solar_os_shell_io_flush(io);
        python_return_to_shell(ctx);
        return ESP_OK;
    }
    if (!python_path_has_suffix(python_app.path, ".py") &&
        !python_path_has_suffix(python_app.path, ".mpy")) {
        solar_os_shell_io_writeln(io, "python: expected .py or .mpy file");
        solar_os_shell_io_flush(io);
        python_return_to_shell(ctx);
        return ESP_OK;
    }

    struct stat st;
    if (stat(python_app.path, &st) != 0 || !S_ISREG(st.st_mode)) {
        solar_os_shell_io_printf(io, "python: not found: %s\n", python_app.path);
        solar_os_shell_io_flush(io);
        python_return_to_shell(ctx);
        return ESP_OK;
    }

    for (int i = 1; i < argc; i++) {
        strlcpy(python_app.argv[i - 1],
                solar_os_context_argv(ctx, i),
                sizeof(python_app.argv[i - 1]));
    }
    strlcpy(python_app.argv[0], python_app.path, sizeof(python_app.argv[0]));

start_task:
    python_app.events = xQueueCreate(PYTHON_EVENT_QUEUE_LEN, sizeof(python_event_t));
    if (python_app.events == NULL) {
        solar_os_shell_io_writeln(io, "python: out of memory");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            python_return_to_shell(ctx);
        }
        return ESP_OK;
    }
    if (repl_mode) {
        python_app.input = xQueueCreate(PYTHON_INPUT_QUEUE_LEN, sizeof(python_input_t));
        if (python_app.input == NULL) {
            vQueueDelete(python_app.events);
            python_app.events = NULL;
            solar_os_shell_io_writeln(io, "python: out of memory");
            solar_os_shell_io_flush(io);
            return ESP_OK;
        }
    } else {
        python_app.key_input = xQueueCreate(PYTHON_KEY_QUEUE_LEN, sizeof(char));
        if (python_app.key_input == NULL) {
            vQueueDelete(python_app.events);
            python_app.events = NULL;
            solar_os_shell_io_writeln(io, "python: out of memory");
            solar_os_shell_io_flush(io);
            python_return_to_shell(ctx);
            return ESP_OK;
        }
    }

    python_app.running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(python_task,
                                                       "solar_os_python",
                                                       PYTHON_TASK_STACK,
                                                       NULL,
                                                       PYTHON_TASK_PRIORITY,
                                                       &python_app.task,
                                                       tskNO_AFFINITY);
    if (created != pdPASS) {
        if (python_app.input != NULL) {
            vQueueDelete(python_app.input);
            python_app.input = NULL;
        }
        if (python_app.key_input != NULL) {
            vQueueDelete(python_app.key_input);
            python_app.key_input = NULL;
        }
        vQueueDelete(python_app.events);
        python_app.events = NULL;
        python_app.running = false;
        solar_os_shell_io_writeln(io, "python: task create failed");
        if (!repl_mode) {
            solar_os_shell_io_flush(io);
            python_return_to_shell(ctx);
        } else {
            solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
            solar_os_shell_io_flush(io);
        }
    }

    return ESP_OK;
}

static void python_interrupt(void)
{
    SOLAR_OS_LOGI(TAG,
             "interrupt request: mode=%s task=%p task_done=%d vm_active=%d running=%d interrupted=%d",
             python_mode_name(),
             python_app.task,
             python_app.task_done,
             python_app.vm_active,
             python_app.running,
             python_app.interrupted);
    python_app.stop_requested = true;
    python_app.interrupted = true;
    if (python_app.vm_active) {
        mp_sched_keyboard_interrupt();
    }
}

static void python_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    SOLAR_OS_LOGI(TAG,
             "stop begin: mode=%s task=%p task_done=%d vm_active=%d stop_requested=%d",
             python_mode_name(),
             python_app.task,
             python_app.task_done,
             python_app.vm_active,
             python_app.stop_requested);

    if (python_app.task != NULL && !python_app.task_done) {
        python_interrupt();
        const TickType_t start = xTaskGetTickCount();
        while (python_app.task != NULL &&
               !python_app.task_done &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(PYTHON_STOP_WAIT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (python_app.task != NULL && !python_app.task_done) {
            SOLAR_OS_LOGW(TAG, "force stopping unresponsive script");
            vTaskDelete(python_app.task);
            python_app.task = NULL;
            python_app.task_done = true;
            python_app.vm_active = false;
        }
    }

    SOLAR_OS_LOGI(TAG,
             "stop cleanup: task=%p task_done=%d vm_active=%d",
             python_app.task,
             python_app.task_done,
             python_app.vm_active);

    if (python_app.events != NULL) {
        vQueueDelete(python_app.events);
        python_app.events = NULL;
    }
    if (python_app.input != NULL) {
        vQueueDelete(python_app.input);
        python_app.input = NULL;
    }
    if (python_app.key_input != NULL) {
        vQueueDelete(python_app.key_input);
        python_app.key_input = NULL;
    }
}

static void python_apply_tui_event(solar_os_context_t *ctx, const python_event_t *event)
{
    solar_os_tui_t tui;
    if (event == NULL || solar_os_tui_begin(&tui, ctx) != ESP_OK) {
        return;
    }

    switch (event->type) {
    case PYTHON_EVENT_TUI_CLEAR:
        solar_os_tui_clear(&tui);
        break;
    case PYTHON_EVENT_TUI_REFRESH:
        solar_os_tui_refresh(&tui);
        break;
    case PYTHON_EVENT_TUI_MOVE:
        solar_os_tui_move(&tui, event->row, event->col);
        break;
    case PYTHON_EVENT_TUI_WRITE:
        solar_os_tui_write(&tui, event->data, event->attr);
        break;
    case PYTHON_EVENT_TUI_PUTCH:
        solar_os_tui_putch(&tui, event->row, event->col, event->codepoint, event->attr);
        break;
    case PYTHON_EVENT_TUI_HLINE:
        solar_os_tui_hline(&tui, event->row, event->col, event->width, 0, event->attr);
        break;
    case PYTHON_EVENT_TUI_VLINE:
        solar_os_tui_vline(&tui, event->row, event->col, event->height, 0, event->attr);
        break;
    case PYTHON_EVENT_TUI_VRULE:
        solar_os_tui_vrule(&tui, event->row, event->col, event->height, event->width, event->attr);
        break;
    case PYTHON_EVENT_TUI_BOX:
        solar_os_tui_box(&tui, event->row, event->col, event->height, event->width, event->attr);
        break;
    case PYTHON_EVENT_TUI_FILL:
        solar_os_tui_fill(&tui,
                          event->row,
                          event->col,
                          event->height,
                          event->width,
                          event->codepoint,
                          event->attr);
        break;
    default:
        break;
    }
}

static void python_apply_gfx_event(solar_os_context_t *ctx, const python_event_t *event)
{
    if (event == NULL) {
        return;
    }

    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    switch (event->type) {
    case PYTHON_EVENT_GFX_BEGIN:
        solar_os_context_set_graphics_active(ctx, true);
        break;
    case PYTHON_EVENT_GFX_END:
        solar_os_context_set_graphics_active(ctx, false);
        if (python_display_terminal(ctx) != NULL) {
            solar_os_terminal_draw(python_display_terminal(ctx));
        }
        break;
    case PYTHON_EVENT_GFX_CLEAR:
        solar_os_gfx_clear(gfx, (solar_os_gfx_color_t)event->attr);
        break;
    case PYTHON_EVENT_GFX_COLOR:
        solar_os_gfx_set_color(gfx, (solar_os_gfx_color_t)event->attr);
        break;
    case PYTHON_EVENT_GFX_FONT:
        solar_os_gfx_set_font(gfx, (solar_os_gfx_font_t)event->attr);
        break;
    case PYTHON_EVENT_GFX_PRESENT:
        solar_os_gfx_present(gfx);
        break;
    case PYTHON_EVENT_GFX_PIXEL:
        solar_os_gfx_pixel(gfx, (int)event->x0, (int)event->y0);
        break;
    case PYTHON_EVENT_GFX_LINE:
        solar_os_gfx_line(gfx,
                          (int)event->x0,
                          (int)event->y0,
                          (int)event->x1,
                          (int)event->y1);
        break;
    case PYTHON_EVENT_GFX_RECT:
        solar_os_gfx_rect(gfx,
                          (int)event->x0,
                          (int)event->y0,
                          (int)event->width,
                          (int)event->height);
        break;
    case PYTHON_EVENT_GFX_FILL_RECT:
        solar_os_gfx_fill_rect(gfx,
                               (int)event->x0,
                               (int)event->y0,
                               (int)event->width,
                               (int)event->height);
        break;
    case PYTHON_EVENT_GFX_CIRCLE:
        solar_os_gfx_circle(gfx, (int)event->x0, (int)event->y0, (int)event->width);
        break;
    case PYTHON_EVENT_GFX_FILL_CIRCLE:
        solar_os_gfx_fill_circle(gfx, (int)event->x0, (int)event->y0, (int)event->width);
        break;
    case PYTHON_EVENT_GFX_TEXT:
        solar_os_gfx_text(gfx, (int)event->x0, (int)event->y0, event->data);
        break;
    default:
        break;
    }
}

static void python_drain_events(solar_os_context_t *ctx)
{
    if (python_app.events == NULL) {
        return;
    }

    solar_os_shell_io_t *io = python_io(ctx);
    python_event_t event;
    uint32_t drained = 0;
    while (drained < PYTHON_DRAIN_EVENTS_PER_TICK &&
           xQueueReceive(python_app.events, &event, 0) == pdPASS) {
        drained++;
        switch (event.type) {
        case PYTHON_EVENT_OUTPUT:
            for (size_t i = 0; i < event.data_len; i++) {
                solar_os_shell_io_put_utf8_byte(io, (uint8_t)event.data[i]);
            }
            break;
        case PYTHON_EVENT_STATUS:
            solar_os_shell_io_printf(io, "python: %s\n", event.data);
            break;
        case PYTHON_EVENT_ERROR:
            solar_os_shell_io_printf(io, "\npython: %s\n", event.data);
            break;
        case PYTHON_EVENT_PROMPT:
            python_app.interrupted = false;
            python_app.repl_input_active = true;
            python_app.repl_input_len = 0;
            python_app.repl_input_cursor = 0;
            python_app.repl_input[0] = '\0';
            solar_os_shell_io_write(io, event.data_len > 0 ? event.data : ">>> ");
            python_app.repl_input_row = solar_os_shell_io_cursor_row(io);
            python_app.repl_input_col = solar_os_shell_io_cursor_col(io);
            break;
        case PYTHON_EVENT_TUI_CLEAR:
        case PYTHON_EVENT_TUI_REFRESH:
        case PYTHON_EVENT_TUI_MOVE:
        case PYTHON_EVENT_TUI_WRITE:
        case PYTHON_EVENT_TUI_PUTCH:
        case PYTHON_EVENT_TUI_HLINE:
        case PYTHON_EVENT_TUI_VLINE:
        case PYTHON_EVENT_TUI_BOX:
        case PYTHON_EVENT_TUI_FILL:
            python_apply_tui_event(ctx, &event);
            break;
        case PYTHON_EVENT_GFX_BEGIN:
        case PYTHON_EVENT_GFX_END:
        case PYTHON_EVENT_GFX_CLEAR:
        case PYTHON_EVENT_GFX_COLOR:
        case PYTHON_EVENT_GFX_FONT:
        case PYTHON_EVENT_GFX_PRESENT:
        case PYTHON_EVENT_GFX_PIXEL:
        case PYTHON_EVENT_GFX_LINE:
        case PYTHON_EVENT_GFX_RECT:
        case PYTHON_EVENT_GFX_FILL_RECT:
        case PYTHON_EVENT_GFX_CIRCLE:
        case PYTHON_EVENT_GFX_FILL_CIRCLE:
        case PYTHON_EVENT_GFX_TEXT:
            python_apply_gfx_event(ctx, &event);
            break;
        case PYTHON_EVENT_DONE:
            python_app.running = false;
            python_app.done = true;
            if (python_app.mode == PYTHON_MODE_SCRIPT) {
                python_finish_terminal_line(ctx, io);
                if (!event.success) {
                    const char *status =
                        python_app.interrupted ? "python: stopped" : "python: failed";
                    solar_os_shell_io_writeln(io, status);
                }
                python_flush_io(ctx, io);
                python_return_to_shell(ctx);
                break;
            }
            if (event.success && python_app.repl_exit_requested) {
                python_finish_terminal_line(ctx, io);
                python_flush_io(ctx, io);
                python_return_to_shell(ctx);
                break;
            }
            solar_os_shell_io_writeln(io, "");
            solar_os_shell_io_printf(io,
                                     "python: %s\n",
                                     event.success ? "done" : "stopped");
            solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
            break;
        default:
            break;
        }
    }
    python_flush_io(ctx, io);
}

static void python_queue_script_key(char ch)
{
    if (python_app.key_input != NULL &&
        xQueueSend(python_app.key_input, &ch, 0) != pdPASS) {
        SOLAR_OS_LOGW(TAG, "python key queue full");
    }
}

static bool python_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        python_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        SOLAR_OS_LOGI(TAG,
                 "app-exit key: mode=%s task=%p running=%d interrupted=%d task_done=%d vm_active=%d",
                 python_mode_name(),
                 python_app.task,
                 python_app.running,
                 python_app.interrupted,
                 python_app.task_done,
                 python_app.vm_active);
        if (python_app.mode == PYTHON_MODE_REPL) {
            if (python_app.running && python_app.repl_executing && !python_app.interrupted) {
                solar_os_shell_io_t *io = python_io(ctx);
                solar_os_shell_io_writeln(io, "\npython: interrupt");
                solar_os_shell_io_flush(io);
                python_interrupt();
                return true;
            }

            solar_os_context_request_exit(ctx);
            return true;
        }

        if (python_app.running && !python_app.interrupted) {
            solar_os_shell_io_t *io = python_io(ctx);
            solar_os_shell_io_writeln(io, "\npython: interrupt");
            solar_os_shell_io_flush(io);
            python_interrupt();
        }

        python_return_to_shell(ctx);
        return true;
    }
    if (python_app.mode == PYTHON_MODE_SCRIPT) {
        python_queue_script_key((char)ch);
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_t *term = solar_os_shell_io_terminal(python_io(ctx));
        if (term != NULL) {
            solar_os_terminal_page_up(term);
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *term = solar_os_shell_io_terminal(python_io(ctx));
        if (term != NULL) {
            solar_os_terminal_page_down(term);
        }
        return true;
    }

    if (python_app.mode == PYTHON_MODE_REPL && python_app.repl_input_active) {
        switch (ch) {
        case SOLAR_OS_KEY_LEFT:
            python_repl_move_cursor_left(ctx);
            break;
        case SOLAR_OS_KEY_RIGHT:
            python_repl_move_cursor_right(ctx);
            break;
        case SOLAR_OS_KEY_HOME:
        case SOLAR_OS_KEY_CTRL_HOME:
            python_repl_move_cursor_home(ctx);
            break;
        case SOLAR_OS_KEY_END:
        case SOLAR_OS_KEY_CTRL_END:
            python_repl_move_cursor_end(ctx);
            break;
        case SOLAR_OS_KEY_ESCAPE:
            if (python_app.repl_input_len > 0) {
                python_app.repl_input_len = 0;
                python_app.repl_input_cursor = 0;
                python_app.repl_input[0] = '\0';
                python_repl_render_input(ctx);
            }
            break;
        case '\r':
        case '\n':
            python_repl_submit(ctx);
            break;
        case '\b':
            python_repl_backspace(ctx);
            break;
        default:
            if (python_is_printable_char((char)ch)) {
                python_repl_insert_char(ctx, (char)ch);
            }
            break;
        }
    }

    return true;
}

const solar_os_app_t solar_os_python_app = {
    .name = "python",
    .summary = "MicroPython runtime",
    .start = python_start,
    .stop = python_stop,
    .event = python_event,
};
