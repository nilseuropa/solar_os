#include "solar_os_lua.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "solar_os_app_registry.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#if SOLAR_OS_PACKAGE_SERVICE_ADC
#include "solar_os_adc.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
#include "solar_os_battery.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble_keyboard.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#endif
#include "solar_os_clipboard.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_gpio.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_I2C
#include "solar_os_i2c.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_expansion.h"
#endif
#include "solar_os_identity.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#if SOLAR_OS_PACKAGE_SERVICE_MQTT
#include "solar_os_mqtt.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
#include "solar_os_net.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SSH
#include "solar_os_ssh_keys.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
#include "solar_os_onewire.h"
#endif
#include "solar_os_port_shell.h"
#include "solar_os_pins.h"
#include "solar_os_queue.h"
#if SOLAR_OS_PACKAGE_SERVICE_PWM
#include "solar_os_pwm.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
#include "solar_os_sensors.h"
#endif
#include "solar_os_sessions.h"
#include "solar_os_shell_io.h"
#if SOLAR_OS_PACKAGE_SERVICE_SPI
#include "solar_os_spi.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_status_led.h"
#endif
#include "solar_os_storage.h"
#include "solar_os_terminal.h"
#include "solar_os_task.h"
#include "solar_os_time.h"
#include "solar_os_tui.h"
#if SOLAR_OS_PACKAGE_SERVICE_UART
#include "solar_os_uart.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
#include "solar_os_wifi.h"
#endif

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define SOLUA_EVENT_QUEUE_LEN 24
#define SOLUA_INPUT_QUEUE_LEN 4
#define SOLUA_KEY_QUEUE_LEN 32
#define SOLUA_EVENT_DATA_MAX 128
#define SOLUA_REPL_INPUT_MAX 256
#define SOLUA_TASK_STACK 12288
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(SOLUA_TASK_STACK);
#define SOLUA_TASK_PRIORITY 5
#define SOLUA_STOP_WAIT_MS 800
#define SOLUA_HOOK_INSTRUCTION_COUNT 10000
#define SOLUA_EXIT_MARKER "__solaros_lua_exit__"

typedef enum {
    SOLUA_EVENT_OUTPUT,
    SOLUA_EVENT_ERROR,
    SOLUA_EVENT_PROMPT,
    SOLUA_EVENT_TUI_CLEAR,
    SOLUA_EVENT_TUI_REFRESH,
    SOLUA_EVENT_TUI_MOVE,
    SOLUA_EVENT_TUI_WRITE,
    SOLUA_EVENT_TUI_PUTCH,
    SOLUA_EVENT_TUI_HLINE,
    SOLUA_EVENT_TUI_VLINE,
    SOLUA_EVENT_TUI_VRULE,
    SOLUA_EVENT_TUI_BOX,
    SOLUA_EVENT_TUI_FILL,
    SOLUA_EVENT_GFX_BEGIN,
    SOLUA_EVENT_GFX_END,
    SOLUA_EVENT_GFX_CLEAR,
    SOLUA_EVENT_GFX_COLOR,
    SOLUA_EVENT_GFX_FONT,
    SOLUA_EVENT_GFX_PRESENT,
    SOLUA_EVENT_GFX_PIXEL,
    SOLUA_EVENT_GFX_LINE,
    SOLUA_EVENT_GFX_RECT,
    SOLUA_EVENT_GFX_FILL_RECT,
    SOLUA_EVENT_GFX_CIRCLE,
    SOLUA_EVENT_GFX_FILL_CIRCLE,
    SOLUA_EVENT_GFX_TEXT,
    SOLUA_EVENT_DONE,
} solua_event_type_t;

typedef enum {
    SOLUA_MODE_REPL,
    SOLUA_MODE_SCRIPT,
} solua_mode_t;

typedef struct {
    solua_event_type_t type;
    bool success;
    size_t data_len;
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
    char data[SOLUA_EVENT_DATA_MAX];
} solua_event_t;

typedef struct {
    bool exit;
    char line[SOLUA_REPL_INPUT_MAX];
} solua_input_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_shell_io_t fallback_io;
    QueueHandle_t events;
    QueueHandle_t input;
    QueueHandle_t key_input;
    TaskHandle_t task;
    solua_mode_t mode;
    bool running;
    bool task_done;
    bool stop_requested;
    bool interrupt_requested;
    bool interrupted;
    bool vm_active;
    bool repl_input_active;
    bool repl_executing;
    bool repl_exit_requested;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    int argc;
    char argv[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char repl_input[SOLUA_REPL_INPUT_MAX];
    solar_os_gfx_t *claimed_gfx;
    char gfx_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char gfx_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    size_t repl_input_len;
    size_t repl_input_cursor;
    size_t repl_input_row;
    size_t repl_input_col;
} solua_state_t;

static const char *TAG = "solar_os_lua";
static EXT_RAM_BSS_ATTR solua_state_t solua;

static solar_os_shell_io_t *solua_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        solar_os_shell_io_init_terminal(&solua.fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &solua.fallback_io);
        io = &solua.fallback_io;
    }
    return io;
}

static void solua_return_to_shell(solar_os_context_t *ctx)
{
    solar_os_context_request_terminal_preserve(ctx);
    solar_os_context_request_exit(ctx);
}

static bool solua_send_event(const solua_event_t *event)
{
    if (event == NULL || solua.events == NULL) {
        return false;
    }

    while (!solua.stop_requested) {
        if (xQueueSend(solua.events, event, pdMS_TO_TICKS(50)) == pdPASS) {
            return true;
        }
    }
    return xQueueSend(solua.events, event, 0) == pdPASS;
}

static void solua_send_message(solua_event_type_t type, const char *message)
{
    solua_event_t event = {
        .type = type,
    };
    if (message != NULL) {
        strlcpy(event.data, message, sizeof(event.data));
        event.data_len = strlen(event.data);
    }
    (void)solua_send_event(&event);
}

static void solua_send_output(const char *data, size_t len)
{
    while (data != NULL && len > 0) {
        solua_event_t event = {
            .type = SOLUA_EVENT_OUTPUT,
        };
        const size_t chunk = len < sizeof(event.data) ? len : sizeof(event.data);
        memcpy(event.data, data, chunk);
        event.data_len = chunk;
        if (!solua_send_event(&event)) {
            return;
        }
        data += chunk;
        len -= chunk;
    }
}

static void solua_send_cstr_output(const char *text)
{
    if (text != NULL) {
        solua_send_output(text, strlen(text));
    }
}

static void *solua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;

    if (nsize == 0) {
        solar_os_memory_free(ptr);
        return NULL;
    }

    if (ptr == NULL) {
        return solar_os_memory_alloc(nsize,
                                     SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                     "lua.heap");
    }
    return solar_os_memory_realloc(ptr,
                                   nsize,
                                   SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                   "lua.heap");
}

static void solua_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (solua.stop_requested || solua.interrupt_requested) {
        luaL_error(L, "interrupted");
    }
}

static int solua_print(lua_State *L)
{
    const int top = lua_gettop(L);
    for (int i = 1; i <= top; i++) {
        if (i > 1) {
            solua_send_cstr_output("\t");
        }
        size_t len = 0;
        const char *text = luaL_tolstring(L, i, &len);
        solua_send_output(text, len);
        lua_pop(L, 1);
    }
    solua_send_cstr_output("\n");
    return 0;
}

static int solua_exit(lua_State *L)
{
    (void)L;
    solua.repl_exit_requested = true;
    return luaL_error(L, SOLUA_EXIT_MARKER);
}

static int solua_panic(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    solua_send_message(SOLUA_EVENT_ERROR, message != NULL ? message : "panic");
    return 0;
}

static void solua_open_libs(lua_State *L)
{
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 1);
    lua_pop(L, 1);

    lua_pushcfunction(L, solua_print);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, solua_exit);
    lua_setglobal(L, "exit");
}

static int solua_check_esp(lua_State *L, esp_err_t err)
{
    if (err != ESP_OK) {
        return luaL_error(L, "%s", esp_err_to_name(err));
    }
    return 0;
}

static void solua_set_func(lua_State *L, int table, const char *name, lua_CFunction fn)
{
    table = lua_absindex(L, table);
    lua_pushcfunction(L, fn);
    lua_setfield(L, table, name);
}

static void solua_set_str(lua_State *L, int table, const char *name, const char *value)
{
    table = lua_absindex(L, table);
    if (value != NULL) {
        lua_pushstring(L, value);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, table, name);
}

static void solua_set_int(lua_State *L, int table, const char *name, lua_Integer value)
{
    table = lua_absindex(L, table);
    lua_pushinteger(L, value);
    lua_setfield(L, table, name);
}

static void solua_set_num(lua_State *L, int table, const char *name, lua_Number value)
{
    table = lua_absindex(L, table);
    lua_pushnumber(L, value);
    lua_setfield(L, table, name);
}

static void solua_set_bool(lua_State *L, int table, const char *name, bool value)
{
    table = lua_absindex(L, table);
    lua_pushboolean(L, value);
    lua_setfield(L, table, name);
}

static const char *solua_optional_str(lua_State *L, int index, const char *fallback)
{
    return lua_isnoneornil(L, index) ? fallback : luaL_checkstring(L, index);
}

static uint32_t solua_optional_u32(lua_State *L, int index, uint32_t fallback)
{
    if (lua_isnoneornil(L, index)) {
        return fallback;
    }
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0) {
        luaL_error(L, "expected non-negative integer");
    }
    return (uint32_t)value;
}

static uint8_t solua_optional_u8(lua_State *L, int index, uint8_t fallback)
{
    const uint32_t value = solua_optional_u32(L, index, fallback);
    if (value > UINT8_MAX) {
        luaL_error(L, "expected integer 0..255");
    }
    return (uint8_t)value;
}

static uint8_t solua_check_u8(lua_State *L, int index)
{
    return solua_optional_u8(L, index, 0);
}

static uint32_t solua_check_u32(lua_State *L, int index)
{
    return solua_optional_u32(L, index, 0);
}

static int solua_check_gpio_pin(lua_State *L, int index)
{
    const lua_Integer pin = luaL_checkinteger(L, index);
    if (pin < 0 || pin > 48) {
        luaL_error(L, "expected GPIO pin 0..48");
    }
    return (int)pin;
}

static size_t solua_check_size(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0) {
        luaL_error(L, "expected non-negative integer");
    }
    return (size_t)value;
}

static uint16_t solua_check_u16_size(lua_State *L, int index)
{
    const size_t value = solua_check_size(L, index);
    if (value > UINT16_MAX) {
        luaL_error(L, "value too large");
    }
    return (uint16_t)value;
}

static uint8_t solua_optional_tui_attr(lua_State *L, int index)
{
    return solua_optional_u8(L, index, SOLAR_OS_TUI_ATTR_NORMAL);
}

static uint32_t solua_codepoint_from_arg(lua_State *L, int index)
{
    if (lua_isinteger(L, index)) {
        const lua_Integer value = lua_tointeger(L, index);
        if (value < 0) {
            luaL_error(L, "expected non-negative codepoint");
        }
        return (uint32_t)value;
    }

    size_t len = 0;
    const unsigned char *text = (const unsigned char *)luaL_checklstring(L, index, &len);
    if (len == 0) {
        luaL_error(L, "expected a character");
    }
    return text[0];
}

static solar_os_gfx_color_t solua_gfx_color_from_arg(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > UINT8_MAX ||
        !solar_os_gfx_color_is_valid((solar_os_gfx_color_t)value)) {
        luaL_error(L, "expected gfx color");
    }
    return (solar_os_gfx_color_t)value;
}

static solar_os_gfx_font_t solua_gfx_font_from_arg(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < SOLAR_OS_GFX_FONT_SMALL || value >= SOLAR_OS_GFX_FONT_COUNT) {
        luaL_error(L, "expected gfx font");
    }
    return (solar_os_gfx_font_t)value;
}

static void solua_resolve_path(lua_State *L, int index, char *path, size_t path_len)
{
    const char *arg = luaL_checkstring(L, index);
    (void)solua_check_esp(L, solar_os_storage_resolve_path(arg, path, path_len));
}

static int solua_table_int(lua_State *L,
                           int table,
                           const char *name,
                           bool required,
                           int fallback)
{
    table = lua_absindex(L, table);
    lua_getfield(L, table, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (required) {
            luaL_error(L, "missing field '%s'", name);
        }
        return fallback;
    }

    const int value = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

static solar_os_datetime_t solua_datetime_from_args(lua_State *L, int first_arg)
{
    const int top = lua_gettop(L);
    solar_os_datetime_t datetime = {0};

    if (top == first_arg && lua_istable(L, first_arg)) {
        datetime.year = (uint16_t)solua_table_int(L, first_arg, "year", true, 0);
        datetime.month = (uint8_t)solua_table_int(L, first_arg, "month", true, 0);
        datetime.day = (uint8_t)solua_table_int(L, first_arg, "day", true, 0);
        datetime.hour = (uint8_t)solua_table_int(L, first_arg, "hour", true, 0);
        datetime.minute = (uint8_t)solua_table_int(L, first_arg, "minute", true, 0);
        datetime.second = (uint8_t)solua_table_int(L, first_arg, "second", false, 0);
        datetime.clock_integrity = true;
        return datetime;
    }

    const int count = top - first_arg + 1;
    if (count < 5 || count > 6) {
        luaL_error(L, "expected table or year,month,day,hour,minute[,second]");
    }

    datetime.year = (uint16_t)luaL_checkinteger(L, first_arg);
    datetime.month = (uint8_t)luaL_checkinteger(L, first_arg + 1);
    datetime.day = (uint8_t)luaL_checkinteger(L, first_arg + 2);
    datetime.hour = (uint8_t)luaL_checkinteger(L, first_arg + 3);
    datetime.minute = (uint8_t)luaL_checkinteger(L, first_arg + 4);
    datetime.second = count >= 6 ? (uint8_t)luaL_checkinteger(L, first_arg + 5) : 0;
    datetime.clock_integrity = true;
    return datetime;
}

static void solua_push_datetime(lua_State *L, const solar_os_datetime_t *datetime)
{
    lua_newtable(L);
    solua_set_int(L, -1, "year", datetime->year);
    solua_set_int(L, -1, "month", datetime->month);
    solua_set_int(L, -1, "day", datetime->day);
    solua_set_int(L, -1, "hour", datetime->hour);
    solua_set_int(L, -1, "minute", datetime->minute);
    solua_set_int(L, -1, "second", datetime->second);
    solua_set_int(L, -1, "weekday", datetime->weekday);
    solua_set_bool(L, -1, "clock_integrity", datetime->clock_integrity);
}

static void solua_push_storage_usage(lua_State *L, const solar_os_storage_usage_t *usage)
{
    lua_newtable(L);
    solua_set_int(L, -1, "total_bytes", (lua_Integer)usage->total_bytes);
    solua_set_int(L, -1, "used_bytes", (lua_Integer)usage->used_bytes);
    solua_set_int(L, -1, "free_bytes", (lua_Integer)usage->free_bytes);
}

static void solua_push_storage_block(lua_State *L, const solar_os_storage_block_t *block)
{
    lua_newtable(L);
    solua_set_str(L, -1, "name", block->name);
    solua_set_str(L, -1,
                  "type",
                  block->type == SOLAR_OS_STORAGE_BLOCK_DISK ? "disk" : "partition");
    solua_set_int(L, -1, "partition_number", block->partition_number);
    solua_set_int(L, -1, "mbr_type", block->mbr_type);
    solua_set_bool(L, -1, "bootable", block->bootable);
    solua_set_bool(L, -1, "mountable", block->mountable);
    solua_set_bool(L, -1, "mounted", block->mounted);
    solua_set_bool(L, -1, "whole_disk_filesystem", block->whole_disk_filesystem);
    solua_set_int(L, -1, "logical_volume", block->logical_volume);
    solua_set_int(L, -1, "start_sector", (lua_Integer)block->start_sector);
    solua_set_int(L, -1, "sector_count", (lua_Integer)block->sector_count);
    solua_set_int(L, -1, "sector_size", block->sector_size);
    solua_set_int(L, -1, "size_bytes", (lua_Integer)block->size_bytes);
    solua_set_str(L, -1, "fs", block->fs);
    solua_set_str(L, -1, "type_name", block->type_name);
    solua_set_str(L, -1, "mount_point", block->mount_point);
}

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
static void solua_push_battery_status(lua_State *L, const solar_os_battery_status_t *status)
{
    lua_newtable(L);
    solua_set_int(L, -1, "voltage_mv", status->voltage_mv);
    solua_set_int(L, -1, "percent", status->percent);
    solua_set_bool(L, -1, "percent_estimated", status->percent_estimated);
    solua_set_bool(L, -1, "adc_calibrated", status->adc_calibrated);
    solua_set_bool(L, -1, "external_power", status->external_power);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
static void solua_push_environment(lua_State *L, const solar_os_environment_t *environment)
{
    lua_newtable(L);
    solua_set_num(L, -1, "temperature_c", environment->temperature_c);
    solua_set_num(L, -1, "humidity_percent", environment->humidity_percent);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
static void solua_push_wifi_status(lua_State *L, const solar_os_wifi_status_t *status)
{
    lua_newtable(L);
    solua_set_str(L, -1, "state", solar_os_wifi_state_name(status->state));
    solua_set_bool(L, -1, "initialized", status->initialized);
    solua_set_bool(L, -1, "started", status->started);
    solua_set_bool(L, -1, "connected", status->connected);
    solua_set_bool(L, -1, "has_ip", status->has_ip);
    solua_set_bool(L, -1, "has_saved_config", status->has_saved_config);
    solua_set_bool(L, -1, "has_saved_ap_config", status->has_saved_ap_config);
    solua_set_bool(L, -1, "nat_enabled", status->nat_enabled);
    solua_set_bool(L, -1, "nat_active", status->nat_active);
    solua_set_bool(L, -1, "ap_enabled", status->ap_enabled);
    solua_set_bool(L, -1, "ap_running", status->ap_running);
    solua_set_str(L, -1, "ssid", status->ssid);
    solua_set_str(L, -1, "saved_ssid", status->saved_ssid);
    solua_set_str(L, -1, "saved_ap_ssid", status->saved_ap_ssid);
    solua_set_str(L, -1, "saved_ap_auth", status->saved_ap_auth);
    solua_set_str(L, -1, "ip", status->ip);
    solua_set_str(L, -1, "gateway", status->gateway);
    solua_set_str(L, -1, "netmask", status->netmask);
    solua_set_str(L, -1, "ap_ssid", status->ap_ssid);
    solua_set_str(L, -1, "ap_auth", status->ap_auth);
    solua_set_str(L, -1, "ap_ip", status->ap_ip);
    solua_set_int(L, -1, "rssi", status->rssi);
    solua_set_int(L, -1, "channel", status->channel);
    solua_set_int(L, -1, "disconnect_reason", status->disconnect_reason);
    solua_set_int(L, -1, "ap_channel", status->ap_channel);
    solua_set_int(L, -1, "ap_station_count", status->ap_station_count);
    solua_set_int(L, -1, "ap_max_connections", status->ap_max_connections);
    solua_set_int(L, -1, "saved_profile_count", status->saved_profile_count);
    solua_set_int(L, -1, "nat_last_error", status->nat_last_error);
    solua_set_str(L, -1, "nat_last_error_name", esp_err_to_name(status->nat_last_error));
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
static void solua_push_audio_status(lua_State *L, const solar_os_audio_status_t *status)
{
    lua_newtable(L);
    solua_set_bool(L, -1, "initialized", status->initialized);
    solua_set_int(L, -1, "sample_rate", status->sample_rate);
    solua_set_int(L, -1, "channels", status->channels);
    solua_set_int(L, -1, "bits_per_sample", status->bits_per_sample);
    solua_set_int(L, -1, "volume", status->volume);
    solua_set_num(L, -1, "mic_gain_db", status->mic_gain_db);
    solua_set_int(L, -1, "i2s_port", status->i2s_port);
    solua_set_int(L, -1, "mclk_pin", status->mclk_pin);
    solua_set_int(L, -1, "bclk_pin", status->bclk_pin);
    solua_set_int(L, -1, "ws_pin", status->ws_pin);
    solua_set_int(L, -1, "din_pin", status->din_pin);
    solua_set_int(L, -1, "dout_pin", status->dout_pin);
    solua_set_int(L, -1, "pa_pin", status->pa_pin);
    solua_set_str(L, -1, "output_codec", status->output_codec);
    solua_set_str(L, -1, "input_codec", status->input_codec);
}

static void solua_push_wav_info(lua_State *L, const solar_os_audio_wav_info_t *info)
{
    lua_newtable(L);
    solua_set_int(L, -1, "sample_rate", info->sample_rate);
    solua_set_int(L, -1, "data_bytes", info->data_bytes);
    solua_set_int(L, -1, "duration_ms", info->duration_ms);
    solua_set_int(L, -1, "block_align", info->block_align);
    solua_set_int(L, -1, "channels", info->channels);
    solua_set_int(L, -1, "bits_per_sample", info->bits_per_sample);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MQTT
static void solua_push_mqtt_status(lua_State *L, const solar_os_mqtt_status_t *status)
{
    lua_newtable(L);
    solua_set_bool(L, -1, "initialized", status->initialized);
    solua_set_bool(L, -1, "configured", status->configured);
    solua_set_bool(L, -1, "running", status->running);
    solua_set_bool(L, -1, "connected", status->connected);
    solua_set_bool(L, -1, "username_set", status->username_set);
    solua_set_bool(L, -1, "password_set", status->password_set);
    solua_set_str(L, -1, "url", status->url);
    solua_set_str(L, -1, "username", status->username);
    solua_set_str(L, -1, "client_id", status->client_id);
    solua_set_str(L, -1, "last_error", status->last_error);
    solua_set_int(L, -1, "last_esp_error", status->last_esp_error);
    solua_set_int(L, -1, "last_msg_id", status->last_msg_id);
    solua_set_int(L, -1, "rx_count", status->rx_count);
    solua_set_int(L, -1, "tx_count", status->tx_count);
    solua_set_int(L, -1, "dropped_count", status->dropped_count);
    solua_set_int(L, -1, "queued_messages", (lua_Integer)status->queued_messages);
}

static void solua_push_mqtt_message(lua_State *L, const solar_os_mqtt_message_t *message)
{
    lua_newtable(L);
    solua_set_str(L, -1, "topic", message->topic);
    lua_pushlstring(L, message->payload, message->payload_len);
    lua_setfield(L, -2, "payload");
    solua_set_int(L, -1, "payload_len", (lua_Integer)message->payload_len);
    solua_set_int(L, -1, "qos", message->qos);
    solua_set_bool(L, -1, "retain", message->retain);
    solua_set_bool(L, -1, "truncated", message->truncated);
}

static void solua_push_ssh_key_status(lua_State *L, const solar_os_ssh_key_status_t *status)
{
    lua_newtable(L);
    solua_set_bool(L, -1, "private_key_exists", status->private_key_exists);
    solua_set_bool(L, -1, "public_key_exists", status->public_key_exists);
    solua_set_int(L, -1, "private_key_size", status->private_key_size);
    solua_set_int(L, -1, "public_key_size", status->public_key_size);
    solua_set_str(L, -1, "private_key_path", status->private_key_path);
    solua_set_str(L, -1, "public_key_path", status->public_key_path);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
static void solua_push_gpio_info(lua_State *L, const solar_os_gpio_pin_info_t *info)
{
    lua_newtable(L);
    solua_set_int(L, -1, "pin", info->pin);
    solua_set_bool(L, -1, "expansion", info->expansion);
    solua_set_bool(L, -1, "allowed", info->runtime_allowed);
    solua_set_bool(L, -1, "available", info->available);
    solua_set_bool(L, -1, "claimed", info->claimed);
    solua_set_str(L, -1, "owner", info->claimed ? info->owner : NULL);
    solua_set_str(L, -1, "policy", solar_os_pin_policy_name(info->policy));
    solua_set_str(L, -1, "role", info->role);
    solua_set_bool(L, -1, "configured", info->configured);
    solua_set_str(L,
                  -1,
                  "mode",
                  info->configured ? solar_os_gpio_mode_name(info->mode) : NULL);
    solua_set_str(L,
                  -1,
                  "pull",
                  info->configured ? solar_os_gpio_pull_name(info->pull) : NULL);
    solua_set_int(L, -1, "level", info->level ? 1 : 0);
    solua_set_bool(L, -1, "level_valid", info->level_valid);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
static void solua_push_adc_info(lua_State *L, const solar_os_adc_pin_info_t *info)
{
    lua_newtable(L);
    solua_set_int(L, -1, "pin", info->pin);
    solua_set_bool(L, -1, "allowed", info->runtime_allowed);
    solua_set_bool(L, -1, "adc_capable", info->adc_capable);
    solua_set_int(L, -1, "unit", info->unit);
    solua_set_int(L, -1, "channel", info->channel);
}

static void solua_push_adc_sample(lua_State *L, const solar_os_adc_sample_t *sample)
{
    lua_newtable(L);
    solua_set_int(L, -1, "pin", sample->pin);
    solua_set_int(L, -1, "raw", sample->raw);
    solua_set_int(L, -1, "voltage_mv", sample->voltage_mv);
    solua_set_int(L, -1, "unit", sample->unit);
    solua_set_int(L, -1, "channel", sample->channel);
    solua_set_bool(L, -1, "calibrated", sample->calibrated);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PWM
static void solua_push_pwm_info(lua_State *L, const solar_os_pwm_pin_info_t *info)
{
    lua_newtable(L);
    solua_set_int(L, -1, "pin", info->pin);
    solua_set_bool(L, -1, "allowed", info->runtime_allowed);
    solua_set_bool(L, -1, "active", info->active);
    solua_set_int(L, -1, "channel", info->channel);
    solua_set_int(L, -1, "freq_hz", info->freq_hz);
    solua_set_int(L, -1, "duty_percent", info->duty_percent);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UART
static void solua_push_uart_status(lua_State *L, const solar_os_uart_status_t *status)
{
    lua_newtable(L);
    solua_set_str(L, -1, "name", status->name);
    solua_set_bool(L, -1, "attached", status->attached);
    solua_set_bool(L, -1, "initialized", status->initialized);
    solua_set_int(L, -1, "port_num", status->port_num);
    solua_set_int(L, -1, "tx_pin", status->tx_pin);
    solua_set_int(L, -1, "rx_pin", status->rx_pin);
    solua_set_int(L, -1, "baud_rate", status->baud_rate);
    solua_set_str(L, -1, "mode", solar_os_uart_mode_name(status->mode));
    solua_set_int(L, -1, "rx_buffered", (lua_Integer)status->rx_buffered);
    solua_set_bool(L, -1, "rx_buffered_valid", status->rx_buffered_valid);
}
#endif

static void solua_push_job_status(lua_State *L, const solar_os_job_status_t *status)
{
    lua_newtable(L);
    solua_set_str(L, -1, "name", status->name);
    solua_set_str(L, -1, "summary", status->summary);
    solua_set_str(L, -1, "state", solar_os_job_state_name(status->state));
    solua_set_int(L, -1, "last_error", status->last_error);
    solua_set_str(L, -1, "last_error_name", esp_err_to_name(status->last_error));
    solua_set_int(L, -1, "tick_count", status->tick_count);
    solua_set_int(L, -1, "last_tick_ms", status->last_tick_ms);
    solua_set_int(L, -1, "tick_interval_ms", status->tick_stats.interval_ms);
    solua_set_int(L, -1, "tick_deadline_ms", status->tick_stats.deadline_ms);
    solua_set_int(L, -1, "tick_last_us", status->tick_stats.last_duration_us);
    solua_set_int(L, -1, "tick_max_us", status->tick_stats.max_duration_us);
    solua_set_int(L, -1, "tick_deadline_misses", status->tick_stats.deadline_miss_count);
}

static bool solua_should_cancel(void *user)
{
    (void)user;
    return solua.stop_requested || solua.interrupt_requested;
}

static int solua_solaros_write(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    solua_send_output(text, len);
    return 0;
}

static int solua_solaros_version(lua_State *L)
{
    lua_pushstring(L, SOLAR_OS_VERSION);
    return 1;
}

static int solua_solaros_should_exit(lua_State *L)
{
    lua_pushboolean(L, solua.stop_requested || solua.interrupt_requested);
    return 1;
}

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
static int solua_solaros_battery_status(lua_State *L)
{
    solar_os_battery_status_t status;
    if (solar_os_battery_get_status(&status) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }
    solua_push_battery_status(L, &status);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
static int solua_solaros_wifi_status_short(lua_State *L)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    lua_newtable(L);
    solua_set_str(L, -1, "state", solar_os_wifi_state_name(status.state));
    solua_set_bool(L, -1, "started", status.started);
    solua_set_bool(L, -1, "connected", status.connected);
    solua_set_bool(L, -1, "has_ip", status.has_ip);
    solua_set_str(L, -1, "ssid", status.ssid);
    solua_set_str(L, -1, "ip", status.ip);
    solua_set_int(L, -1, "rssi", status.rssi);
    solua_set_bool(L, -1, "ap_running", status.ap_running);
    solua_set_str(L, -1, "ap_ssid", status.ap_ssid);
    solua_set_str(L, -1, "ap_ip", status.ap_ip);
    solua_set_bool(L, -1, "nat_enabled", status.nat_enabled);
    solua_set_bool(L, -1, "nat_active", status.nat_active);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
static int solua_solaros_environment(lua_State *L)
{
    solar_os_environment_t environment;
    if (solar_os_sensors_read_environment(&environment) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }
    solua_push_environment(L, &environment);
    return 1;
}
#endif

static int solua_storage_status(lua_State *L)
{
    char status[96];
    solar_os_storage_get_status(status, sizeof(status));
    lua_pushstring(L, status);
    return 1;
}

static int solua_storage_is_mounted(lua_State *L)
{
    lua_pushboolean(L, solar_os_storage_is_mounted());
    return 1;
}

static int solua_storage_mount(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_mount());
}

static int solua_storage_unmount(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_unmount());
}

static int solua_storage_mount_point(lua_State *L)
{
    lua_pushstring(L, solar_os_storage_mount_point());
    return 1;
}

static int solua_storage_usage(lua_State *L)
{
    solar_os_storage_usage_t usage;
    esp_err_t err;
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        err = solar_os_storage_get_usage(&usage);
    } else {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        solua_resolve_path(L, 1, path, sizeof(path));
        err = solar_os_storage_get_usage_for_path(path, &usage);
    }
    (void)solua_check_esp(L, err);
    solua_push_storage_usage(L, &usage);
    return 1;
}

static int solua_storage_resolve(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    lua_pushstring(L, path);
    return 1;
}

static int solua_storage_rescan(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_rescan());
}

static int solua_storage_blocks(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    const size_t count = solar_os_storage_block_count();
    int out = 1;
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (solar_os_storage_get_block(i, &block)) {
            solua_push_storage_block(L, &block);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_storage_block_count(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)solar_os_storage_block_count());
    return 1;
}

static int solua_storage_block(lua_State *L)
{
    const lua_Integer index = luaL_checkinteger(L, 1);
    if (index < 0) {
        return luaL_error(L, "expected non-negative index");
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_storage_block(L, &block);
    return 1;
}

static int solua_storage_usage_for_block(lua_State *L)
{
    const lua_Integer index = luaL_checkinteger(L, 1);
    if (index < 0) {
        return luaL_error(L, "expected non-negative index");
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }

    solar_os_storage_usage_t usage;
    (void)solua_check_esp(L, solar_os_storage_get_usage_for_block(&block, &usage));
    solua_push_storage_usage(L, &usage);
    return 1;
}

static int solua_storage_mkdir(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    return solua_check_esp(L, solar_os_storage_mkdir(path));
}

static int solua_storage_rmdir(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    return solua_check_esp(L, solar_os_storage_rmdir(path));
}

static int solua_storage_remove(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    return solua_check_esp(L, solar_os_storage_remove(path));
}

static int solua_storage_rename(lua_State *L)
{
    char old_path[SOLAR_OS_STORAGE_PATH_MAX];
    char new_path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, old_path, sizeof(old_path));
    solua_resolve_path(L, 2, new_path, sizeof(new_path));
    return solua_check_esp(L, solar_os_storage_rename(old_path, new_path));
}

static int solua_storage_copy(lua_State *L)
{
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char dest[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, source, sizeof(source));
    solua_resolve_path(L, 2, dest, sizeof(dest));
    return solua_check_esp(L, solar_os_storage_copy_file(source, dest));
}

static int solua_storage_mount_volume(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *mount_point = solua_optional_str(L, 2, NULL);
    return solua_check_esp(L, solar_os_storage_mount_volume(name, mount_point));
}

static int solua_storage_unmount_volume(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_unmount_volume(luaL_checkstring(L, 1)));
}

static int solua_time_uptime_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)solar_os_time_uptime_ms());
    return 1;
}

static int solua_time_uptime(lua_State *L)
{
    char buffer[48];
    solar_os_time_format_uptime(solar_os_time_uptime_ms(), buffer, sizeof(buffer));
    lua_pushstring(L, buffer);
    return 1;
}

static int solua_time_datetime(lua_State *L)
{
    solar_os_datetime_t datetime;
    (void)solua_check_esp(L, solar_os_time_get_datetime(&datetime));
    solua_push_datetime(L, &datetime);
    return 1;
}

static int solua_time_utc_datetime(lua_State *L)
{
    solar_os_datetime_t datetime;
    (void)solua_check_esp(L, solar_os_time_get_utc_datetime(&datetime));
    solua_push_datetime(L, &datetime);
    return 1;
}

static int solua_time_set_datetime(lua_State *L)
{
    solar_os_datetime_t datetime = solua_datetime_from_args(L, 1);
    return solua_check_esp(L, solar_os_time_set_datetime(&datetime));
}

static int solua_time_set_utc_datetime(lua_State *L)
{
    solar_os_datetime_t datetime = solua_datetime_from_args(L, 1);
    return solua_check_esp(L, solar_os_time_set_utc_datetime(&datetime));
}

static int solua_time_utc_to_local(lua_State *L)
{
    solar_os_datetime_t utc = solua_datetime_from_args(L, 1);
    solar_os_datetime_t local;
    (void)solua_check_esp(L, solar_os_time_utc_to_local(&utc, &local));
    solua_push_datetime(L, &local);
    return 1;
}

static int solua_time_local_to_utc(lua_State *L)
{
    solar_os_datetime_t local = solua_datetime_from_args(L, 1);
    solar_os_datetime_t utc;
    (void)solua_check_esp(L, solar_os_time_local_to_utc(&local, &utc));
    solua_push_datetime(L, &utc);
    return 1;
}

static int solua_time_is_valid(lua_State *L)
{
    solar_os_datetime_t datetime = solua_datetime_from_args(L, 1);
    lua_pushboolean(L, solar_os_time_datetime_is_valid(&datetime));
    return 1;
}

static int solua_time_timezone(lua_State *L)
{
    char name[SOLAR_OS_TIMEZONE_NAME_MAX];
    char posix[SOLAR_OS_TIMEZONE_POSIX_MAX];
    solar_os_time_get_timezone(name, sizeof(name), posix, sizeof(posix));

    lua_newtable(L);
    solua_set_str(L, -1, "name", name);
    solua_set_str(L, -1, "posix", posix);
    return 1;
}

static int solua_time_set_timezone(lua_State *L)
{
    return solua_check_esp(L, solar_os_time_set_timezone(luaL_checkstring(L, 1)));
}

static int solua_time_ntp_sync(lua_State *L)
{
    const char *server = solua_optional_str(L, 1, SOLAR_OS_NTP_DEFAULT_SERVER);
    const uint32_t timeout_ms = solua_optional_u32(L, 2, SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS);
    solar_os_datetime_t utc;
    solar_os_datetime_t local;
    (void)solua_check_esp(L, solar_os_time_ntp_sync(server, timeout_ms, &utc, &local));

    lua_newtable(L);
    solua_push_datetime(L, &utc);
    lua_setfield(L, -2, "utc");
    solua_push_datetime(L, &local);
    lua_setfield(L, -2, "local");
    return 1;
}

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
static int solua_sensors_environment(lua_State *L)
{
    return solua_solaros_environment(L);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
static int solua_wifi_status(lua_State *L)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    solua_push_wifi_status(L, &status);
    return 1;
}

static int solua_wifi_status_text(lua_State *L)
{
    char text[96];
    solar_os_wifi_get_status_text(text, sizeof(text));
    lua_pushstring(L, text);
    return 1;
}

static int solua_wifi_start(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_start());
}

static int solua_wifi_stop(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_stop());
}

static int solua_wifi_connect(lua_State *L)
{
    const char *ssid = luaL_checkstring(L, 1);
    const char *password = solua_optional_str(L, 2, "");
    return solua_check_esp(L, solar_os_wifi_connect(ssid, password));
}

static int solua_wifi_connect_saved(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_connect_saved());
}

static int solua_wifi_disconnect(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_disconnect());
}

static int solua_wifi_forget(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_forget());
}

static int solua_wifi_forget_ssid(lua_State *L)
{
    const char *ssid = luaL_checkstring(L, 1);
    return solua_check_esp(L, solar_os_wifi_forget_ssid(ssid));
}

static int solua_wifi_forget_all(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_forget_all());
}

static int solua_wifi_scan(lua_State *L)
{
    solar_os_wifi_ap_t aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t found = 0;
    (void)solua_check_esp(L, solar_os_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found));

    lua_newtable(L);
    const int list = lua_gettop(L);
    for (size_t i = 0; i < found; i++) {
        lua_newtable(L);
        solua_set_str(L, -1, "ssid", aps[i].ssid);
        solua_set_str(L, -1, "auth", aps[i].auth);
        solua_set_int(L, -1, "rssi", aps[i].rssi);
        solua_set_int(L, -1, "channel", aps[i].channel);
        solua_set_bool(L, -1, "hidden", aps[i].hidden);
        lua_rawseti(L, list, (lua_Integer)i + 1);
    }
    return 1;
}

static int solua_wifi_known(lua_State *L)
{
    solar_os_wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;
    (void)solua_check_esp(L, solar_os_wifi_known(profiles,
                                                 sizeof(profiles) / sizeof(profiles[0]),
                                                 &count));

    lua_newtable(L);
    const int list = lua_gettop(L);
    const size_t shown = count < SOLAR_OS_WIFI_PROFILE_MAX ? count : SOLAR_OS_WIFI_PROFILE_MAX;
    for (size_t i = 0; i < shown; i++) {
        lua_newtable(L);
        solua_set_str(L, -1, "ssid", profiles[i].ssid);
        solua_set_bool(L, -1, "preferred", profiles[i].preferred);
        lua_rawseti(L, list, (lua_Integer)i + 1);
    }
    return 1;
}

static int solua_wifi_ap_start(lua_State *L)
{
    const char *ssid = solua_optional_str(L, 1, NULL);
    const char *password = solua_optional_str(L, 2, NULL);
    const char *auth = solua_optional_str(L, 3, NULL);
    return solua_check_esp(L, solar_os_wifi_ap_start(ssid, password, auth));
}

static int solua_wifi_ap_stop(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_ap_stop());
}

static int solua_wifi_nat(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_nat_set(lua_toboolean(L, 1)));
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MQTT
static int solua_mqtt_status(lua_State *L)
{
    solar_os_mqtt_status_t status;
    (void)solua_check_esp(L, solar_os_mqtt_get_status(&status));
    solua_push_mqtt_status(L, &status);
    return 1;
}

static int solua_mqtt_connect(lua_State *L)
{
    const char *url = solua_optional_str(L, 1, NULL);
    const char *username = solua_optional_str(L, 2, NULL);
    const char *password = solua_optional_str(L, 3, NULL);
    return solua_check_esp(L, solar_os_mqtt_connect(url, username, password));
}

static int solua_mqtt_disconnect(lua_State *L)
{
    return solua_check_esp(L, solar_os_mqtt_disconnect());
}

static int solua_mqtt_publish(lua_State *L)
{
    const char *topic = luaL_checkstring(L, 1);
    size_t payload_len = 0;
    const char *payload = luaL_checklstring(L, 2, &payload_len);
    const int qos = lua_isnoneornil(L, 3) ? 0 : (int)luaL_checkinteger(L, 3);
    const bool retain = lua_isnoneornil(L, 4) ? false : lua_toboolean(L, 4);

    int msg_id = 0;
    (void)solua_check_esp(L,
                          solar_os_mqtt_publish(topic,
                                                payload,
                                                payload_len,
                                                qos,
                                                retain,
                                                &msg_id));
    lua_pushinteger(L, msg_id);
    return 1;
}

static int solua_mqtt_subscribe(lua_State *L)
{
    const char *topic = luaL_checkstring(L, 1);
    const int qos = lua_isnoneornil(L, 2) ? 0 : (int)luaL_checkinteger(L, 2);
    int msg_id = 0;
    (void)solua_check_esp(L, solar_os_mqtt_subscribe(topic, qos, &msg_id));
    lua_pushinteger(L, msg_id);
    return 1;
}

static int solua_mqtt_read(lua_State *L)
{
    const uint32_t timeout_ms = solua_optional_u32(L, 1, 0);
    solar_os_mqtt_message_t message;
    const esp_err_t err = solar_os_mqtt_read_message(&message, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        lua_pushnil(L);
        return 1;
    }
    (void)solua_check_esp(L, err);
    solua_push_mqtt_message(L, &message);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
static solar_os_gpio_mode_t solua_gpio_mode_from_arg(lua_State *L, int index)
{
    if (lua_isinteger(L, index)) {
        const lua_Integer value = lua_tointeger(L, index);
        if (value == SOLAR_OS_GPIO_MODE_INPUT || value == SOLAR_OS_GPIO_MODE_OUTPUT) {
            return (solar_os_gpio_mode_t)value;
        }
        luaL_error(L, "expected GPIO mode");
    }

    solar_os_gpio_mode_t mode;
    if (!solar_os_gpio_parse_mode(luaL_checkstring(L, index), &mode)) {
        luaL_error(L, "expected input or output");
    }
    return mode;
}

static solar_os_gpio_pull_t solua_gpio_pull_from_arg(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return SOLAR_OS_GPIO_PULL_NONE;
    }
    if (lua_isinteger(L, index)) {
        const lua_Integer value = lua_tointeger(L, index);
        if (value >= SOLAR_OS_GPIO_PULL_NONE && value <= SOLAR_OS_GPIO_PULL_DOWN) {
            return (solar_os_gpio_pull_t)value;
        }
        luaL_error(L, "expected GPIO pull");
    }

    solar_os_gpio_pull_t pull;
    if (!solar_os_gpio_parse_pull(luaL_checkstring(L, index), &pull)) {
        luaL_error(L, "expected none, up, or down");
    }
    return pull;
}

static int solua_gpio_pins(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (solar_os_gpio_get_pin_info(i, &info)) {
            solua_push_gpio_info(L, &info);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_gpio_allowed(lua_State *L)
{
    lua_pushboolean(L, solar_os_gpio_is_runtime_allowed(solua_check_gpio_pin(L, 1)));
    return 1;
}

static int solua_gpio_mode(lua_State *L)
{
    const int pin = solua_check_gpio_pin(L, 1);
    if (lua_gettop(L) == 1) {
        solar_os_gpio_pin_info_t info;
        if (!solar_os_gpio_get_pin_info_by_pin(pin, &info)) {
            return luaL_error(L, "not an expansion GPIO");
        }
        solua_push_gpio_info(L, &info);
        return 1;
    }

    const solar_os_gpio_mode_t mode = solua_gpio_mode_from_arg(L, 2);
    const solar_os_gpio_pull_t pull = solua_gpio_pull_from_arg(L, 3);
    return solua_check_esp(L, solar_os_gpio_configure(pin, mode, pull));
}

static int solua_gpio_read(lua_State *L)
{
    bool level = false;
    (void)solua_check_esp(L, solar_os_gpio_read(solua_check_gpio_pin(L, 1), &level));
    lua_pushinteger(L, level ? 1 : 0);
    return 1;
}

static int solua_gpio_write(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_gpio_write(solua_check_gpio_pin(L, 1),
                                               lua_toboolean(L, 2)));
}

static int solua_gpio_release(lua_State *L)
{
    return solua_check_esp(L, solar_os_gpio_release(solua_check_gpio_pin(L, 1)));
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
static int solua_onewire_allowed(lua_State *L)
{
    lua_pushboolean(L, solar_os_onewire_pin_allowed(solua_check_gpio_pin(L, 1)));
    return 1;
}

static int solua_onewire_reset(lua_State *L)
{
    bool present = false;
    (void)solua_check_esp(L,
                          solar_os_onewire_reset(solua_check_gpio_pin(L, 1), &present));
    lua_pushboolean(L, present);
    return 1;
}

static int solua_onewire_scan(lua_State *L)
{
    uint64_t addresses[SOLAR_OS_ONEWIRE_MAX_DEVICES];
    size_t count = 0;
    (void)solua_check_esp(L,
                          solar_os_onewire_scan(solua_check_gpio_pin(L, 1),
                                                addresses,
                                                SOLAR_OS_ONEWIRE_MAX_DEVICES,
                                                &count));

    lua_newtable(L);
    const int list = lua_gettop(L);
    for (size_t i = 0; i < count; i++) {
        char address[17];
        snprintf(address, sizeof(address), "%016" PRIx64, addresses[i]);

        lua_newtable(L);
        solua_set_str(L, -1, "address", address);
        solua_set_int(L, -1, "family", (uint8_t)addresses[i]);
        lua_rawseti(L, list, (lua_Integer)i + 1);
    }
    return 1;
}

static int solua_onewire_xfer(lua_State *L)
{
    const int pin = solua_check_gpio_pin(L, 1);
    const size_t read_len = solua_check_size(L, 2);
    if (read_len > SOLAR_OS_ONEWIRE_MAX_TRANSFER) {
        return luaL_error(L, "read length exceeds 64 bytes");
    }

    size_t write_len = 0;
    const char *write_data = "";
    if (!lua_isnoneornil(L, 3)) {
        write_data = luaL_checklstring(L, 3, &write_len);
    }
    if (write_len > SOLAR_OS_ONEWIRE_MAX_TRANSFER) {
        return luaL_error(L, "write data exceeds 64 bytes");
    }
    if (read_len == 0 && write_len == 0) {
        return luaL_error(L, "empty transfer");
    }

    uint8_t rx_data[SOLAR_OS_ONEWIRE_MAX_TRANSFER];
    (void)solua_check_esp(L,
                          solar_os_onewire_transfer(pin,
                                                    (const uint8_t *)write_data,
                                                    write_len,
                                                    rx_data,
                                                    read_len));
    lua_pushlstring(L, (const char *)rx_data, read_len);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
static int solua_led_status(lua_State *L)
{
    bool on = false;
    (void)solua_check_esp(L, solar_os_status_led_get(&on));
    lua_pushboolean(L, on);
    return 1;
}

static int solua_led_set(lua_State *L)
{
    const bool on = lua_toboolean(L, 1);
    (void)solua_check_esp(L, solar_os_status_led_set(on));
    lua_pushboolean(L, on);
    return 1;
}

static int solua_led_on(lua_State *L)
{
    (void)solua_check_esp(L, solar_os_status_led_set(true));
    lua_pushboolean(L, true);
    return 1;
}

static int solua_led_off(lua_State *L)
{
    (void)solua_check_esp(L, solar_os_status_led_set(false));
    lua_pushboolean(L, false);
    return 1;
}

static int solua_led_toggle(lua_State *L)
{
    bool on = false;
    (void)solua_check_esp(L, solar_os_status_led_toggle(&on));
    lua_pushboolean(L, on);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
static int solua_adc_pins(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    for (size_t i = 0; i < solar_os_adc_pin_count(); i++) {
        solar_os_adc_pin_info_t info;
        if (solar_os_adc_get_pin_info(i, &info)) {
            solua_push_adc_info(L, &info);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_adc_read(lua_State *L)
{
    solar_os_adc_sample_t sample;
    (void)solua_check_esp(L, solar_os_adc_read(solua_check_gpio_pin(L, 1), &sample));
    solua_push_adc_sample(L, &sample);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PWM
static int solua_pwm_status(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    for (size_t i = 0; i < solar_os_pwm_pin_count(); i++) {
        solar_os_pwm_pin_info_t info;
        if (solar_os_pwm_get_pin_info(i, &info)) {
            solua_push_pwm_info(L, &info);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_pwm_set(lua_State *L)
{
    const uint32_t duty_percent = solua_check_u32(L, 3);
    if (duty_percent > SOLAR_OS_PWM_DUTY_MAX_PERCENT) {
        return luaL_error(L, "expected duty 0..100");
    }
    return solua_check_esp(L,
                           solar_os_pwm_set(solua_check_gpio_pin(L, 1),
                                            solua_check_u32(L, 2),
                                            (uint8_t)duty_percent));
}

static int solua_pwm_off(lua_State *L)
{
    return solua_check_esp(L, solar_os_pwm_stop(solua_check_gpio_pin(L, 1)));
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
static bool solua_bus_find_any(const char *name, solar_os_bus_info_t *info)
{
    for (solar_os_bus_protocol_t protocol = SOLAR_OS_BUS_PROTOCOL_I2C;
         protocol <= SOLAR_OS_BUS_PROTOCOL_ONEWIRE;
         protocol++) {
        if (solar_os_bus_find(name, protocol, info)) {
            return true;
        }
    }
    return false;
}

static void solua_push_bus_info(lua_State *L, const solar_os_bus_info_t *info)
{
    lua_newtable(L);
    solua_set_int(L, -1, "id", (lua_Integer)info->id);
    solua_set_str(L, -1, "name", info->name);
    solua_set_str(L, -1, "protocol", solar_os_bus_protocol_name(info->protocol));
    solua_set_str(L, -1, "origin", solar_os_bus_origin_name(info->origin));
    solua_set_str(L, -1, "sharing", solar_os_bus_sharing_name(info->sharing));
    solua_set_bool(L, -1, "attached", info->attached);
    solua_set_bool(L, -1, "detachable", info->detachable);
    solua_set_bool(L, -1, "ready", info->ready);
    solua_set_int(L, -1, "lease_count", (lua_Integer)info->lease_count);

    switch (info->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        solua_set_int(L, -1, "port", info->config.i2c.port);
        solua_set_int(L, -1, "sda_pin", info->config.i2c.sda_pin);
        solua_set_int(L, -1, "scl_pin", info->config.i2c.scl_pin);
        solua_set_int(L, -1, "speed_hz", info->config.i2c.speed_hz);
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        solua_set_int(L, -1, "host", info->config.spi.host);
        solua_set_int(L, -1, "sclk_pin", info->config.spi.sclk_pin);
        solua_set_int(L, -1, "miso_pin", info->config.spi.miso_pin);
        solua_set_int(L, -1, "mosi_pin", info->config.spi.mosi_pin);
        solua_set_int(L,
                      -1,
                      "max_transfer_size",
                      (lua_Integer)info->config.spi.max_transfer_size);
        lua_newtable(L);
        for (size_t i = 0;
             i < info->config.spi.cs_count && i < SOLAR_OS_BUS_SPI_CS_MAX;
             i++) {
            lua_newtable(L);
            solua_set_str(L, -1, "name", info->config.spi.cs[i].name);
            solua_set_int(L, -1, "pin", info->config.spi.cs[i].pin);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        lua_setfield(L, -2, "cs");
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        solua_set_int(L, -1, "port", info->config.uart.port);
        solua_set_int(L, -1, "tx_pin", info->config.uart.tx_pin);
        solua_set_int(L, -1, "rx_pin", info->config.uart.rx_pin);
        solua_set_int(L, -1, "baud_rate", info->config.uart.baud_rate);
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        solua_set_int(L, -1, "pin", info->config.onewire.pin);
        break;
    default:
        break;
    }
}

static int solua_buses_list(lua_State *L)
{
    lua_newtable(L);
    int out = 1;
    for (size_t i = 0; i < solar_os_bus_count(); i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get(i, &info)) {
            solua_push_bus_info(L, &info);
            lua_rawseti(L, -2, out++);
        }
    }
    return 1;
}

static int solua_buses_get(lua_State *L)
{
    solar_os_bus_info_t info;
    if (!solua_bus_find_any(luaL_checkstring(L, 1), &info)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_bus_info(L, &info);
    return 1;
}

#if SOLAR_OS_PACKAGE_SERVICE_SPI
static int solua_buses_create_spi(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    solar_os_bus_definition_t definition = {
        .name = name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = SOLAR_OS_BUS_SHARED,
        .config.spi = {
            .host = solua_table_int(L, 2, "host", true, -1),
            .sclk_pin = solua_table_int(L, 2, "sclk", true, -1),
            .miso_pin = solua_table_int(L, 2, "miso", false, -1),
            .mosi_pin = solua_table_int(L, 2, "mosi", true, -1),
            .max_transfer_size = (uint32_t)solua_table_int(L,
                                                           2,
                                                           "max_transfer_size",
                                                           false,
                                                           4096),
        },
    };
    if (definition.config.spi.max_transfer_size < 1 ||
        definition.config.spi.max_transfer_size > 65536) {
        return luaL_error(L, "expected max_transfer_size 1..65536");
    }

    lua_getfield(L, 2, "cs");
    luaL_checktype(L, -1, LUA_TTABLE);
    const size_t cs_count = lua_rawlen(L, -1);
    if (cs_count == 0 || cs_count > SOLAR_OS_BUS_SPI_CS_MAX) {
        lua_pop(L, 1);
        return luaL_error(L, "expected 1..4 SPI chip-select pins");
    }
    definition.config.spi.cs_count = (uint8_t)cs_count;
    for (size_t i = 0; i < cs_count; i++) {
        lua_rawgeti(L, -1, (lua_Integer)i + 1);
        const int pin = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        definition.config.spi.cs[i].pin = pin;
        snprintf(definition.config.spi.cs[i].name,
                 sizeof(definition.config.spi.cs[i].name),
                 "gpio%d",
                 pin);
    }
    lua_pop(L, 1);

    (void)solua_check_esp(L, solar_os_bus_register(&definition));
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_SPI, &info)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_bus_info(L, &info);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_I2C
static int solua_buses_create_i2c(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    solar_os_bus_definition_t definition = {
        .name = name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_I2C,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = SOLAR_OS_BUS_SHARED,
        .config.i2c = {
            .port = solua_table_int(L, 2, "port", true, -1),
            .sda_pin = solua_table_int(L, 2, "sda", true, -1),
            .scl_pin = solua_table_int(L, 2, "scl", true, -1),
            .speed_hz = (uint32_t)solua_table_int(L,
                                                  2,
                                                  "speed_hz",
                                                  false,
                                                  SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ),
        },
    };
    if (definition.config.i2c.speed_hz < 1 ||
        definition.config.i2c.speed_hz > 1000000) {
        return luaL_error(L, "expected speed_hz 1..1000000");
    }

    (void)solua_check_esp(L, solar_os_bus_register(&definition));
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_I2C, &info)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_bus_info(L, &info);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
static int solua_buses_create_onewire(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    solar_os_bus_definition_t definition = {
        .name = name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_ONEWIRE,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = SOLAR_OS_BUS_EXCLUSIVE,
        .config.onewire = {
            .pin = solua_table_int(L, 2, "pin", true, -1),
        },
    };

    (void)solua_check_esp(L, solar_os_bus_register(&definition));
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, &info)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_bus_info(L, &info);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UART
static int solua_buses_create_uart(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    solar_os_bus_definition_t definition = {
        .name = name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_UART,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = SOLAR_OS_BUS_EXCLUSIVE,
        .config.uart = {
            .port = solua_table_int(L, 2, "port", true, -1),
            .tx_pin = solua_table_int(L, 2, "tx", true, -1),
            .rx_pin = solua_table_int(L, 2, "rx", true, -1),
            .baud_rate = (uint32_t)solua_table_int(L,
                                                   2,
                                                   "baud_rate",
                                                   false,
                                                   SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE),
        },
    };
    if (definition.config.uart.baud_rate < SOLAR_OS_BUS_UART_MIN_BAUD_RATE ||
        definition.config.uart.baud_rate > SOLAR_OS_BUS_UART_MAX_BAUD_RATE) {
        return luaL_error(L, "expected baud_rate 300..921600");
    }

    (void)solua_check_esp(L, solar_os_bus_register(&definition));
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_UART, &info)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_bus_info(L, &info);
    return 1;
}
#endif

static int solua_buses_remove(lua_State *L)
{
    return solua_check_esp(L, solar_os_bus_unregister(luaL_checkstring(L, 1)));
}

static int solua_buses_attach(lua_State *L)
{
    return solua_check_esp(L, solar_os_bus_attach(luaL_checkstring(L, 1)));
}

static int solua_buses_detach(lua_State *L)
{
    return solua_check_esp(L, solar_os_bus_detach(luaL_checkstring(L, 1)));
}

#if SOLAR_OS_PACKAGE_SERVICE_UART
static const char *solua_bus_uart_name(lua_State *L, int index)
{
    const char *name = luaL_checkstring(L, index);
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_UART, NULL)) {
        luaL_error(L, "%s", esp_err_to_name(ESP_ERR_NOT_FOUND));
    }
    return name;
}

static int solua_buses_uart_write(lua_State *L)
{
    const char *name = solua_bus_uart_name(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    size_t written = 0;
    (void)solua_check_esp(L,
                          solar_os_bus_uart_write_once(name,
                                                       (const uint8_t *)data,
                                                       len,
                                                       &written,
                                                       "lua.buses"));
    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

static int solua_buses_uart_read(lua_State *L)
{
    const char *name = solua_bus_uart_name(L, 1);
    const uint32_t len = solua_optional_u32(L, 2, 64);
    const uint32_t timeout_ms = solua_optional_u32(L, 3, 0);
    if (len == 0 || len > 512) {
        return luaL_error(L, "expected length 1..512");
    }

    uint8_t data[512];
    size_t read_len = 0;
    (void)solua_check_esp(L,
                          solar_os_bus_uart_read_once(name,
                                                      data,
                                                      len,
                                                      timeout_ms,
                                                      &read_len,
                                                      "lua.buses"));
    lua_pushlstring(L, (const char *)data, read_len);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_I2C
static const char *solua_bus_i2c_name(lua_State *L, int index)
{
    const char *name = luaL_checkstring(L, index);
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_I2C, NULL)) {
        luaL_error(L, "%s", esp_err_to_name(ESP_ERR_NOT_FOUND));
    }
    return name;
}

static int solua_buses_i2c_probe(lua_State *L)
{
    const char *name = solua_bus_i2c_name(L, 1);
    return solua_check_esp(L,
                           solar_os_i2c_bus_probe(name, solua_check_u8(L, 2)));
}

static int solua_buses_i2c_scan(lua_State *L)
{
    const char *name = solua_bus_i2c_name(L, 1);
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    for (uint8_t address = SOLAR_OS_I2C_SCAN_MIN_ADDR;
         address <= SOLAR_OS_I2C_SCAN_MAX_ADDR;
         address++) {
        if (solar_os_i2c_bus_probe(name, address) == ESP_OK) {
            lua_pushinteger(L, address);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_buses_i2c_read_reg(lua_State *L)
{
    const char *name = solua_bus_i2c_name(L, 1);
    const uint8_t address = solua_check_u8(L, 2);
    const uint8_t reg = solua_check_u8(L, 3);
    const lua_Integer len = luaL_checkinteger(L, 4);
    if (len <= 0 || len > 256) {
        return luaL_error(L, "expected length 1..256");
    }

    uint8_t data[256];
    (void)solua_check_esp(L,
                          solar_os_i2c_bus_read_reg(name,
                                                    address,
                                                    reg,
                                                    data,
                                                    (size_t)len));
    lua_pushlstring(L, (const char *)data, (size_t)len);
    return 1;
}

static int solua_buses_i2c_write_reg(lua_State *L)
{
    const char *name = solua_bus_i2c_name(L, 1);
    const uint8_t address = solua_check_u8(L, 2);
    const uint8_t reg = solua_check_u8(L, 3);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 4, &len);
    return solua_check_esp(L,
                           solar_os_i2c_bus_write_reg(name,
                                                      address,
                                                      reg,
                                                      (const uint8_t *)data,
                                                      len));
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
static const char *solua_bus_onewire_name(lua_State *L, int index)
{
    const char *name = luaL_checkstring(L, index);
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, NULL)) {
        luaL_error(L, "%s", esp_err_to_name(ESP_ERR_NOT_FOUND));
    }
    return name;
}

static int solua_buses_onewire_reset(lua_State *L)
{
    bool present = false;
    (void)solua_check_esp(L,
                          solar_os_onewire_bus_reset(solua_bus_onewire_name(L, 1),
                                                     &present));
    lua_pushboolean(L, present);
    return 1;
}

static int solua_buses_onewire_scan(lua_State *L)
{
    uint64_t addresses[SOLAR_OS_ONEWIRE_MAX_DEVICES];
    size_t count = 0;
    (void)solua_check_esp(L,
                          solar_os_onewire_bus_scan(solua_bus_onewire_name(L, 1),
                                                    addresses,
                                                    SOLAR_OS_ONEWIRE_MAX_DEVICES,
                                                    &count));

    lua_newtable(L);
    const int list = lua_gettop(L);
    for (size_t i = 0; i < count; i++) {
        char address[17];
        snprintf(address, sizeof(address), "%016" PRIx64, addresses[i]);
        lua_newtable(L);
        solua_set_str(L, -1, "address", address);
        solua_set_int(L, -1, "family", (uint8_t)addresses[i]);
        lua_rawseti(L, list, (lua_Integer)i + 1);
    }
    return 1;
}

static int solua_buses_onewire_xfer(lua_State *L)
{
    const char *name = solua_bus_onewire_name(L, 1);
    const size_t read_len = solua_check_size(L, 2);
    if (read_len > SOLAR_OS_ONEWIRE_MAX_TRANSFER) {
        return luaL_error(L, "read length exceeds 64 bytes");
    }

    size_t write_len = 0;
    const char *write_data = "";
    if (!lua_isnoneornil(L, 3)) {
        write_data = luaL_checklstring(L, 3, &write_len);
    }
    if (write_len > SOLAR_OS_ONEWIRE_MAX_TRANSFER) {
        return luaL_error(L, "write data exceeds 64 bytes");
    }
    if (read_len == 0 && write_len == 0) {
        return luaL_error(L, "empty transfer");
    }

    uint8_t rx_data[SOLAR_OS_ONEWIRE_MAX_TRANSFER];
    (void)solua_check_esp(L,
                          solar_os_onewire_bus_transfer(name,
                                                        (const uint8_t *)write_data,
                                                        write_len,
                                                        rx_data,
                                                        read_len));
    lua_pushlstring(L, (const char *)rx_data, read_len);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SPI
static int solua_bus_spi_cs_from_arg(lua_State *L,
                                     const solar_os_bus_info_t *info,
                                     int index)
{
    int pin = -1;
    if (lua_isinteger(L, index)) {
        pin = (int)lua_tointeger(L, index);
    } else {
        const char *name = luaL_checkstring(L, index);
        for (size_t i = 0;
             i < info->config.spi.cs_count && i < SOLAR_OS_BUS_SPI_CS_MAX;
             i++) {
            if (strcmp(name, info->config.spi.cs[i].name) == 0) {
                pin = info->config.spi.cs[i].pin;
                break;
            }
        }
    }
    for (size_t i = 0;
         i < info->config.spi.cs_count && i < SOLAR_OS_BUS_SPI_CS_MAX;
         i++) {
        if (pin == info->config.spi.cs[i].pin) {
            return pin;
        }
    }
    luaL_error(L, "%s", esp_err_to_name(ESP_ERR_NOT_FOUND));
    return -1;
}

static void solua_bus_spi_args(lua_State *L,
                               int mode_index,
                               int speed_index,
                               solar_os_bus_info_t *info,
                               int *cs_pin,
                               uint8_t *mode,
                               uint32_t *speed_hz)
{
    if (!solar_os_bus_find(luaL_checkstring(L, 1),
                           SOLAR_OS_BUS_PROTOCOL_SPI,
                           info)) {
        luaL_error(L, "%s", esp_err_to_name(ESP_ERR_NOT_FOUND));
    }
    *cs_pin = solua_bus_spi_cs_from_arg(L, info, 2);
    *mode = solua_optional_u8(L, mode_index, 0);
    if (*mode > 3) {
        luaL_error(L, "expected SPI mode 0..3");
    }
    *speed_hz = solua_optional_u32(L,
                                   speed_index,
                                   SOLAR_OS_BUS_SPI_DEFAULT_SPEED_HZ);
    if (*speed_hz == 0 || *speed_hz > SOLAR_OS_BUS_SPI_MAX_SPEED_HZ) {
        luaL_error(L, "expected SPI speed 1..20000000 Hz");
    }
}

static uint8_t *solua_bus_spi_alloc(lua_State *L, size_t len)
{
    uint8_t *data = solar_os_memory_alloc(len,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "lua.buses");
    if (data == NULL) {
        luaL_error(L, "SPI buffer allocation failed");
    }
    return data;
}

static int solua_buses_spi_xfer(lua_State *L)
{
    solar_os_bus_info_t info;
    int cs_pin = -1;
    uint8_t mode = 0;
    uint32_t speed_hz = 0;
    solua_bus_spi_args(L, 4, 5, &info, &cs_pin, &mode, &speed_hz);
    size_t len = 0;
    const char *tx = luaL_checklstring(L, 3, &len);
    if (len == 0 || len > info.config.spi.max_transfer_size) {
        return luaL_error(L, "invalid SPI transfer length");
    }
    uint8_t *rx = solua_bus_spi_alloc(L, len);
    const esp_err_t ret = solar_os_bus_spi_transfer_once(info.name,
                                                         cs_pin,
                                                         mode,
                                                         speed_hz,
                                                         (const uint8_t *)tx,
                                                         rx,
                                                         len,
                                                         "lua-spi");
    if (ret != ESP_OK) {
        solar_os_memory_free(rx);
        return solua_check_esp(L, ret);
    }
    lua_pushlstring(L, (const char *)rx, len);
    solar_os_memory_free(rx);
    return 1;
}

static int solua_buses_spi_read(lua_State *L)
{
    solar_os_bus_info_t info;
    int cs_pin = -1;
    uint8_t mode = 0;
    uint32_t speed_hz = 0;
    solua_bus_spi_args(L, 5, 6, &info, &cs_pin, &mode, &speed_hz);
    const size_t len = solua_check_size(L, 3);
    if (len == 0 || len > info.config.spi.max_transfer_size) {
        return luaL_error(L, "invalid SPI transfer length");
    }
    const uint8_t fill = solua_optional_u8(L, 4, 0xff);
    uint8_t *buffers = solua_bus_spi_alloc(L, len * 2U);
    uint8_t *tx = buffers;
    uint8_t *rx = buffers + len;
    memset(tx, fill, len);
    const esp_err_t ret = solar_os_bus_spi_transfer_once(info.name,
                                                         cs_pin,
                                                         mode,
                                                         speed_hz,
                                                         tx,
                                                         rx,
                                                         len,
                                                         "lua-spi");
    if (ret != ESP_OK) {
        solar_os_memory_free(buffers);
        return solua_check_esp(L, ret);
    }
    lua_pushlstring(L, (const char *)rx, len);
    solar_os_memory_free(buffers);
    return 1;
}

static int solua_buses_spi_write(lua_State *L)
{
    solar_os_bus_info_t info;
    int cs_pin = -1;
    uint8_t mode = 0;
    uint32_t speed_hz = 0;
    solua_bus_spi_args(L, 4, 5, &info, &cs_pin, &mode, &speed_hz);
    size_t len = 0;
    const char *tx = luaL_checklstring(L, 3, &len);
    if (len == 0 || len > info.config.spi.max_transfer_size) {
        return luaL_error(L, "invalid SPI transfer length");
    }
    (void)solua_check_esp(L,
                          solar_os_bus_spi_transfer_once(info.name,
                                                         cs_pin,
                                                         mode,
                                                         speed_hz,
                                                         (const uint8_t *)tx,
                                                         NULL,
                                                         len,
                                                         "lua-spi"));
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}
#endif
#endif

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
static void solua_push_expansion_binding(lua_State *L,
                                         const solar_os_expansion_binding_t *binding)
{
    lua_newtable(L);
    solua_set_str(L,
                  -1,
                  "kind",
                  solar_os_expansion_binding_kind_name(binding->kind));
    solua_set_str(L, -1, "role", binding->role);
    solua_set_str(L, -1, "target", binding->target);
    solua_set_int(L, -1, "value", binding->value);
    solua_set_int(L, -1, "aux", binding->aux);
}

static int solua_expansion_drivers(lua_State *L)
{
    lua_newtable(L);
    int out = 1;
    for (size_t i = 0; i < solar_os_expansion_driver_count(); i++) {
        solar_os_expansion_driver_t driver;
        if (!solar_os_expansion_get_driver(i, &driver)) {
            continue;
        }
        lua_newtable(L);
        solua_set_str(L, -1, "name", driver.name);
        solua_set_str(L, -1, "summary", driver.summary);
        solua_set_int(L,
                      -1,
                      "required_capabilities",
                      (lua_Integer)driver.required_capabilities);
        solua_set_bool(L, -1, "probe_supported", driver.probe_supported);
        solua_set_bool(L,
                       -1,
                       "supported",
                       solar_os_expansion_driver_supported(driver.name));
        lua_rawseti(L, -2, out++);
    }
    return 1;
}

static int solua_expansion_devices(lua_State *L)
{
    lua_newtable(L);
    int out = 1;
    for (size_t i = 0; i < solar_os_expansion_device_count(); i++) {
        solar_os_expansion_device_t device;
        if (!solar_os_expansion_get_device(i, &device)) {
            continue;
        }
        lua_newtable(L);
        solua_set_str(L, -1, "name", device.name);
        solua_set_str(L, -1, "driver", device.driver);
        lua_newtable(L);
        for (size_t j = 0; j < device.binding_count; j++) {
            solua_push_expansion_binding(L, &device.bindings[j]);
            lua_rawseti(L, -2, (lua_Integer)j + 1);
        }
        lua_setfield(L, -2, "bindings");
        lua_rawseti(L, -2, out++);
    }
    return 1;
}

static bool solua_expansion_key_known(const char *key)
{
    static const char *const keys[] = {
        "spi", "cs", "ce", "i2c", "addr", "uart", "gpio", "irq", "reset",
        "rst", "dc", "busy", "adc", "pwm",
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void solua_expansion_validate_keys(lua_State *L, int table)
{
    table = lua_absindex(L, table);
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        const char *key = luaL_checkstring(L, -2);
        if (!solua_expansion_key_known(key)) {
            luaL_error(L, "unknown expansion binding %s", key);
        }
        lua_pop(L, 1);
    }
}

static const char *solua_table_optional_string(lua_State *L,
                                               int table,
                                               const char *key)
{
    table = lua_absindex(L, table);
    lua_getfield(L, table, key);
    const char *value = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool solua_table_optional_int(lua_State *L,
                                     int table,
                                     const char *key,
                                     int *value)
{
    table = lua_absindex(L, table);
    lua_getfield(L, table, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    *value = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return true;
}

static void solua_expansion_add_binding(lua_State *L,
                                        solar_os_expansion_binding_t *bindings,
                                        size_t *count,
                                        solar_os_expansion_binding_kind_t kind,
                                        const char *role,
                                        const char *target,
                                        int value,
                                        int aux)
{
    if (*count >= SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX) {
        luaL_error(L, "too many expansion bindings");
    }
    solar_os_expansion_binding_t *binding = &bindings[(*count)++];
    *binding = (solar_os_expansion_binding_t) {
        .kind = kind,
        .value = value,
        .aux = aux,
    };
    strlcpy(binding->role, role != NULL ? role : "", sizeof(binding->role));
    strlcpy(binding->target, target != NULL ? target : "", sizeof(binding->target));
}

static int solua_expansion_attach(lua_State *L)
{
    const char *driver = luaL_checkstring(L, 1);
    const char *name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    solua_expansion_validate_keys(L, 3);

    solar_os_expansion_binding_t bindings[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX] = {0};
    size_t binding_count = 0;
    const char *spi = solua_table_optional_string(L, 3, "spi");
    const char *i2c = solua_table_optional_string(L, 3, "i2c");
    const char *uart = solua_table_optional_string(L, 3, "uart");
    int value = 0;

    if (spi != NULL) {
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_SPI_BUS,
                                    "",
                                    spi,
                                    -1,
                                    -1);
    }
    int cs = 0;
    int ce = 0;
    const bool has_cs = solua_table_optional_int(L, 3, "cs", &cs);
    const bool has_ce = solua_table_optional_int(L, 3, "ce", &ce);
    if (has_cs && has_ce) {
        return luaL_error(L, "use cs or ce, not both");
    }
    if (has_cs || has_ce) {
        if (spi == NULL) {
            return luaL_error(L, "cs requires spi");
        }
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_SPI_CS,
                                    "cs",
                                    spi,
                                    has_cs ? cs : ce,
                                    -1);
    }
    if (i2c != NULL) {
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_I2C_BUS,
                                    "",
                                    i2c,
                                    -1,
                                    -1);
    }
    if (solua_table_optional_int(L, 3, "addr", &value)) {
        if (i2c == NULL) {
            return luaL_error(L, "addr requires i2c");
        }
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
                                    "",
                                    i2c,
                                    value,
                                    -1);
    }
    if (uart != NULL) {
        solar_os_expansion_uart_port_t port;
        if (!solar_os_expansion_find_uart_port(uart, &port, NULL)) {
            return solua_check_esp(L, ESP_ERR_NOT_FOUND);
        }
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_UART_PORT,
                                    "",
                                    uart,
                                    port.port,
                                    -1);
    }

    static const struct {
        const char *key;
        const char *role;
        solar_os_expansion_binding_kind_t kind;
    } pin_bindings[] = {
        {"gpio", "gpio", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"irq", "irq", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"reset", "reset", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"rst", "reset", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"dc", "dc", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"busy", "busy", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"adc", "adc", SOLAR_OS_EXPANSION_BINDING_ADC},
        {"pwm", "pwm", SOLAR_OS_EXPANSION_BINDING_PWM},
    };
    int reset = 0;
    int rst = 0;
    if (solua_table_optional_int(L, 3, "reset", &reset) &&
        solua_table_optional_int(L, 3, "rst", &rst)) {
        return luaL_error(L, "use reset or rst, not both");
    }
    for (size_t i = 0; i < sizeof(pin_bindings) / sizeof(pin_bindings[0]); i++) {
        if (!solua_table_optional_int(L, 3, pin_bindings[i].key, &value)) {
            continue;
        }
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    pin_bindings[i].kind,
                                    pin_bindings[i].role,
                                    "",
                                    value,
                                    -1);
    }

    return solua_check_esp(L,
                           solar_os_expansion_attach(driver,
                                                     name,
                                                     bindings,
                                                     binding_count));
}

static int solua_expansion_detach(lua_State *L)
{
    return solua_check_esp(L, solar_os_expansion_detach(luaL_checkstring(L, 1)));
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_I2C
static int solua_i2c_info(lua_State *L)
{
    lua_newtable(L);
    solua_set_int(L, -1, "speed_hz", solar_os_i2c_get_speed_hz());
    solua_set_int(L, -1, "sda_pin", solar_os_i2c_get_sda_pin());
    solua_set_int(L, -1, "scl_pin", solar_os_i2c_get_scl_pin());
    return 1;
}

static int solua_i2c_probe(lua_State *L)
{
    return solua_check_esp(L, solar_os_i2c_probe(solua_check_u8(L, 1)));
}

static int solua_i2c_scan(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    for (uint8_t address = SOLAR_OS_I2C_SCAN_MIN_ADDR;
         address <= SOLAR_OS_I2C_SCAN_MAX_ADDR;
         address++) {
        if (solar_os_i2c_probe(address) == ESP_OK) {
            lua_pushinteger(L, address);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_i2c_read_reg(lua_State *L)
{
    const uint8_t address = solua_check_u8(L, 1);
    const uint8_t reg = solua_check_u8(L, 2);
    const lua_Integer len = luaL_checkinteger(L, 3);
    if (len <= 0 || len > 256) {
        return luaL_error(L, "expected length 1..256");
    }

    uint8_t data[256];
    (void)solua_check_esp(L, solar_os_i2c_read_reg(address, reg, data, (size_t)len));
    lua_pushlstring(L, (const char *)data, (size_t)len);
    return 1;
}

static int solua_i2c_write_reg(lua_State *L)
{
    const uint8_t address = solua_check_u8(L, 1);
    const uint8_t reg = solua_check_u8(L, 2);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    return solua_check_esp(L,
                           solar_os_i2c_write_reg(address,
                                                  reg,
                                                  (const uint8_t *)data,
                                                  len));
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SPI
static int solua_spi_cs_from_arg(lua_State *L, int index)
{
    char pin_text[12];
    const char *name = NULL;
    if (lua_isinteger(L, index)) {
        snprintf(pin_text, sizeof(pin_text), "%d", solua_check_gpio_pin(L, index));
        name = pin_text;
    } else {
        name = luaL_checkstring(L, index);
    }

    int pin = -1;
    (void)solua_check_esp(L, solar_os_spi_resolve_cs(name, &pin));
    return pin;
}

static uint8_t solua_spi_mode(lua_State *L, int index)
{
    const uint8_t mode = solua_optional_u8(L, index, 0);
    if (mode > 3) {
        luaL_error(L, "expected SPI mode 0..3");
    }
    return mode;
}

static uint32_t solua_spi_speed(lua_State *L, int index)
{
    const uint32_t speed_hz = solua_optional_u32(L, index, SOLAR_OS_SPI_DEFAULT_SPEED_HZ);
    if (speed_hz == 0 || speed_hz > SOLAR_OS_SPI_MAX_SPEED_HZ) {
        luaL_error(L, "expected SPI speed 1..20000000 Hz");
    }
    return speed_hz;
}

static void solua_spi_validate_length(lua_State *L, size_t len)
{
    solar_os_spi_status_t status;
    (void)solua_check_esp(L, solar_os_spi_get_status(&status));
    if (len == 0 || len > status.max_transfer_size) {
        luaL_error(L, "invalid SPI transfer length");
    }
}

static uint8_t *solua_spi_alloc(lua_State *L, size_t len)
{
    uint8_t *data = solar_os_memory_alloc(len,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "lua.spi");
    if (data == NULL) {
        luaL_error(L, "SPI buffer allocation failed");
    }
    return data;
}

static int solua_spi_status(lua_State *L)
{
    solar_os_spi_status_t status;
    (void)solua_check_esp(L, solar_os_spi_get_status(&status));

    lua_newtable(L);
    solua_set_bool(L, -1, "available", status.available);
    solua_set_int(L, -1, "host", status.host);
    solua_set_str(L, -1, "name", status.name);
    solua_set_int(L, -1, "sclk_pin", status.sclk_pin);
    solua_set_int(L, -1, "miso_pin", status.miso_pin);
    solua_set_int(L, -1, "mosi_pin", status.mosi_pin);
    solua_set_int(L, -1, "max_transfer_size", status.max_transfer_size);
    solua_set_int(L, -1, "default_speed_hz", status.default_speed_hz);

    lua_newtable(L);
    const int cs = lua_gettop(L);
    for (size_t i = 0; i < status.cs_count; i++) {
        lua_newtable(L);
        solua_set_str(L, -1, "name", status.cs[i].name);
        solua_set_int(L, -1, "pin", status.cs[i].pin);
        lua_rawseti(L, cs, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "cs");
    return 1;
}

static int solua_spi_xfer(lua_State *L)
{
    const int cs_pin = solua_spi_cs_from_arg(L, 1);
    size_t len = 0;
    const char *tx = luaL_checklstring(L, 2, &len);
    solua_spi_validate_length(L, len);
    const uint8_t mode = solua_spi_mode(L, 3);
    const uint32_t speed_hz = solua_spi_speed(L, 4);
    uint8_t *rx = solua_spi_alloc(L, len);

    const esp_err_t err = solar_os_spi_transfer(cs_pin,
                                                mode,
                                                speed_hz,
                                                (const uint8_t *)tx,
                                                rx,
                                                len);
    if (err != ESP_OK) {
        solar_os_memory_free(rx);
        return solua_check_esp(L, err);
    }

    lua_pushlstring(L, (const char *)rx, len);
    solar_os_memory_free(rx);
    return 1;
}

static int solua_spi_read(lua_State *L)
{
    const int cs_pin = solua_spi_cs_from_arg(L, 1);
    const size_t len = solua_check_size(L, 2);
    solua_spi_validate_length(L, len);
    const uint8_t fill = solua_optional_u8(L, 3, 0xff);
    const uint8_t mode = solua_spi_mode(L, 4);
    const uint32_t speed_hz = solua_spi_speed(L, 5);
    uint8_t *buffers = solua_spi_alloc(L, len * 2U);
    uint8_t *tx = buffers;
    uint8_t *rx = buffers + len;
    memset(tx, fill, len);

    const esp_err_t err = solar_os_spi_transfer(cs_pin, mode, speed_hz, tx, rx, len);
    if (err != ESP_OK) {
        solar_os_memory_free(buffers);
        return solua_check_esp(L, err);
    }

    lua_pushlstring(L, (const char *)rx, len);
    solar_os_memory_free(buffers);
    return 1;
}

static int solua_spi_write(lua_State *L)
{
    const int cs_pin = solua_spi_cs_from_arg(L, 1);
    size_t len = 0;
    const char *tx = luaL_checklstring(L, 2, &len);
    solua_spi_validate_length(L, len);
    (void)solua_check_esp(L,
                          solar_os_spi_transfer(cs_pin,
                                                solua_spi_mode(L, 3),
                                                solua_spi_speed(L, 4),
                                                (const uint8_t *)tx,
                                                NULL,
                                                len));
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UART
static int solua_uart_status(lua_State *L)
{
    solar_os_uart_status_t status;
    solar_os_uart_get_status(&status);
    solua_push_uart_status(L, &status);
    return 1;
}

static int solua_uart_baud(lua_State *L)
{
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        solar_os_uart_status_t status;
        solar_os_uart_get_status(&status);
        lua_pushinteger(L, status.baud_rate);
        return 1;
    }
    return solua_check_esp(L, solar_os_uart_set_baud_rate(solua_check_u32(L, 1)));
}

static int solua_uart_is_valid_baud(lua_State *L)
{
    const lua_Integer baud = luaL_checkinteger(L, 1);
    lua_pushboolean(L, baud >= 0 && solar_os_uart_is_valid_baud_rate((uint32_t)baud));
    return 1;
}

static int solua_uart_mode(lua_State *L)
{
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        solar_os_uart_status_t status;
        solar_os_uart_get_status(&status);
        lua_pushstring(L, solar_os_uart_mode_name(status.mode));
        return 1;
    }

    solar_os_uart_mode_t mode;
    if (!solar_os_uart_parse_mode(luaL_checkstring(L, 1), &mode)) {
        return luaL_error(L, "expected raw or line");
    }
    return solua_check_esp(L, solar_os_uart_set_mode(mode));
}

static int solua_uart_write(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    size_t written = 0;
    (void)solua_check_esp(L, solar_os_uart_write((const uint8_t *)data, len, &written));
    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

static int solua_uart_read(lua_State *L)
{
    const uint32_t len = solua_optional_u32(L, 1, 64);
    const uint32_t timeout_ms = solua_optional_u32(L, 2, 0);
    if (len == 0 || len > 512) {
        return luaL_error(L, "expected length 1..512");
    }

    uint8_t data[512];
    size_t read_len = 0;
    (void)solua_check_esp(L, solar_os_uart_read(data, len, timeout_ms, &read_len));
    lua_pushlstring(L, (const char *)data, read_len);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
static int solua_audio_status(lua_State *L)
{
    solar_os_audio_status_t status;
    solar_os_audio_get_status(&status);
    solua_push_audio_status(L, &status);
    return 1;
}

static int solua_audio_deinit(lua_State *L)
{
    (void)L;
    solar_os_audio_deinit();
    return 0;
}

static int solua_audio_set_volume(lua_State *L)
{
    return solua_check_esp(L, solar_os_audio_set_volume(solua_check_u8(L, 1)));
}

static int solua_audio_set_mic_gain(lua_State *L)
{
    return solua_check_esp(L, solar_os_audio_set_mic_gain((float)luaL_checknumber(L, 1)));
}

static int solua_audio_tone(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_audio_play_tone(solua_check_u32(L, 1),
                                                    solua_check_u32(L, 2),
                                                    solua_optional_u8(L,
                                                                      3,
                                                                      SOLAR_OS_AUDIO_VOLUME_GLOBAL)));
}

static int solua_audio_level(lua_State *L)
{
    solar_os_audio_level_t level;
    (void)solua_check_esp(L,
                          solar_os_audio_measure_level(solua_check_u32(L, 1), &level));

    lua_newtable(L);
    solua_set_int(L, -1, "samples", level.samples);
    solua_set_int(L, -1, "peak_percent", level.peak_percent);
    solua_set_int(L, -1, "average_percent", level.average_percent);
    return 1;
}

static int solua_audio_loopback(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_audio_loopback(solua_check_u32(L, 1),
                                                   solua_optional_u8(L,
                                                                     2,
                                                                     SOLAR_OS_AUDIO_VOLUME_GLOBAL)));
}

static int solua_audio_wav_info(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));

    solar_os_audio_wav_info_t info;
    (void)solua_check_esp(L, solar_os_audio_get_wav_info(path, &info));
    solua_push_wav_info(L, &info);
    return 1;
}

static int solua_audio_record_wav(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));

    solar_os_audio_wav_info_t info;
    const solar_os_audio_wav_options_t options = {
        .should_cancel = solua_should_cancel,
        .progress = NULL,
        .user = NULL,
        .progress_interval_ms = SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS,
    };
    (void)solua_check_esp(L,
                          solar_os_audio_record_wav(path,
                                                    solua_check_u32(L, 2),
                                                    &options,
                                                    &info));
    solua_push_wav_info(L, &info);
    return 1;
}

static int solua_audio_play_wav(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));

    solar_os_audio_wav_info_t info;
    const solar_os_audio_wav_options_t options = {
        .should_cancel = solua_should_cancel,
        .progress = NULL,
        .user = NULL,
        .progress_interval_ms = SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS,
    };
    (void)solua_check_esp(L,
                          solar_os_audio_play_wav(path,
                                                  solua_optional_u8(L,
                                                                    2,
                                                                    SOLAR_OS_AUDIO_VOLUME_GLOBAL),
                                                  &options,
                                                  &info));
    solua_push_wav_info(L, &info);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE
static int solua_ble_status(lua_State *L)
{
    char status[96];
    solar_os_ble_keyboard_get_status(status, sizeof(status));
    lua_pushstring(L, status);
    return 1;
}

static int solua_ble_connected(lua_State *L)
{
    lua_pushboolean(L, solar_os_ble_keyboard_is_connected());
    return 1;
}

static int solua_ble_pair(lua_State *L)
{
    return solua_check_esp(L, solar_os_ble_keyboard_start_pairing());
}

static int solua_ble_forget(lua_State *L)
{
    return solua_check_esp(L, solar_os_ble_keyboard_forget());
}

static int solua_ble_layout(lua_State *L)
{
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        lua_pushstring(L, solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout()));
        return 1;
    }

    solar_os_ble_keyboard_layout_t layout;
    if (!solar_os_ble_keyboard_parse_layout(luaL_checkstring(L, 1), &layout)) {
        return luaL_error(L, "expected us or de");
    }
    return solua_check_esp(L, solar_os_ble_keyboard_set_layout(layout));
}

static int solua_ble_read(lua_State *L)
{
    const uint32_t len = solua_optional_u32(L, 1, 64);
    if (len == 0 || len > 256) {
        return luaL_error(L, "expected length 1..256");
    }

    char buffer[256];
    const size_t read_len = solar_os_ble_keyboard_read_chars(buffer, len);
    lua_pushlstring(L, buffer, read_len);
    return 1;
}
#endif

static int solua_clipboard_set(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    return solua_check_esp(L, solar_os_clipboard_set(data, len));
}

static int solua_clipboard_get(lua_State *L)
{
    size_t len = 0;
    const char *data = solar_os_clipboard_data(&len);
    lua_pushlstring(L, data != NULL ? data : "", data != NULL ? len : 0);
    return 1;
}

static int solua_clipboard_size(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)solar_os_clipboard_size());
    return 1;
}

static int solua_clipboard_clear(lua_State *L)
{
    (void)L;
    solar_os_clipboard_clear();
    return 0;
}

static int solua_identity_user(lua_State *L)
{
    char buffer[SOLAR_OS_IDENTITY_USER_MAX + 1];
    solar_os_identity_get_user(buffer, sizeof(buffer));
    lua_pushstring(L, buffer);
    return 1;
}

static int solua_identity_hostname(lua_State *L)
{
    char buffer[SOLAR_OS_IDENTITY_HOSTNAME_MAX + 1];
    solar_os_identity_get_hostname(buffer, sizeof(buffer));
    lua_pushstring(L, buffer);
    return 1;
}

static int solua_identity_format(lua_State *L)
{
    char buffer[SOLAR_OS_IDENTITY_USER_MAX + SOLAR_OS_IDENTITY_HOSTNAME_MAX + 2];
    solar_os_identity_format(buffer, sizeof(buffer));
    lua_pushstring(L, buffer);
    return 1;
}

#if SOLAR_OS_PACKAGE_SERVICE_NET
static int solua_net_ping(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    const solar_os_net_ping_options_t options = {
        .count = solua_optional_u32(L, 2, 4),
        .timeout_ms = solua_optional_u32(L, 3, 1000),
        .interval_ms = solua_optional_u32(L, 4, 1000),
        .data_size = solua_optional_u32(L, 5, 32),
    };
    solar_os_net_ping_result_t result;
    (void)solua_check_esp(L,
                          solar_os_net_ping(host,
                                            &options,
                                            NULL,
                                            NULL,
                                            solua_should_cancel,
                                            NULL,
                                            &result));

    lua_newtable(L);
    solua_set_str(L, -1, "resolved_ip", result.resolved_ip);
    solua_set_bool(L, -1, "interrupted", result.interrupted);
    solua_set_int(L, -1, "transmitted", result.transmitted);
    solua_set_int(L, -1, "received", result.received);
    solua_set_int(L, -1, "loss_percent", result.loss_percent);
    solua_set_int(L, -1, "total_time_ms", result.total_time_ms);
    solua_set_int(L, -1, "min_time_ms", result.min_time_ms);
    solua_set_int(L, -1, "avg_time_ms", result.avg_time_ms);
    solua_set_int(L, -1, "max_time_ms", result.max_time_ms);
    return 1;
}

#endif

#if SOLAR_OS_PACKAGE_SERVICE_SSH
static int solua_ssh_keys_default_paths(lua_State *L)
{
    char private_path[SOLAR_OS_STORAGE_PATH_MAX];
    char public_path[SOLAR_OS_STORAGE_PATH_MAX];
    (void)solua_check_esp(L,
                          solar_os_ssh_keys_default_paths(private_path,
                                                          sizeof(private_path),
                                                          public_path,
                                                          sizeof(public_path)));

    lua_newtable(L);
    solua_set_str(L, -1, "private", private_path);
    solua_set_str(L, -1, "public", public_path);
    return 1;
}

static int solua_ssh_keys_default_exists(lua_State *L)
{
    lua_pushboolean(L, solar_os_ssh_keys_default_exists());
    return 1;
}

static int solua_ssh_keys_status(lua_State *L)
{
    solar_os_ssh_key_status_t status;
    (void)solua_check_esp(L, solar_os_ssh_keys_get_status(&status));
    solua_push_ssh_key_status(L, &status);
    return 1;
}

static int solua_ssh_keys_generate(lua_State *L)
{
    const uint32_t bits = solua_optional_u32(L, 1, SOLAR_OS_SSH_KEY_DEFAULT_BITS);
    const bool overwrite = lua_isnoneornil(L, 2) ? false : lua_toboolean(L, 2);
    return solua_check_esp(L, solar_os_ssh_keys_generate_rsa(bits, overwrite));
}

static int solua_ssh_keys_remove(lua_State *L)
{
    return solua_check_esp(L, solar_os_ssh_keys_remove_default());
}
#endif

static int solua_jobs_list(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    const size_t count = solar_os_jobs_count();
    int out = 1;
    for (size_t i = 0; i < count; i++) {
        solar_os_job_status_t status;
        if (solar_os_jobs_get(i, &status)) {
            solua_push_job_status(L, &status);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_jobs_count(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)solar_os_jobs_count());
    return 1;
}

static int solua_jobs_status(lua_State *L)
{
    solar_os_job_status_t status;
    if (!solar_os_jobs_get_by_name(luaL_checkstring(L, 1), &status)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_job_status(L, &status);
    return 1;
}

static int solua_jobs_start(lua_State *L)
{
    if (solua.ctx == NULL) {
        return solua_check_esp(L, ESP_ERR_INVALID_STATE);
    }

    const char *name = luaL_checkstring(L, 1);
    int argc = 0;
    char arg_storage[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char *argv[SOLAR_OS_APP_ARG_MAX];

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        const size_t count = lua_rawlen(L, 2);
        if (count > SOLAR_OS_APP_ARG_MAX) {
            return luaL_error(L, "too many job arguments");
        }
        argc = (int)count;
        for (size_t i = 0; i < count; i++) {
            lua_rawgeti(L, 2, (lua_Integer)i + 1);
            const char *arg = luaL_checkstring(L, -1);
            if (strlen(arg) >= SOLAR_OS_APP_ARG_LEN) {
                lua_pop(L, 1);
                return luaL_error(L, "job argument too long");
            }
            strlcpy(arg_storage[i], arg, sizeof(arg_storage[i]));
            argv[i] = arg_storage[i];
            lua_pop(L, 1);
        }
    }

    return solua_check_esp(L, solar_os_jobs_start(solua.ctx, name, argc, argv));
}

static int solua_jobs_stop(lua_State *L)
{
    if (solua.ctx == NULL) {
        return solua_check_esp(L, ESP_ERR_INVALID_STATE);
    }
    return solua_check_esp(L, solar_os_jobs_stop(solua.ctx, luaL_checkstring(L, 1)));
}

static solar_os_shell_terminal_profile_t solua_terminal_profile_from_arg(lua_State *L, int index)
{
    solar_os_shell_terminal_profile_t profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO;
    if (lua_isnoneornil(L, index)) {
        return profile;
    }
    if (!solar_os_shell_parse_terminal_profile(luaL_checkstring(L, index), &profile)) {
        luaL_error(L, "expected terminal profile auto, vt100, ansi, or dumb");
    }
    return profile;
}

static void solua_apply_session_options_table(lua_State *L,
                                              int index,
                                              solar_os_port_shell_options_t *options)
{
    lua_getfield(L, index, "term");
    if (!lua_isnil(L, -1)) {
        options->terminal_profile = solua_terminal_profile_from_arg(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "cols");
    if (!lua_isnil(L, -1)) {
        options->cols = solua_check_u16_size(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "rows");
    if (!lua_isnil(L, -1)) {
        options->rows = solua_check_u16_size(L, -1);
    }
    lua_pop(L, 1);
}

static int solua_sessions_create_shell(lua_State *L)
{
    if (solua.ctx == NULL) {
        return solua_check_esp(L, ESP_ERR_INVALID_STATE);
    }

    const int argc = lua_gettop(L);
    if (argc < 1 || argc > 4) {
        return luaL_error(L, "usage: solaros.sessions.create_shell(port[, term[, cols, rows]])");
    }

    solar_os_port_shell_options_t options = {
        .terminal_profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO,
        .cols = 0,
        .rows = 0,
    };

    const char *port_name = luaL_checkstring(L, 1);
    if (argc >= 2 && !lua_isnoneornil(L, 2)) {
        if (lua_istable(L, 2)) {
            if (argc != 2) {
                return luaL_error(L, "options table must be the last argument");
            }
            solua_apply_session_options_table(L, 2, &options);
        } else {
            options.terminal_profile = solua_terminal_profile_from_arg(L, 2);
        }
    }
    if (argc >= 3 && !lua_isnoneornil(L, 3)) {
        options.cols = solua_check_u16_size(L, 3);
    }
    if (argc >= 4 && !lua_isnoneornil(L, 4)) {
        options.rows = solua_check_u16_size(L, 4);
    }

    uint8_t session_id = 0;
    const esp_err_t err = solar_os_port_shell_start_with_options(solua.ctx,
                                                                 port_name,
                                                                 &options,
                                                                 false,
                                                                 &session_id);
    if (err != ESP_OK) {
        return solua_check_esp(L, err);
    }

    lua_pushinteger(L, (lua_Integer)session_id);
    return 1;
}

static int solua_sessions_close(lua_State *L)
{
    return solua_check_esp(L, solar_os_sessions_close_any(solua_check_u8(L, 1), NULL));
}

static int solua_apps_list(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    const size_t count = solar_os_app_registry_count();
    int out = 1;
    for (size_t i = 0; i < count; i++) {
        const solar_os_app_registry_entry_t *entry = solar_os_app_registry_get(i);
        if (entry == NULL) {
            continue;
        }

        lua_newtable(L);
        solua_set_str(L, -1, "name", entry->name);
        solua_set_str(L, -1, "summary", entry->summary);
        lua_rawseti(L, list, out++);
    }
    return 1;
}

static int solua_apps_find(lua_State *L)
{
    const solar_os_app_registry_entry_t *entry =
        solar_os_app_registry_find(luaL_checkstring(L, 1));
    if (entry == NULL) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    solua_set_str(L, -1, "name", entry->name);
    solua_set_str(L, -1, "summary", entry->summary);
    return 1;
}

static solar_os_shell_io_t *solua_current_io(void)
{
    return solua.ctx != NULL ? solua_io(solua.ctx) : NULL;
}

static solar_os_gfx_t *solua_current_gfx(void)
{
    if (solua.claimed_gfx != NULL) {
        return solua.claimed_gfx;
    }
    return solua.ctx != NULL ? solar_os_context_gfx(solua.ctx) : NULL;
}

static const char *solua_gfx_owner(void)
{
    if (solua.gfx_owner[0] == '\0') {
        strlcpy(solua.gfx_owner, "lua", sizeof(solua.gfx_owner));
    }
    return solua.gfx_owner;
}

static void solua_gfx_release_target(void)
{
    if (solua.gfx_target[0] != '\0') {
        (void)solar_os_display_release(solua.gfx_target, solua_gfx_owner());
    }
    solua.claimed_gfx = NULL;
    solua.gfx_target[0] = '\0';
}

static void solua_gfx_release_target_name(const char *target)
{
    if (target == NULL || target[0] == '\0') {
        solua_gfx_release_target();
        return;
    }

    (void)solar_os_display_release(target, solua_gfx_owner());
    if (strcmp(solua.gfx_target, target) == 0) {
        solua.claimed_gfx = NULL;
        solua.gfx_target[0] = '\0';
    }
}

static void solua_ui_send_event(lua_State *L, const solua_event_t *event)
{
    if (!solua_send_event(event)) {
        luaL_error(L, "ui event queue stopped");
    }
}

static void solua_tui_send_simple(lua_State *L, solua_event_type_t type)
{
    const solua_event_t event = {
        .type = type,
    };
    solua_ui_send_event(L, &event);
}

static void solua_tui_send_write(lua_State *L, const char *text, size_t len, uint8_t attr)
{
    while (len > 0) {
        solua_event_t event = {
            .type = SOLUA_EVENT_TUI_WRITE,
            .attr = attr,
        };
        event.data_len = len > sizeof(event.data) - 1 ? sizeof(event.data) - 1 : len;
        memcpy(event.data, text, event.data_len);
        event.data[event.data_len] = '\0';
        solua_ui_send_event(L, &event);
        text += event.data_len;
        len -= event.data_len;
    }
}

static int solua_tui_rows(lua_State *L)
{
    solar_os_shell_io_t *io = solua_current_io();
    lua_pushinteger(L, io != NULL ? (lua_Integer)solar_os_shell_io_rows(io) : 0);
    return 1;
}

static int solua_tui_cols(lua_State *L)
{
    solar_os_shell_io_t *io = solua_current_io();
    lua_pushinteger(L, io != NULL ? (lua_Integer)solar_os_shell_io_cols(io) : 0);
    return 1;
}

static int solua_tui_size(lua_State *L)
{
    solar_os_shell_io_t *io = solua_current_io();
    lua_pushinteger(L, io != NULL ? (lua_Integer)solar_os_shell_io_rows(io) : 0);
    lua_pushinteger(L, io != NULL ? (lua_Integer)solar_os_shell_io_cols(io) : 0);
    return 2;
}

static int solua_tui_clear(lua_State *L)
{
    solua_tui_send_simple(L, SOLUA_EVENT_TUI_CLEAR);
    return 0;
}

static int solua_tui_refresh(lua_State *L)
{
    solua_tui_send_simple(L, SOLUA_EVENT_TUI_REFRESH);
    return 0;
}

static int solua_tui_move(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_TUI_MOVE,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_tui_write(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    solua_tui_send_write(L, text, len, solua_optional_tui_attr(L, 2));
    return 0;
}

static int solua_tui_addstr(lua_State *L)
{
    const solua_event_t move = {
        .type = SOLUA_EVENT_TUI_MOVE,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
    };
    solua_ui_send_event(L, &move);

    size_t len = 0;
    const char *text = luaL_checklstring(L, 3, &len);
    solua_tui_send_write(L, text, len, solua_optional_tui_attr(L, 4));
    return 0;
}

static int solua_tui_putch(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_TUI_PUTCH,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
        .codepoint = solua_codepoint_from_arg(L, 3),
        .attr = solua_optional_tui_attr(L, 4),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_tui_hline(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_TUI_HLINE,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
        .width = solua_check_u16_size(L, 3),
        .attr = solua_optional_tui_attr(L, 4),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_tui_vline(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_TUI_VLINE,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
        .height = solua_check_u16_size(L, 3),
        .attr = solua_optional_tui_attr(L, 4),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_tui_vrule(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_TUI_VRULE,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
        .height = solua_check_u16_size(L, 3),
        .width = lua_isnoneornil(L, 4) ? 1 : solua_check_u16_size(L, 4),
        .attr = solua_optional_tui_attr(L, 5),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_tui_box(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_TUI_BOX,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
        .height = solua_check_u16_size(L, 3),
        .width = solua_check_u16_size(L, 4),
        .attr = solua_optional_tui_attr(L, 5),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_tui_fill(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_TUI_FILL,
        .row = solua_check_u16_size(L, 1),
        .col = solua_check_u16_size(L, 2),
        .height = solua_check_u16_size(L, 3),
        .width = solua_check_u16_size(L, 4),
        .codepoint = lua_isnoneornil(L, 5) ? ' ' : solua_codepoint_from_arg(L, 5),
        .attr = solua_optional_tui_attr(L, 6),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_tui_getch(lua_State *L)
{
    const uint32_t timeout_ms = solua_optional_u32(L, 1, 0);
    if (solua.key_input == NULL) {
        lua_pushnil(L);
        return 1;
    }

    char ch = 0;
    if (timeout_ms == 0) {
        if (xQueueReceive(solua.key_input, &ch, 0) == pdPASS) {
            lua_pushinteger(L, (uint8_t)ch);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (!solua.stop_requested && (xTaskGetTickCount() - start) <= timeout_ticks) {
        if (xQueueReceive(solua.key_input, &ch, pdMS_TO_TICKS(20)) == pdPASS) {
            lua_pushinteger(L, (uint8_t)ch);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

static void solua_gfx_send_simple(lua_State *L, solua_event_type_t type)
{
    const solua_event_t event = {
        .type = type,
    };
    solua_ui_send_event(L, &event);
}

static int solua_gfx_begin(lua_State *L)
{
    const char *target = NULL;
    if (!lua_isnoneornil(L, 1)) {
        target = luaL_checkstring(L, 1);
    }

    if (target == NULL || target[0] == '\0') {
        solua_gfx_release_target();
    } else if (strcmp(solua.gfx_target, target) != 0) {
        solar_os_gfx_t *gfx = NULL;
        char busy_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
        const esp_err_t err =
            solar_os_display_open_gfx(target,
                                      solua_gfx_owner(),
                                      &gfx,
                                      busy_owner,
                                      sizeof(busy_owner));
        if (err == ESP_ERR_INVALID_STATE && busy_owner[0] != '\0') {
            return luaL_error(L, "%s owned by %s", target, busy_owner);
        }
        if (err != ESP_OK) {
            return solua_check_esp(L, err);
        }
        solua_gfx_release_target();
        solua.claimed_gfx = gfx;
        strlcpy(solua.gfx_target, target, sizeof(solua.gfx_target));
    }

    solua_gfx_send_simple(L, SOLUA_EVENT_GFX_BEGIN);
    return 0;
}

static int solua_gfx_end(lua_State *L)
{
    solua_event_t event = {
        .type = SOLUA_EVENT_GFX_END,
    };
    if (solua.gfx_target[0] != '\0') {
        strlcpy(event.data, solua.gfx_target, sizeof(event.data));
        event.data_len = strlen(event.data);
    }
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_width(lua_State *L)
{
    solar_os_gfx_t *gfx = solua_current_gfx();
    lua_pushinteger(L, gfx != NULL ? (lua_Integer)solar_os_gfx_width(gfx) : 0);
    return 1;
}

static int solua_gfx_height(lua_State *L)
{
    solar_os_gfx_t *gfx = solua_current_gfx();
    lua_pushinteger(L, gfx != NULL ? (lua_Integer)solar_os_gfx_height(gfx) : 0);
    return 1;
}

static int solua_gfx_size(lua_State *L)
{
    solar_os_gfx_t *gfx = solua_current_gfx();
    lua_pushinteger(L, gfx != NULL ? (lua_Integer)solar_os_gfx_width(gfx) : 0);
    lua_pushinteger(L, gfx != NULL ? (lua_Integer)solar_os_gfx_height(gfx) : 0);
    return 2;
}

static int solua_gfx_clear(lua_State *L)
{
    const solar_os_gfx_color_t color =
        lua_isnoneornil(L, 1) ? SOLAR_OS_GFX_COLOR_WHITE : solua_gfx_color_from_arg(L, 1);
    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_CLEAR,
        .attr = (uint8_t)color,
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_color(lua_State *L)
{
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        solar_os_gfx_t *gfx = solua_current_gfx();
        lua_pushinteger(L, gfx != NULL ? solar_os_gfx_color(gfx) : SOLAR_OS_GFX_COLOR_BLACK);
        return 1;
    }

    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_COLOR,
        .attr = (uint8_t)solua_gfx_color_from_arg(L, 1),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_gray(lua_State *L)
{
    lua_Integer level = luaL_checkinteger(L, 1);
    if (level < 0) {
        level = 0;
    } else if (level > SOLAR_OS_GFX_GRAY_MAX) {
        level = SOLAR_OS_GFX_GRAY_MAX;
    }
    lua_pushinteger(L, solar_os_gfx_gray((uint8_t)level));
    return 1;
}

static int solua_gfx_font(lua_State *L)
{
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        solar_os_gfx_t *gfx = solua_current_gfx();
        lua_pushinteger(L, gfx != NULL ? solar_os_gfx_font(gfx) : SOLAR_OS_GFX_FONT_MONO);
        return 1;
    }

    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_FONT,
        .attr = (uint8_t)solua_gfx_font_from_arg(L, 1),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_present(lua_State *L)
{
    solua_gfx_send_simple(L, SOLUA_EVENT_GFX_PRESENT);
    return 0;
}

static int solua_gfx_pixel(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_PIXEL,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_line(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_LINE,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
        .x1 = (int32_t)luaL_checkinteger(L, 3),
        .y1 = (int32_t)luaL_checkinteger(L, 4),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_rect(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_RECT,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
        .width = solua_check_u16_size(L, 3),
        .height = solua_check_u16_size(L, 4),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_fill_rect(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_FILL_RECT,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
        .width = solua_check_u16_size(L, 3),
        .height = solua_check_u16_size(L, 4),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_circle(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_CIRCLE,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
        .width = solua_check_u16_size(L, 3),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_fill_circle(lua_State *L)
{
    const solua_event_t event = {
        .type = SOLUA_EVENT_GFX_FILL_CIRCLE,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
        .width = solua_check_u16_size(L, 3),
    };
    solua_ui_send_event(L, &event);
    return 0;
}

static int solua_gfx_text(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 3, &len);
    if (len >= SOLUA_EVENT_DATA_MAX) {
        return luaL_error(L, "text too long");
    }

    solua_event_t event = {
        .type = SOLUA_EVENT_GFX_TEXT,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
        .data_len = len,
    };
    memcpy(event.data, text, len);
    event.data[len] = '\0';
    solua_ui_send_event(L, &event);
    return 0;
}

static void solua_new_submodule(lua_State *L, int parent, const char *name)
{
    parent = lua_absindex(L, parent);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, parent, name);
}

static int solua_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    if (strcmp(name, "solaros") == 0) {
        lua_getglobal(L, "solaros");
        if (!lua_isnil(L, -1)) {
            return 1;
        }
    }
    return luaL_error(L, "module '%s' not found", name);
}

static void solua_open_solaros(lua_State *L)
{
    lua_newtable(L);
    const int solaros = lua_gettop(L);
    solua_set_func(L, solaros, "write", solua_solaros_write);
    solua_set_func(L, solaros, "version", solua_solaros_version);
    solua_set_func(L, solaros, "should_exit", solua_solaros_should_exit);
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solua_set_func(L, solaros, "battery_status", solua_solaros_battery_status);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solua_set_func(L, solaros, "wifi_status", solua_solaros_wifi_status_short);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    solua_set_func(L, solaros, "environment", solua_solaros_environment);
#endif

    solua_new_submodule(L, solaros, "storage");
    int mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_storage_status);
    solua_set_func(L, mod, "is_mounted", solua_storage_is_mounted);
    solua_set_func(L, mod, "mount", solua_storage_mount);
    solua_set_func(L, mod, "unmount", solua_storage_unmount);
    solua_set_func(L, mod, "mount_point", solua_storage_mount_point);
    solua_set_func(L, mod, "usage", solua_storage_usage);
    solua_set_func(L, mod, "resolve", solua_storage_resolve);
    solua_set_func(L, mod, "rescan", solua_storage_rescan);
    solua_set_func(L, mod, "blocks", solua_storage_blocks);
    solua_set_func(L, mod, "block_count", solua_storage_block_count);
    solua_set_func(L, mod, "block", solua_storage_block);
    solua_set_func(L, mod, "usage_for_block", solua_storage_usage_for_block);
    solua_set_func(L, mod, "mkdir", solua_storage_mkdir);
    solua_set_func(L, mod, "rmdir", solua_storage_rmdir);
    solua_set_func(L, mod, "remove", solua_storage_remove);
    solua_set_func(L, mod, "rename", solua_storage_rename);
    solua_set_func(L, mod, "copy", solua_storage_copy);
    solua_set_func(L, mod, "mount_volume", solua_storage_mount_volume);
    solua_set_func(L, mod, "unmount_volume", solua_storage_unmount_volume);
    lua_pop(L, 1);

    solua_new_submodule(L, solaros, "time");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "uptime_ms", solua_time_uptime_ms);
    solua_set_func(L, mod, "uptime", solua_time_uptime);
    solua_set_func(L, mod, "datetime", solua_time_datetime);
    solua_set_func(L, mod, "utc_datetime", solua_time_utc_datetime);
    solua_set_func(L, mod, "set_datetime", solua_time_set_datetime);
    solua_set_func(L, mod, "set_utc_datetime", solua_time_set_utc_datetime);
    solua_set_func(L, mod, "utc_to_local", solua_time_utc_to_local);
    solua_set_func(L, mod, "local_to_utc", solua_time_local_to_utc);
    solua_set_func(L, mod, "is_valid", solua_time_is_valid);
    solua_set_func(L, mod, "timezone", solua_time_timezone);
    solua_set_func(L, mod, "set_timezone", solua_time_set_timezone);
    solua_set_func(L, mod, "ntp_sync", solua_time_ntp_sync);
    lua_pop(L, 1);

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solua_new_submodule(L, solaros, "battery");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_solaros_battery_status);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    solua_new_submodule(L, solaros, "sensors");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "environment", solua_sensors_environment);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solua_new_submodule(L, solaros, "wifi");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_wifi_status);
    solua_set_func(L, mod, "status_text", solua_wifi_status_text);
    solua_set_func(L, mod, "start", solua_wifi_start);
    solua_set_func(L, mod, "stop", solua_wifi_stop);
    solua_set_func(L, mod, "connect", solua_wifi_connect);
    solua_set_func(L, mod, "connect_saved", solua_wifi_connect_saved);
    solua_set_func(L, mod, "disconnect", solua_wifi_disconnect);
    solua_set_func(L, mod, "forget", solua_wifi_forget);
    solua_set_func(L, mod, "forget_ssid", solua_wifi_forget_ssid);
    solua_set_func(L, mod, "forget_all", solua_wifi_forget_all);
    solua_set_func(L, mod, "known", solua_wifi_known);
    solua_set_func(L, mod, "scan", solua_wifi_scan);
    solua_set_func(L, mod, "ap_start", solua_wifi_ap_start);
    solua_set_func(L, mod, "ap_stop", solua_wifi_ap_stop);
    solua_set_func(L, mod, "nat", solua_wifi_nat);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MQTT
    solua_new_submodule(L, solaros, "mqtt");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_mqtt_status);
    solua_set_func(L, mod, "connect", solua_mqtt_connect);
    solua_set_func(L, mod, "disconnect", solua_mqtt_disconnect);
    solua_set_func(L, mod, "publish", solua_mqtt_publish);
    solua_set_func(L, mod, "subscribe", solua_mqtt_subscribe);
    solua_set_func(L, mod, "read", solua_mqtt_read);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    solua_new_submodule(L, solaros, "gpio");
    mod = lua_gettop(L);
    solua_set_int(L, mod, "INPUT", SOLAR_OS_GPIO_MODE_INPUT);
    solua_set_int(L, mod, "OUTPUT", SOLAR_OS_GPIO_MODE_OUTPUT);
    solua_set_int(L, mod, "PULL_NONE", SOLAR_OS_GPIO_PULL_NONE);
    solua_set_int(L, mod, "PULL_UP", SOLAR_OS_GPIO_PULL_UP);
    solua_set_int(L, mod, "PULL_DOWN", SOLAR_OS_GPIO_PULL_DOWN);
    solua_set_func(L, mod, "pins", solua_gpio_pins);
    solua_set_func(L, mod, "allowed", solua_gpio_allowed);
    solua_set_func(L, mod, "mode", solua_gpio_mode);
    solua_set_func(L, mod, "configure", solua_gpio_mode);
    solua_set_func(L, mod, "read", solua_gpio_read);
    solua_set_func(L, mod, "write", solua_gpio_write);
    solua_set_func(L, mod, "release", solua_gpio_release);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    solua_new_submodule(L, solaros, "onewire");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "allowed", solua_onewire_allowed);
    solua_set_func(L, mod, "reset", solua_onewire_reset);
    solua_set_func(L, mod, "scan", solua_onewire_scan);
    solua_set_func(L, mod, "xfer", solua_onewire_xfer);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    solua_new_submodule(L, solaros, "led");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_led_status);
    solua_set_func(L, mod, "set", solua_led_set);
    solua_set_func(L, mod, "on", solua_led_on);
    solua_set_func(L, mod, "off", solua_led_off);
    solua_set_func(L, mod, "toggle", solua_led_toggle);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
    solua_new_submodule(L, solaros, "adc");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "pins", solua_adc_pins);
    solua_set_func(L, mod, "read", solua_adc_read);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PWM
    solua_new_submodule(L, solaros, "pwm");
    mod = lua_gettop(L);
    solua_set_int(L, mod, "FREQ_MIN", SOLAR_OS_PWM_FREQ_MIN_HZ);
    solua_set_int(L, mod, "FREQ_MAX", SOLAR_OS_PWM_FREQ_MAX_HZ);
    solua_set_func(L, mod, "status", solua_pwm_status);
    solua_set_func(L, mod, "set", solua_pwm_set);
    solua_set_func(L, mod, "off", solua_pwm_off);
    lua_pop(L, 1);
#endif

#define SOLAR_OS_SCRIPT_API_STRINGIFY_INNER(value) #value
#define SOLAR_OS_SCRIPT_API_STRINGIFY(value) SOLAR_OS_SCRIPT_API_STRINGIFY_INNER(value)
#define SOLAR_OS_SCRIPT_API_MODULE_BEGIN(module_name) \
    { \
        solua_new_submodule(L, solaros, SOLAR_OS_SCRIPT_API_STRINGIFY(module_name)); \
        const int script_module = lua_gettop(L)
#define SOLAR_OS_SCRIPT_API_INT(module_name, public_name, value) \
    solua_set_int(L, script_module, SOLAR_OS_SCRIPT_API_STRINGIFY(public_name), value)
#define SOLAR_OS_SCRIPT_API_UINT(module_name, public_name, value) \
    solua_set_int(L, script_module, SOLAR_OS_SCRIPT_API_STRINGIFY(public_name), value)
#define SOLAR_OS_SCRIPT_API_FUNCTION(module_name, public_name, native_name) \
    solua_set_func(L, \
                   script_module, \
                   SOLAR_OS_SCRIPT_API_STRINGIFY(public_name), \
                   solua_##module_name##_##native_name)
#define SOLAR_OS_SCRIPT_API_MODULE_END(module_name) \
        lua_pop(L, 1); \
    }
#include "solar_os_script_bus_api.inc"
#undef SOLAR_OS_SCRIPT_API_MODULE_END
#undef SOLAR_OS_SCRIPT_API_FUNCTION
#undef SOLAR_OS_SCRIPT_API_UINT
#undef SOLAR_OS_SCRIPT_API_INT
#undef SOLAR_OS_SCRIPT_API_MODULE_BEGIN
#undef SOLAR_OS_SCRIPT_API_STRINGIFY
#undef SOLAR_OS_SCRIPT_API_STRINGIFY_INNER

#if SOLAR_OS_PACKAGE_SERVICE_I2C
    solua_new_submodule(L, solaros, "i2c");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "info", solua_i2c_info);
    solua_set_func(L, mod, "probe", solua_i2c_probe);
    solua_set_func(L, mod, "scan", solua_i2c_scan);
    solua_set_func(L, mod, "read_reg", solua_i2c_read_reg);
    solua_set_func(L, mod, "write_reg", solua_i2c_write_reg);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SPI
    solua_new_submodule(L, solaros, "spi");
    mod = lua_gettop(L);
    solua_set_int(L, mod, "MODE0", 0);
    solua_set_int(L, mod, "MODE1", 1);
    solua_set_int(L, mod, "MODE2", 2);
    solua_set_int(L, mod, "MODE3", 3);
    solua_set_int(L, mod, "DEFAULT_SPEED", SOLAR_OS_SPI_DEFAULT_SPEED_HZ);
    solua_set_int(L, mod, "MAX_SPEED", SOLAR_OS_SPI_MAX_SPEED_HZ);
    solua_set_func(L, mod, "status", solua_spi_status);
    solua_set_func(L, mod, "xfer", solua_spi_xfer);
    solua_set_func(L, mod, "read", solua_spi_read);
    solua_set_func(L, mod, "write", solua_spi_write);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UART
    solua_new_submodule(L, solaros, "uart");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_uart_status);
    solua_set_func(L, mod, "baud", solua_uart_baud);
    solua_set_func(L, mod, "is_valid_baud", solua_uart_is_valid_baud);
    solua_set_func(L, mod, "mode", solua_uart_mode);
    solua_set_func(L, mod, "write", solua_uart_write);
    solua_set_func(L, mod, "read", solua_uart_read);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solua_new_submodule(L, solaros, "audio");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_audio_status);
    solua_set_func(L, mod, "deinit", solua_audio_deinit);
    solua_set_func(L, mod, "off", solua_audio_deinit);
    solua_set_func(L, mod, "set_volume", solua_audio_set_volume);
    solua_set_func(L, mod, "set_mic_gain", solua_audio_set_mic_gain);
    solua_set_func(L, mod, "tone", solua_audio_tone);
    solua_set_func(L, mod, "level", solua_audio_level);
    solua_set_func(L, mod, "loopback", solua_audio_loopback);
    solua_set_func(L, mod, "wav_info", solua_audio_wav_info);
    solua_set_func(L, mod, "record_wav", solua_audio_record_wav);
    solua_set_func(L, mod, "play_wav", solua_audio_play_wav);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    solua_new_submodule(L, solaros, "ble");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "status", solua_ble_status);
    solua_set_func(L, mod, "connected", solua_ble_connected);
    solua_set_func(L, mod, "pair", solua_ble_pair);
    solua_set_func(L, mod, "forget", solua_ble_forget);
    solua_set_func(L, mod, "layout", solua_ble_layout);
    solua_set_func(L, mod, "read", solua_ble_read);
    lua_pop(L, 1);
#endif

    solua_new_submodule(L, solaros, "clipboard");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "set", solua_clipboard_set);
    solua_set_func(L, mod, "get", solua_clipboard_get);
    solua_set_func(L, mod, "size", solua_clipboard_size);
    solua_set_func(L, mod, "clear", solua_clipboard_clear);
    lua_pop(L, 1);

    solua_new_submodule(L, solaros, "identity");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "user", solua_identity_user);
    solua_set_func(L, mod, "hostname", solua_identity_hostname);
    solua_set_func(L, mod, "format", solua_identity_format);
    lua_pop(L, 1);

#if SOLAR_OS_PACKAGE_SERVICE_NET
    solua_new_submodule(L, solaros, "net");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "ping", solua_net_ping);
    lua_pop(L, 1);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SSH
    solua_new_submodule(L, solaros, "ssh_keys");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "default_paths", solua_ssh_keys_default_paths);
    solua_set_func(L, mod, "default_exists", solua_ssh_keys_default_exists);
    solua_set_func(L, mod, "status", solua_ssh_keys_status);
    solua_set_func(L, mod, "generate", solua_ssh_keys_generate);
    solua_set_func(L, mod, "remove", solua_ssh_keys_remove);
    lua_pop(L, 1);
#endif

    solua_new_submodule(L, solaros, "jobs");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "list", solua_jobs_list);
    solua_set_func(L, mod, "count", solua_jobs_count);
    solua_set_func(L, mod, "status", solua_jobs_status);
    solua_set_func(L, mod, "start", solua_jobs_start);
    solua_set_func(L, mod, "stop", solua_jobs_stop);
    lua_pop(L, 1);

    solua_new_submodule(L, solaros, "sessions");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "create_shell", solua_sessions_create_shell);
    solua_set_func(L, mod, "close", solua_sessions_close);
    lua_pop(L, 1);

    solua_new_submodule(L, solaros, "apps");
    mod = lua_gettop(L);
    solua_set_func(L, mod, "list", solua_apps_list);
    solua_set_func(L, mod, "find", solua_apps_find);
    lua_pop(L, 1);

    solua_new_submodule(L, solaros, "tui");
    mod = lua_gettop(L);
    solua_set_int(L, mod, "NORMAL", SOLAR_OS_TUI_ATTR_NORMAL);
    solua_set_int(L, mod, "BOLD", SOLAR_OS_TUI_ATTR_BOLD);
    solua_set_int(L, mod, "INVERSE", SOLAR_OS_TUI_ATTR_INVERSE);
    solua_set_int(L, mod, "ITALIC", SOLAR_OS_TUI_ATTR_ITALIC);
    solua_set_int(L, mod, "UNDERLINE", SOLAR_OS_TUI_ATTR_UNDERLINE);
    solua_set_int(L, mod, "KEY_UP", SOLAR_OS_KEY_UP);
    solua_set_int(L, mod, "KEY_DOWN", SOLAR_OS_KEY_DOWN);
    solua_set_int(L, mod, "KEY_LEFT", SOLAR_OS_KEY_LEFT);
    solua_set_int(L, mod, "KEY_RIGHT", SOLAR_OS_KEY_RIGHT);
    solua_set_int(L, mod, "KEY_HOME", SOLAR_OS_KEY_HOME);
    solua_set_int(L, mod, "KEY_END", SOLAR_OS_KEY_END);
    solua_set_int(L, mod, "KEY_DELETE", SOLAR_OS_KEY_DELETE);
    solua_set_int(L, mod, "KEY_ESCAPE", SOLAR_OS_KEY_ESCAPE);
    solua_set_int(L, mod, "KEY_CTRL", SOLAR_OS_KEY_CTRL);
    solua_set_int(L, mod, "KEY_AUDIO_MUTE_TOGGLE", SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE);
    solua_set_int(L, mod, "KEY_PAGE_UP", SOLAR_OS_KEY_PAGE_UP);
    solua_set_int(L, mod, "KEY_PAGE_DOWN", SOLAR_OS_KEY_PAGE_DOWN);
    solua_set_func(L, mod, "rows", solua_tui_rows);
    solua_set_func(L, mod, "cols", solua_tui_cols);
    solua_set_func(L, mod, "size", solua_tui_size);
    solua_set_func(L, mod, "clear", solua_tui_clear);
    solua_set_func(L, mod, "refresh", solua_tui_refresh);
    solua_set_func(L, mod, "move", solua_tui_move);
    solua_set_func(L, mod, "write", solua_tui_write);
    solua_set_func(L, mod, "addstr", solua_tui_addstr);
    solua_set_func(L, mod, "putch", solua_tui_putch);
    solua_set_func(L, mod, "hline", solua_tui_hline);
    solua_set_func(L, mod, "vline", solua_tui_vline);
    solua_set_func(L, mod, "vrule", solua_tui_vrule);
    solua_set_func(L, mod, "box", solua_tui_box);
    solua_set_func(L, mod, "fill", solua_tui_fill);
    solua_set_func(L, mod, "getch", solua_tui_getch);
    lua_pop(L, 1);

    solua_new_submodule(L, solaros, "gfx");
    mod = lua_gettop(L);
    solua_set_int(L, mod, "WHITE", SOLAR_OS_GFX_COLOR_WHITE);
    solua_set_int(L, mod, "LIGHT", SOLAR_OS_GFX_COLOR_LIGHT);
    solua_set_int(L, mod, "DARK", SOLAR_OS_GFX_COLOR_DARK);
    solua_set_int(L, mod, "BLACK", SOLAR_OS_GFX_COLOR_BLACK);
    solua_set_int(L, mod, "GRAY_MAX", SOLAR_OS_GFX_GRAY_MAX);
    solua_set_int(L, mod, "FONT_SMALL", SOLAR_OS_GFX_FONT_SMALL);
    solua_set_int(L, mod, "FONT_MONO", SOLAR_OS_GFX_FONT_MONO);
    solua_set_int(L, mod, "FONT_BOLD", SOLAR_OS_GFX_FONT_BOLD);
    solua_set_int(L, mod, "FONT_MONO_12", SOLAR_OS_GFX_FONT_MONO_12);
    solua_set_int(L, mod, "FONT_MONO_14", SOLAR_OS_GFX_FONT_MONO_14);
    solua_set_int(L, mod, "FONT_MONO_16", SOLAR_OS_GFX_FONT_MONO_16);
    solua_set_int(L, mod, "FONT_MONO_18", SOLAR_OS_GFX_FONT_MONO_18);
    solua_set_int(L, mod, "FONT_MONO_20", SOLAR_OS_GFX_FONT_MONO_20);
    solua_set_int(L, mod, "FONT_BOLD_12", SOLAR_OS_GFX_FONT_BOLD_12);
    solua_set_int(L, mod, "FONT_BOLD_14", SOLAR_OS_GFX_FONT_BOLD_14);
    solua_set_int(L, mod, "FONT_BOLD_16", SOLAR_OS_GFX_FONT_BOLD_16);
    solua_set_int(L, mod, "FONT_BOLD_18", SOLAR_OS_GFX_FONT_BOLD_18);
    solua_set_int(L, mod, "FONT_BOLD_20", SOLAR_OS_GFX_FONT_BOLD_20);
    solua_set_int(L, mod, "FONT_ITALIC_12", SOLAR_OS_GFX_FONT_ITALIC_12);
    solua_set_int(L, mod, "FONT_ITALIC_14", SOLAR_OS_GFX_FONT_ITALIC_14);
    solua_set_int(L, mod, "FONT_ITALIC_16", SOLAR_OS_GFX_FONT_ITALIC_16);
    solua_set_int(L, mod, "FONT_ITALIC_18", SOLAR_OS_GFX_FONT_ITALIC_18);
    solua_set_int(L, mod, "FONT_ITALIC_20", SOLAR_OS_GFX_FONT_ITALIC_20);
    solua_set_int(L, mod, "FONT_BOLD_ITALIC_12", SOLAR_OS_GFX_FONT_BOLD_ITALIC_12);
    solua_set_int(L, mod, "FONT_BOLD_ITALIC_14", SOLAR_OS_GFX_FONT_BOLD_ITALIC_14);
    solua_set_int(L, mod, "FONT_BOLD_ITALIC_16", SOLAR_OS_GFX_FONT_BOLD_ITALIC_16);
    solua_set_int(L, mod, "FONT_BOLD_ITALIC_18", SOLAR_OS_GFX_FONT_BOLD_ITALIC_18);
    solua_set_int(L, mod, "FONT_BOLD_ITALIC_20", SOLAR_OS_GFX_FONT_BOLD_ITALIC_20);
    solua_set_int(L, mod, "KEY_UP", SOLAR_OS_KEY_UP);
    solua_set_int(L, mod, "KEY_DOWN", SOLAR_OS_KEY_DOWN);
    solua_set_int(L, mod, "KEY_LEFT", SOLAR_OS_KEY_LEFT);
    solua_set_int(L, mod, "KEY_RIGHT", SOLAR_OS_KEY_RIGHT);
    solua_set_int(L, mod, "KEY_HOME", SOLAR_OS_KEY_HOME);
    solua_set_int(L, mod, "KEY_END", SOLAR_OS_KEY_END);
    solua_set_int(L, mod, "KEY_DELETE", SOLAR_OS_KEY_DELETE);
    solua_set_int(L, mod, "KEY_ESCAPE", SOLAR_OS_KEY_ESCAPE);
    solua_set_int(L, mod, "KEY_CTRL", SOLAR_OS_KEY_CTRL);
    solua_set_int(L, mod, "KEY_AUDIO_MUTE_TOGGLE", SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE);
    solua_set_int(L, mod, "KEY_PAGE_UP", SOLAR_OS_KEY_PAGE_UP);
    solua_set_int(L, mod, "KEY_PAGE_DOWN", SOLAR_OS_KEY_PAGE_DOWN);
    solua_set_func(L, mod, "begin", solua_gfx_begin);
    solua_set_func(L, mod, "end", solua_gfx_end);
    solua_set_func(L, mod, "width", solua_gfx_width);
    solua_set_func(L, mod, "height", solua_gfx_height);
    solua_set_func(L, mod, "size", solua_gfx_size);
    solua_set_func(L, mod, "clear", solua_gfx_clear);
    solua_set_func(L, mod, "gray", solua_gfx_gray);
    solua_set_func(L, mod, "color", solua_gfx_color);
    solua_set_func(L, mod, "set_color", solua_gfx_color);
    solua_set_func(L, mod, "font", solua_gfx_font);
    solua_set_func(L, mod, "set_font", solua_gfx_font);
    solua_set_func(L, mod, "present", solua_gfx_present);
    solua_set_func(L, mod, "refresh", solua_gfx_present);
    solua_set_func(L, mod, "pixel", solua_gfx_pixel);
    solua_set_func(L, mod, "line", solua_gfx_line);
    solua_set_func(L, mod, "rect", solua_gfx_rect);
    solua_set_func(L, mod, "fill_rect", solua_gfx_fill_rect);
    solua_set_func(L, mod, "circle", solua_gfx_circle);
    solua_set_func(L, mod, "fill_circle", solua_gfx_fill_circle);
    solua_set_func(L, mod, "text", solua_gfx_text);
    solua_set_func(L, mod, "getch", solua_tui_getch);
    lua_pop(L, 1);

    lua_pushvalue(L, solaros);
    lua_setglobal(L, "solaros");
    lua_pop(L, 1);

    lua_pushcfunction(L, solua_require);
    lua_setglobal(L, "require");
}

static bool solua_is_exit_error(const char *message)
{
    return message != NULL && strstr(message, SOLUA_EXIT_MARKER) != NULL;
}

static void solua_report_error(const char *message)
{
    if (solua.stop_requested || solua.interrupt_requested) {
        solua.interrupted = true;
        return;
    }
    solua_send_message(SOLUA_EVENT_ERROR, message != NULL ? message : "unknown error");
}

static bool solua_call_loaded(lua_State *L, bool print_results)
{
    const int base = lua_gettop(L) - 1;
    solua.vm_active = true;
    const int status = lua_pcall(L, 0, print_results ? LUA_MULTRET : 0, 0);
    solua.vm_active = false;
    if (status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        if (solua_is_exit_error(message)) {
            lua_pop(L, 1);
            return true;
        }
        solua_report_error(message);
        lua_pop(L, 1);
        return false;
    }

    if (print_results) {
        const int top = lua_gettop(L);
        for (int i = base + 1; i <= top; i++) {
            if (i > base + 1) {
                solua_send_cstr_output("\t");
            }
            size_t len = 0;
            const char *text = luaL_tolstring(L, i, &len);
            solua_send_output(text, len);
            lua_pop(L, 1);
        }
        if (top > base) {
            solua_send_cstr_output("\n");
        }
        lua_settop(L, base);
    }
    return true;
}

static int solua_load_repl_line(lua_State *L, const char *line)
{
    while (isspace((unsigned char)*line)) {
        line++;
    }

    const char *expr = line;
    if (*expr == '=') {
        expr++;
        while (isspace((unsigned char)*expr)) {
            expr++;
        }
    }

    char chunk[SOLUA_REPL_INPUT_MAX + 8];
    const int written = snprintf(chunk, sizeof(chunk), "return %s", expr);
    if (written > 0 && (size_t)written < sizeof(chunk)) {
        const int status = luaL_loadbufferx(L, chunk, strlen(chunk), "=stdin", "t");
        if (status == LUA_OK) {
            return status;
        }
        lua_pop(L, 1);
    }

    return luaL_loadbufferx(L, line, strlen(line), "=stdin", "t");
}

static void solua_set_args(lua_State *L)
{
    lua_newtable(L);
    for (int i = 0; i < solua.argc; i++) {
        lua_pushinteger(L, i);
        lua_pushstring(L, solua.argv[i]);
        lua_settable(L, -3);
    }
    lua_setglobal(L, "arg");
}

static bool solua_run_script(lua_State *L)
{
    solua_set_args(L);
    int status = luaL_loadfilex(L, solua.path, "t");
    if (status != LUA_OK) {
        solua_report_error(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return solua_call_loaded(L, false);
}

static void solua_run_repl(lua_State *L)
{
    solua_send_message(SOLUA_EVENT_PROMPT, "> ");
    while (!solua.stop_requested && !solua.repl_exit_requested) {
        solua_input_t input = {0};
        if (xQueueReceive(solua.input, &input, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (input.exit || solua.stop_requested) {
            break;
        }

        char *line = input.line;
        while (isspace((unsigned char)*line)) {
            line++;
        }
        if (*line == '\0') {
            solua_send_message(SOLUA_EVENT_PROMPT, "> ");
            continue;
        }

        solua.repl_executing = true;
        solua.interrupted = false;
        const int status = solua_load_repl_line(L, line);
        if (status == LUA_OK) {
            (void)solua_call_loaded(L, true);
        } else {
            solua_report_error(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        solua.interrupt_requested = false;
        solua.repl_executing = false;

        if (!solua.stop_requested && !solua.repl_exit_requested) {
            solua_send_message(SOLUA_EVENT_PROMPT, "> ");
        }
    }
}

static void solua_task(void *arg)
{
    (void)arg;

    SOLAR_OS_LOGI(TAG, "task start: mode=%s", solua.mode == SOLUA_MODE_REPL ? "repl" : "script");
    lua_State *L = lua_newstate(solua_alloc, NULL);
    bool success = false;
    if (L == NULL) {
        solua_send_message(SOLUA_EVENT_ERROR, "out of memory");
        goto done;
    }

    lua_atpanic(L, solua_panic);
    solua_open_libs(L);
    solua_open_solaros(L);
    lua_sethook(L, solua_hook, LUA_MASKCOUNT, SOLUA_HOOK_INSTRUCTION_COUNT);

    if (solua.mode == SOLUA_MODE_SCRIPT) {
        success = solua_run_script(L);
    } else {
        solua_run_repl(L);
        success = !solua.interrupted || solua.repl_exit_requested;
    }

done:
    if (L != NULL) {
        lua_close(L);
    }

    solua_event_t event = {
        .type = SOLUA_EVENT_DONE,
        .success = success,
    };
    (void)solua_send_event(&event);
    solua.task_done = true;
    solua.task = NULL;
    SOLAR_OS_LOGI(TAG, "task stop: success=%d", success);
    solar_os_task_delete(NULL);
}

static void solua_render_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage: lua [file.lua] [args...]");
    solar_os_shell_io_writeln(io, "  lua");
    solar_os_shell_io_writeln(io, "  lua hello.lua");
    solar_os_shell_io_writeln(io, "  lua /sdcard/apps/demo/main.lua arg");
}

static bool solua_path_has_suffix(const char *path, const char *suffix)
{
    const size_t path_len = path != NULL ? strlen(path) : 0;
    const size_t suffix_len = suffix != NULL ? strlen(suffix) : 0;
    return path_len >= suffix_len &&
        suffix_len > 0 &&
        strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool solua_display_io_hidden_by_gfx(solar_os_context_t *ctx, solar_os_shell_io_t *io)
{
    return solar_os_context_graphics_active(ctx) &&
        solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL;
}

static void solua_flush_io(solar_os_context_t *ctx, solar_os_shell_io_t *io)
{
    if (io == NULL || solua_display_io_hidden_by_gfx(ctx, io)) {
        return;
    }
    solar_os_shell_io_flush(io);
}

static void solua_finish_terminal_line(solar_os_context_t *ctx, solar_os_shell_io_t *io)
{
    if (io != NULL && solar_os_shell_io_cursor_col(io) != 0) {
        solar_os_shell_io_newline(io);
        solua_flush_io(ctx, io);
    }
}

static esp_err_t solua_start(solar_os_context_t *ctx)
{
    memset(&solua, 0, sizeof(solua));
    solua.ctx = ctx;

    solar_os_shell_io_t *io = solua_io(ctx);
    const int argc = solar_os_context_argc(ctx);
    if (argc > SOLAR_OS_APP_ARG_MAX) {
        solar_os_shell_io_writeln(io, "lua: too many arguments");
        solar_os_shell_io_flush(io);
        solua_return_to_shell(ctx);
        return ESP_OK;
    }

    const bool repl_mode = argc < 2;
    solua.mode = repl_mode ? SOLUA_MODE_REPL : SOLUA_MODE_SCRIPT;
    solua.argc = repl_mode ? 1 : argc - 1;
    strlcpy(solua.argv[0], repl_mode ? "lua" : solar_os_context_argv(ctx, 1), sizeof(solua.argv[0]));

    if (repl_mode) {
        solar_os_shell_io_clear(io);
        solar_os_shell_io_write_bold(io, LUA_RELEASE " on SolarOS");
        solar_os_shell_io_newline(io);
        solar_os_shell_io_writeln(io, "exit() returns to shell");
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
    } else {
        const char *script_arg = solar_os_context_argv(ctx, 1);
        if (script_arg == NULL || script_arg[0] == '\0') {
            solua_render_usage(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }

        esp_err_t path_err = solar_os_storage_resolve_path(script_arg,
                                                           solua.path,
                                                           sizeof(solua.path));
        if (path_err != ESP_OK) {
            solar_os_shell_io_printf(io, "lua: invalid path: %s\n", esp_err_to_name(path_err));
            solar_os_shell_io_flush(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }
        if (!solua_path_has_suffix(solua.path, ".lua")) {
            solar_os_shell_io_writeln(io, "lua: expected .lua file");
            solar_os_shell_io_flush(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }

        struct stat st;
        if (stat(solua.path, &st) != 0 || !S_ISREG(st.st_mode)) {
            solar_os_shell_io_printf(io, "lua: not found: %s\n", solua.path);
            solar_os_shell_io_flush(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }

        for (int i = 1; i < argc; i++) {
            strlcpy(solua.argv[i - 1],
                    solar_os_context_argv(ctx, i),
                    sizeof(solua.argv[i - 1]));
        }
        strlcpy(solua.argv[0], solua.path, sizeof(solua.argv[0]));
    }

    solua.events = solar_os_queue_create(SOLUA_EVENT_QUEUE_LEN,
                                          sizeof(solua_event_t));
    if (solua.events == NULL) {
        solar_os_shell_io_writeln(io, "lua: out of memory");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            solua_return_to_shell(ctx);
        }
        return ESP_OK;
    }

    solua.key_input = solar_os_queue_create(SOLUA_KEY_QUEUE_LEN, sizeof(char));
    if (solua.key_input == NULL) {
        solar_os_queue_delete(solua.events);
        solua.events = NULL;
        solar_os_shell_io_writeln(io, "lua: out of memory");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            solua_return_to_shell(ctx);
        }
        return ESP_OK;
    }

    if (repl_mode) {
        solua.input = solar_os_queue_create(SOLUA_INPUT_QUEUE_LEN,
                                             sizeof(solua_input_t));
        if (solua.input == NULL) {
            solar_os_queue_delete(solua.key_input);
            solua.key_input = NULL;
            solar_os_queue_delete(solua.events);
            solua.events = NULL;
            solar_os_shell_io_writeln(io, "lua: out of memory");
            solar_os_shell_io_flush(io);
            return ESP_OK;
        }
    }

    solua.running = true;
    const BaseType_t created = solar_os_task_create_pinned(
        solua_task,
        "solar_os_lua",
        SOLUA_TASK_STACK,
        NULL,
        SOLUA_TASK_PRIORITY,
        &solua.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        if (solua.input != NULL) {
            solar_os_queue_delete(solua.input);
            solua.input = NULL;
        }
        if (solua.key_input != NULL) {
            solar_os_queue_delete(solua.key_input);
            solua.key_input = NULL;
        }
        solar_os_queue_delete(solua.events);
        solua.events = NULL;
        solua.running = false;
        solar_os_shell_io_writeln(io, "lua: task create failed");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            solua_return_to_shell(ctx);
        }
    }

    return ESP_OK;
}

static void solua_interrupt_current(void)
{
    solua.interrupt_requested = true;
    solua.interrupted = true;
}

static void solua_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    solua.stop_requested = true;
    if (solua.input != NULL) {
        solua_input_t input = {
            .exit = true,
        };
        (void)xQueueSend(solua.input, &input, 0);
    }

    if (solua.task != NULL && !solua.task_done) {
        const TickType_t start = xTaskGetTickCount();
        while (solua.task != NULL &&
               !solua.task_done &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(SOLUA_STOP_WAIT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (solua.task != NULL && !solua.task_done) {
            SOLAR_OS_LOGW(TAG, "force stopping unresponsive Lua task");
            solar_os_task_delete(solua.task);
            solua.task = NULL;
            solua.task_done = true;
            solua.vm_active = false;
        }
    }

    if (solua.events != NULL) {
        solar_os_queue_delete(solua.events);
        solua.events = NULL;
    }
    if (solua.input != NULL) {
        solar_os_queue_delete(solua.input);
        solua.input = NULL;
    }
    if (solua.key_input != NULL) {
        solar_os_queue_delete(solua.key_input);
        solua.key_input = NULL;
    }
    solua_gfx_release_target();
}

static bool solua_is_printable_char(char ch)
{
    const unsigned char uch = (unsigned char)ch;
    return uch >= 0x20 && uch < 0x7f;
}

static size_t solua_repl_max_input_len(solar_os_context_t *ctx)
{
    (void)ctx;
    return sizeof(solua.repl_input) - 1;
}

static void solua_repl_render_input(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solua_io(ctx);
    solar_os_shell_io_clear_line_from(io, solua.repl_input_row, solua.repl_input_col);
    solar_os_shell_io_set_cursor(io, solua.repl_input_row, solua.repl_input_col);
    solar_os_shell_io_write_len(io, solua.repl_input, solua.repl_input_len);
    solar_os_shell_io_set_cursor(io,
                                 solua.repl_input_row,
                                 solua.repl_input_col + solua.repl_input_cursor);
    solar_os_shell_io_flush(io);
}

static void solua_repl_move_cursor_left(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor > 0) {
        solua.repl_input_cursor--;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_move_cursor_right(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor < solua.repl_input_len) {
        solua.repl_input_cursor++;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_move_cursor_home(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor != 0) {
        solua.repl_input_cursor = 0;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_move_cursor_end(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor != solua.repl_input_len) {
        solua.repl_input_cursor = solua.repl_input_len;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_insert_char(solar_os_context_t *ctx, char ch)
{
    if (solua.repl_input_len >= solua_repl_max_input_len(ctx)) {
        return;
    }
    memmove(&solua.repl_input[solua.repl_input_cursor + 1],
            &solua.repl_input[solua.repl_input_cursor],
            solua.repl_input_len - solua.repl_input_cursor + 1);
    solua.repl_input[solua.repl_input_cursor++] = ch;
    solua.repl_input_len++;
    solua_repl_render_input(ctx);
}

static void solua_repl_backspace(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor == 0) {
        return;
    }
    memmove(&solua.repl_input[solua.repl_input_cursor - 1],
            &solua.repl_input[solua.repl_input_cursor],
            solua.repl_input_len - solua.repl_input_cursor + 1);
    solua.repl_input_cursor--;
    solua.repl_input_len--;
    solua_repl_render_input(ctx);
}

static void solua_repl_delete(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor >= solua.repl_input_len) {
        return;
    }
    memmove(&solua.repl_input[solua.repl_input_cursor],
            &solua.repl_input[solua.repl_input_cursor + 1],
            solua.repl_input_len - solua.repl_input_cursor);
    solua.repl_input_len--;
    solua_repl_render_input(ctx);
}

static void solua_repl_submit(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solua_io(ctx);
    solar_os_shell_io_newline(io);
    solar_os_shell_io_flush(io);

    solua_input_t input = {0};
    strlcpy(input.line, solua.repl_input, sizeof(input.line));
    solua.repl_input_active = false;
    solua.repl_input_len = 0;
    solua.repl_input_cursor = 0;
    solua.repl_input[0] = '\0';

    if (solua.input == NULL || xQueueSend(solua.input, &input, 0) != pdPASS) {
        solar_os_shell_io_writeln(io, "lua: input queue full");
        solar_os_shell_io_flush(io);
        solua.repl_input_active = true;
    }
}

static void solua_apply_tui_event(solar_os_context_t *ctx, const solua_event_t *event)
{
    solar_os_tui_t tui;
    if (event == NULL || solar_os_tui_begin(&tui, ctx) != ESP_OK) {
        return;
    }

    switch (event->type) {
    case SOLUA_EVENT_TUI_CLEAR:
        solar_os_tui_clear(&tui);
        break;
    case SOLUA_EVENT_TUI_REFRESH:
        solar_os_tui_refresh(&tui);
        break;
    case SOLUA_EVENT_TUI_MOVE:
        solar_os_tui_move(&tui, event->row, event->col);
        break;
    case SOLUA_EVENT_TUI_WRITE:
        solar_os_tui_write(&tui, event->data, event->attr);
        break;
    case SOLUA_EVENT_TUI_PUTCH:
        solar_os_tui_putch(&tui, event->row, event->col, event->codepoint, event->attr);
        break;
    case SOLUA_EVENT_TUI_HLINE:
        solar_os_tui_hline(&tui, event->row, event->col, event->width, 0, event->attr);
        break;
    case SOLUA_EVENT_TUI_VLINE:
        solar_os_tui_vline(&tui, event->row, event->col, event->height, 0, event->attr);
        break;
    case SOLUA_EVENT_TUI_VRULE:
        solar_os_tui_vrule(&tui, event->row, event->col, event->height, event->width, event->attr);
        break;
    case SOLUA_EVENT_TUI_BOX:
        solar_os_tui_box(&tui, event->row, event->col, event->height, event->width, event->attr);
        break;
    case SOLUA_EVENT_TUI_FILL:
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

static void solua_apply_gfx_event(solar_os_context_t *ctx, const solua_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case SOLUA_EVENT_GFX_BEGIN:
        solar_os_context_set_graphics_active(ctx, true);
        return;
    case SOLUA_EVENT_GFX_END:
        solar_os_context_set_graphics_active(ctx, false);
        solua_gfx_release_target_name(event->data_len > 0 ? event->data : NULL);
        if (solar_os_context_terminal(ctx) != NULL) {
            solar_os_terminal_draw(solar_os_context_terminal(ctx));
        }
        return;
    default:
        break;
    }

    solar_os_gfx_t *gfx = solua_current_gfx();
    if (gfx == NULL) {
        return;
    }

    switch (event->type) {
    case SOLUA_EVENT_GFX_CLEAR:
        solar_os_gfx_clear(gfx, (solar_os_gfx_color_t)event->attr);
        break;
    case SOLUA_EVENT_GFX_COLOR:
        solar_os_gfx_set_color(gfx, (solar_os_gfx_color_t)event->attr);
        break;
    case SOLUA_EVENT_GFX_FONT:
        solar_os_gfx_set_font(gfx, (solar_os_gfx_font_t)event->attr);
        break;
    case SOLUA_EVENT_GFX_PRESENT:
        solar_os_gfx_present(gfx);
        break;
    case SOLUA_EVENT_GFX_PIXEL:
        solar_os_gfx_pixel(gfx, (int)event->x0, (int)event->y0);
        break;
    case SOLUA_EVENT_GFX_LINE:
        solar_os_gfx_line(gfx,
                          (int)event->x0,
                          (int)event->y0,
                          (int)event->x1,
                          (int)event->y1);
        break;
    case SOLUA_EVENT_GFX_RECT:
        solar_os_gfx_rect(gfx,
                          (int)event->x0,
                          (int)event->y0,
                          (int)event->width,
                          (int)event->height);
        break;
    case SOLUA_EVENT_GFX_FILL_RECT:
        solar_os_gfx_fill_rect(gfx,
                               (int)event->x0,
                               (int)event->y0,
                               (int)event->width,
                               (int)event->height);
        break;
    case SOLUA_EVENT_GFX_CIRCLE:
        solar_os_gfx_circle(gfx, (int)event->x0, (int)event->y0, (int)event->width);
        break;
    case SOLUA_EVENT_GFX_FILL_CIRCLE:
        solar_os_gfx_fill_circle(gfx, (int)event->x0, (int)event->y0, (int)event->width);
        break;
    case SOLUA_EVENT_GFX_TEXT:
        solar_os_gfx_text(gfx, (int)event->x0, (int)event->y0, event->data);
        break;
    default:
        break;
    }
}

static void solua_drain_events(solar_os_context_t *ctx)
{
    if (solua.events == NULL) {
        return;
    }

    solar_os_shell_io_t *io = solua_io(ctx);
    solua_event_t event;
    uint32_t drained = 0;
    while (drained++ < 24 && xQueueReceive(solua.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case SOLUA_EVENT_OUTPUT:
            for (size_t i = 0; i < event.data_len; i++) {
                solar_os_shell_io_put_utf8_byte(io, (uint8_t)event.data[i]);
            }
            break;
        case SOLUA_EVENT_ERROR:
            solar_os_shell_io_printf(io, "lua: %s\n", event.data);
            break;
        case SOLUA_EVENT_PROMPT:
            solua.repl_input_active = true;
            solua.repl_input_len = 0;
            solua.repl_input_cursor = 0;
            solua.repl_input[0] = '\0';
            solar_os_shell_io_write(io, event.data_len > 0 ? event.data : "> ");
            solua.repl_input_row = solar_os_shell_io_cursor_row(io);
            solua.repl_input_col = solar_os_shell_io_cursor_col(io);
            break;
        case SOLUA_EVENT_TUI_CLEAR:
        case SOLUA_EVENT_TUI_REFRESH:
        case SOLUA_EVENT_TUI_MOVE:
        case SOLUA_EVENT_TUI_WRITE:
        case SOLUA_EVENT_TUI_PUTCH:
        case SOLUA_EVENT_TUI_HLINE:
        case SOLUA_EVENT_TUI_VLINE:
        case SOLUA_EVENT_TUI_VRULE:
        case SOLUA_EVENT_TUI_BOX:
        case SOLUA_EVENT_TUI_FILL:
            solua_apply_tui_event(ctx, &event);
            break;
        case SOLUA_EVENT_GFX_BEGIN:
        case SOLUA_EVENT_GFX_END:
        case SOLUA_EVENT_GFX_CLEAR:
        case SOLUA_EVENT_GFX_COLOR:
        case SOLUA_EVENT_GFX_FONT:
        case SOLUA_EVENT_GFX_PRESENT:
        case SOLUA_EVENT_GFX_PIXEL:
        case SOLUA_EVENT_GFX_LINE:
        case SOLUA_EVENT_GFX_RECT:
        case SOLUA_EVENT_GFX_FILL_RECT:
        case SOLUA_EVENT_GFX_CIRCLE:
        case SOLUA_EVENT_GFX_FILL_CIRCLE:
        case SOLUA_EVENT_GFX_TEXT:
            solua_apply_gfx_event(ctx, &event);
            break;
        case SOLUA_EVENT_DONE:
            solua.running = false;
            solua.task_done = true;
            solua_gfx_release_target();
            solar_os_context_set_graphics_active(ctx, false);
            if (solua.mode == SOLUA_MODE_SCRIPT || solua.repl_exit_requested) {
                solua_finish_terminal_line(ctx, io);
                if (!event.success && !solua.interrupted) {
                    solar_os_shell_io_writeln(io, "lua: failed");
                } else if (!event.success) {
                    solar_os_shell_io_writeln(io, "lua: stopped");
                }
                solua_flush_io(ctx, io);
                solua_return_to_shell(ctx);
                break;
            }
            solar_os_shell_io_printf(io, "lua: %s\n", event.success ? "done" : "stopped");
            solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
            break;
        default:
            break;
        }
    }
    solua_flush_io(ctx, io);
}

static void solua_queue_script_key(char ch)
{
    if (solua.key_input != NULL && xQueueSend(solua.key_input, &ch, 0) != pdPASS) {
        SOLAR_OS_LOGW(TAG, "lua key queue full");
    }
}

static bool solua_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        solua_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        if (solua.mode == SOLUA_MODE_REPL && solua.repl_executing && !solua.interrupt_requested) {
            solar_os_shell_io_t *io = solua_io(ctx);
            solar_os_shell_io_writeln(io, "\nlua: interrupt");
            solar_os_shell_io_flush(io);
            solua_interrupt_current();
            return true;
        }
        if (solua.mode == SOLUA_MODE_SCRIPT && solua.running && !solua.stop_requested) {
            solar_os_shell_io_t *io = solua_io(ctx);
            solar_os_shell_io_writeln(io, "\nlua: interrupt");
            solar_os_shell_io_flush(io);
            solua.stop_requested = true;
        }
        solua_return_to_shell(ctx);
        return true;
    }

    if (solua.mode == SOLUA_MODE_SCRIPT || (solua.mode == SOLUA_MODE_REPL && solua.repl_executing)) {
        solua_queue_script_key((char)ch);
        return true;
    }

    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_t *term = solar_os_shell_io_terminal(solua_io(ctx));
        if (term != NULL) {
            solar_os_terminal_page_up(term);
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *term = solar_os_shell_io_terminal(solua_io(ctx));
        if (term != NULL) {
            solar_os_terminal_page_down(term);
        }
        return true;
    }
    if (solua.mode != SOLUA_MODE_REPL || !solua.repl_input_active) {
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_LEFT:
        solua_repl_move_cursor_left(ctx);
        break;
    case SOLAR_OS_KEY_RIGHT:
        solua_repl_move_cursor_right(ctx);
        break;
    case SOLAR_OS_KEY_HOME:
    case SOLAR_OS_KEY_CTRL_HOME:
        solua_repl_move_cursor_home(ctx);
        break;
    case SOLAR_OS_KEY_END:
    case SOLAR_OS_KEY_CTRL_END:
        solua_repl_move_cursor_end(ctx);
        break;
    case SOLAR_OS_KEY_DELETE:
        solua_repl_delete(ctx);
        break;
    case SOLAR_OS_KEY_ESCAPE:
        if (solua.repl_input_len > 0) {
            solua.repl_input_len = 0;
            solua.repl_input_cursor = 0;
            solua.repl_input[0] = '\0';
            solua_repl_render_input(ctx);
        }
        break;
    case '\r':
    case '\n':
        solua_repl_submit(ctx);
        break;
    case '\b':
        solua_repl_backspace(ctx);
        break;
    default:
        if (solua_is_printable_char((char)ch)) {
            solua_repl_insert_char(ctx, (char)ch);
        }
        break;
    }

    return true;
}

const solar_os_app_t solar_os_lua_app = {
    .name = "lua",
    .summary = "Lua runtime",
    .start = solua_start,
    .stop = solua_stop,
    .event = solua_event,
    .worker_stack_bytes = SOLUA_TASK_STACK,
};
