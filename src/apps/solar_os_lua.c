#include "solar_os_lua.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "solar_os_contacts.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_task.h"
#include "solar_os_rtc.h"
#include "solar_os_schedule.h"
#if SOLAR_OS_PACKAGE_SERVICE_ADC
#include "solar_os_adc.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
#include "solar_os_controls.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_DSP
#include "solar_os_dsp.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
#include "solar_os_synth_voice.h"
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
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
#include "solar_os_http_client.h"
#include "solar_os_http_stream.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_FTP
#include "solar_os_ftp.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HID
#include "solar_os_hid.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_I2C
#include "solar_os_i2c.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_expansion.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
#include "solar_os_neopixel.h"
#endif
#include "solar_os_identity.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#if SOLAR_OS_PACKAGE_SERVICE_MQTT
#include "solar_os_mqtt.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
#include "solar_os_midi.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
#include "solar_os_net.h"
#include "solar_os_net_session.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SSH
#include "solar_os_ssh_keys.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
#include "solar_os_onewire.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_OSC
#include "solar_os_osc.h"
#endif
#include "solar_os_port_shell.h"
#include "solar_os_pins.h"
#include "solar_os_queue.h"
#include "solar_os_scheduler.h"
#include "solar_os_script_lifecycle.h"
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
#include "solar_os_tui_widgets.h"
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
#define SOLUA_DRAIN_EVENTS_PER_TICK 24U
#define SOLUA_DRAIN_TUI_EVENTS_PER_TICK 128U
#define SOLUA_HOOK_INSTRUCTION_COUNT 10000
#define SOLUA_EXIT_MARKER "__solaros_lua_exit__"
#define SOLUA_SLEEP_MAX_MS (60U * 60U * 1000U)
#define SOLUA_HTTP_MAX_REQUEST_HEADERS 16U
#define SOLUA_HTTP_DEFAULT_TIMEOUT_MS 10000U
#define SOLUA_HTTP_READ_POLL_MS 100U
#define SOLUA_HTTP_MAX_BODY (256U * 1024U)
#define SOLUA_MIDI_READ_MAX_MS 60000U
#define SOLUA_DEVICE_INPUT_QUEUE_LEN 16U
#define SOLUA_DEVICE_INPUT_READ_MAX_MS 60000U

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
    SOLUA_EVENT_TUI_CELL,
    SOLUA_EVENT_TUI_TITLE,
    SOLUA_EVENT_TUI_HELP,
    SOLUA_EVENT_TUI_TAB,
    SOLUA_EVENT_TUI_INPUT,
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
    SOLUA_EVENT_GFX_BITMAP,
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
    uint32_t attr;
    char data[SOLUA_EVENT_DATA_MAX];
} solua_event_t;

typedef struct {
    bool exit;
    char line[SOLUA_REPL_INPUT_MAX];
} solua_input_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_terminal_t *session_terminal;
    solar_os_shell_io_t *session_io;
    solar_os_gfx_t *session_gfx;
    solar_os_tui_t tui;
    bool tui_active;
    solar_os_shell_io_t fallback_io;
    QueueHandle_t events;
    QueueHandle_t input;
    QueueHandle_t key_input;
    QueueHandle_t device_input;
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
    int exit_code;
    uint32_t device_input_dropped;
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

typedef enum {
    SOLUA_RUNTIME_OWNER_NONE,
    SOLUA_RUNTIME_OWNER_APP,
    SOLUA_RUNTIME_OWNER_RUNNER,
} solua_runtime_owner_t;

typedef struct {
    solua_state_t app;
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
    solar_os_midi_subscription_t midi_subscription;
    bool midi_subscribed;
#endif
} solua_cold_state_t;

static void *solua_state;
#define solua (((solua_cold_state_t *)solua_state)->app)
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
#define solua_midi_subscription \
    (((solua_cold_state_t *)solua_state)->midi_subscription)
#define solua_midi_subscribed \
    (((solua_cold_state_t *)solua_state)->midi_subscribed)
#endif
static solar_os_script_run_control_t *solua_runner_control;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("runtime ownership spinlock")
static portMUX_TYPE solua_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shared shell and Playground Lua runtime")
static solua_runtime_owner_t solua_runtime_owner;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shared shell and Playground Lua cadence")
static uint32_t solua_tick_interval_ms;
#if SOLAR_OS_PACKAGE_SERVICE_NET
static solar_os_net_session_t *solua_net_session;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
static solar_os_http_stream_session_t *solua_http_stream_session;
static solar_os_http_session_context_t *solua_http_session_context;
#endif

static bool solua_runtime_claim(solua_runtime_owner_t owner)
{
    bool claimed = false;
    portENTER_CRITICAL(&solua_runtime_lock);
    if (solua_runtime_owner == SOLUA_RUNTIME_OWNER_NONE) {
        solua_runtime_owner = owner;
        solua_tick_interval_ms = 0;
        claimed = true;
    }
    portEXIT_CRITICAL(&solua_runtime_lock);
    return claimed;
}

static void solua_runtime_release(solua_runtime_owner_t owner)
{
    portENTER_CRITICAL(&solua_runtime_lock);
    if (solua_runtime_owner == owner) {
        solua_runtime_owner = SOLUA_RUNTIME_OWNER_NONE;
        solua_tick_interval_ms = 0;
    }
    portEXIT_CRITICAL(&solua_runtime_lock);
}

static bool solua_runtime_is_owned_by(solua_runtime_owner_t owner)
{
    bool matches;
    portENTER_CRITICAL(&solua_runtime_lock);
    matches = solua_runtime_owner == owner;
    portEXIT_CRITICAL(&solua_runtime_lock);
    return matches;
}

static uint32_t solua_requested_tick_interval_ms(void)
{
    uint32_t interval_ms = 0;
    portENTER_CRITICAL(&solua_runtime_lock);
    if (solua_runtime_owner == SOLUA_RUNTIME_OWNER_APP) {
        interval_ms = solua_tick_interval_ms != 0 ?
            solua_tick_interval_ms : SOLAR_OS_TICK_INTERVAL_DEFAULT_MS;
    }
    portEXIT_CRITICAL(&solua_runtime_lock);
    return interval_ms;
}

static bool solua_set_tick_interval_ms(uint32_t interval_ms)
{
    bool set = false;
    portENTER_CRITICAL(&solua_runtime_lock);
    if (solua_runtime_owner == SOLUA_RUNTIME_OWNER_APP) {
        solua_tick_interval_ms = interval_ms;
        set = true;
    }
    portEXIT_CRITICAL(&solua_runtime_lock);
    return set;
}

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

static void solua_return_to_shell(solar_os_context_t *ctx,
                                  int exit_code,
                                  const char *message)
{
    const bool shared_port = solar_os_shell_io_kind(solua_io(ctx)) ==
        SOLAR_OS_SHELL_IO_KIND_PORT;
    solar_os_context_finish(ctx,
                                         exit_code,
                                         shared_port ? NULL : message);
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
    if (solua_runner_control != NULL) {
        if (type == SOLUA_EVENT_ERROR) {
            solar_os_script_run_error(solua_runner_control,
                                      ESP_FAIL,
                                      message != NULL ? message : "Lua error");
            if (message != NULL) {
                solar_os_script_run_output(solua_runner_control,
                                           message,
                                           strlen(message));
                solar_os_script_run_output(solua_runner_control, "\n", 1);
            }
        }
        return;
    }

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
    if (solua_runner_control != NULL) {
        solar_os_script_run_output(solua_runner_control, data, len);
        return;
    }

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
    if (solua_runner_control != NULL &&
        solar_os_script_run_should_cancel(solua_runner_control)) {
        solua.interrupt_requested = true;
        solua.interrupted = true;
    }
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
    solua.exit_code = (int)luaL_optinteger(L, 1, 0);
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
    if (value < 0 || (uint64_t)value > UINT32_MAX ||
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
    solua_set_bool(L, -1, "repeater_enabled", status->repeater_enabled);
    solua_set_bool(L, -1, "repeater_active", status->repeater_active);
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
    solua_set_int(L, -1, "repeater_learned_clients", status->repeater_learned_clients);
    solua_set_int(L, -1, "repeater_upstream_frames", status->repeater_upstream_frames);
    solua_set_int(L, -1, "repeater_downstream_frames", status->repeater_downstream_frames);
    solua_set_int(L, -1, "repeater_dropped_frames", status->repeater_dropped_frames);
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
    solua_set_int(L, -1, "worker_stack_bytes", status->worker_stack_bytes);
    solua_set_bool(L, -1, "worker_stack_external", status->worker_stack_external);
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
    return solua.stop_requested || solua.interrupt_requested ||
        (solua_runner_control != NULL &&
         solar_os_script_run_should_cancel(solua_runner_control));
}

#if SOLAR_OS_PACKAGE_SERVICE_NET
static solar_os_net_session_t *solua_net_get(lua_State *L)
{
    if (solua_net_session == NULL) {
        const char *owner = solua_runner_control != NULL ? "lua.runner" : "lua.app";
        (void)solua_check_esp(L,
                              solar_os_net_session_create(owner,
                                                          solua_should_cancel,
                                                          NULL,
                                                          &solua_net_session));
    }
    return solua_net_session;
}

static void solua_net_destroy(void)
{
    solar_os_net_session_destroy(solua_net_session);
    solua_net_session = NULL;
}

static uint16_t solua_net_port(lua_State *L, int index, bool allow_zero)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < (allow_zero ? 0 : 1) || value > UINT16_MAX) {
        luaL_error(L, allow_zero ? "expected port 0..65535" :
                                  "expected port 1..65535");
    }
    return (uint16_t)value;
}

static uint32_t solua_net_timeout(lua_State *L, int index, uint32_t fallback)
{
    const lua_Integer value = lua_isnoneornil(L, index) ? fallback :
        luaL_checkinteger(L, index);
    if (value < 0 || value > SOLAR_OS_NET_MAX_TIMEOUT_MS) {
        luaL_error(L, "timeout out of range");
    }
    return (uint32_t)value;
}

static size_t solua_net_receive_size(lua_State *L, int index)
{
    const lua_Integer value = lua_isnoneornil(L, index) ? 4096 :
        luaL_checkinteger(L, index);
    if (value <= 0 || value > SOLAR_OS_NET_MAX_TRANSFER_BYTES) {
        luaL_error(L, "receive size out of range");
    }
    return (size_t)value;
}

static uint8_t *solua_net_buffer(lua_State *L, size_t size)
{
    uint8_t *buffer = solar_os_memory_alloc(size,
                                            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                            "lua.net");
    if (buffer == NULL) {
        (void)solua_check_esp(L, ESP_ERR_NO_MEM);
    }
    return buffer;
}
#endif

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

static int solua_solaros_tick_interval(lua_State *L)
{
    const int argc = lua_gettop(L);
    if (argc > 1) {
        return luaL_error(L, "expected zero or one argument");
    }
    if (argc == 1) {
        const uint32_t interval_ms = solua_check_u32(L, 1);
        if (!solua_set_tick_interval_ms(interval_ms)) {
            return luaL_error(
                L,
                "tick interval requires foreground lua app");
        }
    }

    const uint32_t requested_ms = solua_requested_tick_interval_ms();
    lua_pushinteger(
        L,
        requested_ms != 0 ?
            requested_ms : SOLAR_OS_TICK_INTERVAL_DEFAULT_MS);
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
    solua_set_bool(L, -1, "repeater_enabled", status.repeater_enabled);
    solua_set_bool(L, -1, "repeater_active", status.repeater_active);
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

static int solua_storage_read_file(lua_State *L)
{
    const uint32_t max_bytes = solua_optional_u32(L, 2, 4096U);
    if (max_bytes == 0 || max_bytes > SOLAR_OS_STORAGE_READ_MAX_BYTES) {
        return luaL_error(L, "max_bytes must be 1..65536");
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));

    uint8_t *data = solar_os_memory_alloc(max_bytes,
                                           SOLAR_OS_MEMORY_TRANSIENT,
                                           "lua.storage");
    if (data == NULL) {
        return solua_check_esp(L, ESP_ERR_NO_MEM);
    }

    size_t read_len = 0;
    const esp_err_t err = solar_os_storage_read_file(path,
                                                      data,
                                                      max_bytes,
                                                      &read_len);
    if (err != ESP_OK) {
        solar_os_memory_free(data);
        return solua_check_esp(L, err);
    }

    lua_pushlstring(L, (const char *)data, read_len);
    solar_os_memory_free(data);
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

static int solua_time_sleep_ms(lua_State *L)
{
    const uint32_t duration_ms = solua_check_u32(L, 1);
    if (duration_ms > SOLUA_SLEEP_MAX_MS) {
        return luaL_error(L, "sleep limited to 3600000 ms");
    }
    if (duration_ms == 0) {
        taskYIELD();
        return 0;
    }

    const TickType_t started = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(duration_ms);
    if (duration_ticks == 0) {
        duration_ticks = 1;
    }
    while (!solua_should_cancel(NULL) &&
           (solua_runner_control == NULL ||
            !solar_os_script_run_should_cancel(solua_runner_control)) &&
           (xTaskGetTickCount() - started) < duration_ticks) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t remaining = duration_ticks - elapsed;
        const TickType_t slice = remaining < pdMS_TO_TICKS(20)
                                     ? remaining
                                     : pdMS_TO_TICKS(20);
        vTaskDelay(slice > 0 ? slice : 1);
    }
    if (solua_should_cancel(NULL) ||
        (solua_runner_control != NULL &&
         solar_os_script_run_should_cancel(solua_runner_control))) {
        solua.interrupt_requested = true;
        solua.interrupted = true;
        return luaL_error(L, "interrupted");
    }
    return 0;
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

static int solua_rtc_status(lua_State *L)
{
    solar_os_rtc_info_t info;
    const esp_err_t err = solar_os_rtc_get_info(&info);
    lua_newtable(L);
    solua_set_bool(L, -1, "available", err == ESP_OK);
    if (err == ESP_OK) {
        solua_set_str(L, -1, "provider", info.provider);
        solua_set_int(L, -1, "capabilities", info.capabilities);
        solua_set_int(L, -1, "interrupt_gpio", info.interrupt_gpio);
        solua_set_int(L, -1, "interrupt_active_level", info.interrupt_active_level);
        solua_set_str(L, -1, "alarm_owner", info.alarm_owner);
        solua_set_str(L, -1, "timer_owner", info.countdown_owner);
    }
    return 1;
}

static int solua_rtc_set_alarm(lua_State *L)
{
    const uint32_t hour = solua_check_u32(L, 1);
    const uint32_t minute = solua_check_u32(L, 2);
    const uint32_t second = solua_optional_u32(L, 3, 0);
    const uint32_t day = solua_optional_u32(L, 4, 0);
    const uint32_t weekday = solua_optional_u32(L, 5, 7);
    luaL_argcheck(L, hour <= 23 && minute <= 59 && second <= 59 &&
                  day <= 31 && weekday <= 7, 1, "invalid RTC alarm fields");
    solar_os_rtc_alarm_t alarm = {
        .match_fields = SOLAR_OS_RTC_ALARM_MATCH_SECOND |
            SOLAR_OS_RTC_ALARM_MATCH_MINUTE | SOLAR_OS_RTC_ALARM_MATCH_HOUR,
        .second = (uint8_t)second,
        .minute = (uint8_t)minute,
        .hour = (uint8_t)hour,
    };
    if (day != 0) {
        alarm.day = (uint8_t)day;
        alarm.match_fields |= SOLAR_OS_RTC_ALARM_MATCH_DAY;
    }
    if (weekday <= 6) {
        alarm.weekday = (uint8_t)weekday;
        alarm.match_fields |= SOLAR_OS_RTC_ALARM_MATCH_WEEKDAY;
    }
    return solua_check_esp(L, solar_os_rtc_set_alarm_for("lua", &alarm));
}

static int solua_rtc_clear_alarm(lua_State *L)
{
    (void)L;
    return solua_check_esp(L, solar_os_rtc_disable_alarm_for("lua"));
}

static int solua_rtc_set_timer(lua_State *L)
{
    return solua_check_esp(L, solar_os_rtc_set_countdown_for(
        "lua", solua_check_u32(L, 1), lua_toboolean(L, 2) != 0));
}

static int solua_rtc_clear_timer(lua_State *L)
{
    return solua_check_esp(L, solar_os_rtc_disable_countdown_for("lua"));
}

static int solua_rtc_pending(lua_State *L)
{
    uint32_t pending = 0;
    (void)solua_check_esp(L, solar_os_rtc_get_interrupt_status(&pending));
    lua_pushinteger(L, pending);
    return 1;
}

static int solua_rtc_ack(lua_State *L)
{
    return solua_check_esp(L, solar_os_rtc_clear_interrupt_status(solua_check_u32(L, 1)));
}

static solar_os_schedule_action_t solua_schedule_action(lua_State *L, int index)
{
    const char *action = solua_optional_str(L, index, "alarm");
    if (strcmp(action, "alarm") == 0) return SOLAR_OS_SCHEDULE_ACTION_ALARM;
    if (strcmp(action, "run") == 0) return SOLAR_OS_SCHEDULE_ACTION_SCRIPT;
    luaL_error(L, "action must be alarm or run");
    return SOLAR_OS_SCHEDULE_ACTION_ALARM;
}

static void solua_push_schedule_entry(lua_State *L,
                                      const solar_os_schedule_entry_t *entry)
{
    lua_newtable(L);
    solua_set_str(L, -1, "name", entry->name);
    solua_set_str(L, -1, "kind", solar_os_schedule_kind_name(entry->kind));
    solua_set_str(L, -1, "action", solar_os_schedule_action_name(entry->action));
    solua_set_str(L, -1, "value", entry->value);
    solua_set_bool(L, -1, "enabled", entry->enabled);
    solua_set_bool(L, -1, "persistent", entry->persistent);
    solua_set_int(L, -1, "interval_seconds", entry->interval_seconds);
    solua_set_int(L, -1, "at_utc_seconds", (lua_Integer)entry->at_utc_seconds);
    solua_set_int(L, -1, "hour", entry->hour);
    solua_set_int(L, -1, "minute", entry->minute);
    solua_set_int(L, -1, "second", entry->second);
    solua_set_int(L, -1, "weekdays", entry->weekdays);
    solua_set_int(L, -1, "run_count", entry->run_count);
    solua_set_int(L, -1, "skipped_count", entry->skipped_count);
}

static int solua_schedule_list(lua_State *L)
{
    lua_newtable(L);
    const size_t count = solar_os_schedule_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_schedule_entry_t entry;
        if (solar_os_schedule_get(i, &entry)) {
            solua_push_schedule_entry(L, &entry);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
    }
    return 1;
}

static int solua_schedule_add_in(lua_State *L)
{
    return solua_check_esp(L, solar_os_schedule_add_relative(
        luaL_checkstring(L, 1), solua_check_u32(L, 2),
        solua_schedule_action(L, 3), solua_optional_str(L, 4, NULL),
        lua_gettop(L) < 5 || lua_toboolean(L, 5) != 0));
}

static int solua_schedule_add_every(lua_State *L)
{
    return solua_check_esp(L, solar_os_schedule_add_interval(
        luaL_checkstring(L, 1), solua_check_u32(L, 2),
        solua_schedule_action(L, 3), solua_optional_str(L, 4, NULL)));
}

static int solua_schedule_add_at(lua_State *L)
{
    const uint32_t year = solua_check_u32(L, 2);
    const uint32_t month = solua_check_u32(L, 3);
    const uint32_t day = solua_check_u32(L, 4);
    const uint32_t hour = solua_check_u32(L, 5);
    const uint32_t minute = solua_check_u32(L, 6);
    const uint32_t second = solua_check_u32(L, 7);
    luaL_argcheck(L, year <= UINT16_MAX && month <= UINT8_MAX && day <= UINT8_MAX &&
                  hour <= UINT8_MAX && minute <= UINT8_MAX && second <= UINT8_MAX,
                  2, "calendar fields out of range");
    return solua_check_esp(L, solar_os_schedule_add_at(
        luaL_checkstring(L, 1), year, month, day, hour, minute, second,
        solua_schedule_action(L, 8),
        solua_optional_str(L, 9, NULL)));
}

static int solua_schedule_add_daily(lua_State *L)
{
    const uint32_t hour = solua_check_u32(L, 2);
    const uint32_t minute = solua_check_u32(L, 3);
    const uint32_t second = solua_check_u32(L, 4);
    luaL_argcheck(L, hour <= UINT8_MAX && minute <= UINT8_MAX && second <= UINT8_MAX,
                  2, "time fields out of range");
    return solua_check_esp(L, solar_os_schedule_add_daily(
        luaL_checkstring(L, 1), hour, minute, second, solua_schedule_action(L, 5),
        solua_optional_str(L, 6, NULL)));
}

static int solua_schedule_add_weekly(lua_State *L)
{
    const uint32_t weekdays = solua_check_u32(L, 2);
    const uint32_t hour = solua_check_u32(L, 3);
    const uint32_t minute = solua_check_u32(L, 4);
    const uint32_t second = solua_check_u32(L, 5);
    luaL_argcheck(L, weekdays <= UINT8_MAX && hour <= UINT8_MAX &&
                  minute <= UINT8_MAX && second <= UINT8_MAX,
                  2, "weekly fields out of range");
    return solua_check_esp(L, solar_os_schedule_add_weekly(
        luaL_checkstring(L, 1), weekdays, hour, minute, second,
        solua_schedule_action(L, 6), solua_optional_str(L, 7, NULL)));
}

static int solua_schedule_enable(lua_State *L)
{
    return solua_check_esp(L, solar_os_schedule_set_enabled(
        luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0));
}

static int solua_schedule_remove(lua_State *L)
{
    return solua_check_esp(L, solar_os_schedule_remove(luaL_checkstring(L, 1)));
}

static int solua_schedule_run(lua_State *L)
{
    return solua_check_esp(L, solar_os_schedule_run(luaL_checkstring(L, 1)));
}

static int solua_schedule_stop_alarm(lua_State *L)
{
    (void)L;
    solar_os_schedule_stop_alarm();
    return 0;
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

static int solua_wifi_repeater_start(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_repeater_start());
}

static int solua_wifi_repeater_stop(lua_State *L)
{
    return solua_check_esp(L, solar_os_wifi_repeater_stop());
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

#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
static solar_os_http_stream_session_t *solua_http_stream_get(lua_State *L)
{
    if (solua_http_stream_session == NULL) {
        (void)solua_check_esp(L,
                              solar_os_http_stream_session_create(
                                  solua_should_cancel,
                                  NULL,
                                  &solua_http_stream_session));
    }
    return solua_http_stream_session;
}

static void solua_http_stream_destroy(void)
{
    solar_os_http_stream_session_destroy(solua_http_stream_session);
    solua_http_stream_session = NULL;
}

static solar_os_http_session_context_t *solua_http_session_get(lua_State *L)
{
    if (solua_http_session_context == NULL) {
        (void)solua_check_esp(L,
                              solar_os_http_session_context_create(
                                  solua_should_cancel,
                                  NULL,
                                  &solua_http_session_context));
    }
    return solua_http_session_context;
}

static void solua_http_session_destroy(void)
{
    solar_os_http_session_context_destroy(solua_http_session_context);
    solua_http_session_context = NULL;
}

static solar_os_http_header_t *solua_http_headers_from_table(lua_State *L,
                                                             int index,
                                                             size_t *header_count)
{
    *header_count = 0;
    if (lua_isnoneornil(L, index)) {
        return NULL;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    index = lua_absindex(L, index);

    size_t header_bytes = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 2);
            luaL_error(L, "HTTP header names and values must be strings");
        }
        size_t name_len = 0;
        size_t value_len = 0;
        const char *name = lua_tolstring(L, -2, &name_len);
        const char *value = lua_tolstring(L, -1, &value_len);
        if (name_len == 0 || strlen(name) != name_len || strlen(value) != value_len ||
            strpbrk(name, "\r\n:") != NULL || strpbrk(value, "\r\n") != NULL) {
            lua_pop(L, 2);
            luaL_error(L, "invalid HTTP header");
        }
        if (name_len + value_len + 2U >
            SOLAR_OS_HTTP_BUFFERED_MAX_HEADER_BYTES - header_bytes) {
            lua_pop(L, 2);
            luaL_error(L, "HTTP headers exceed 8192 bytes");
        }
        header_bytes += name_len + value_len + 2U;
        (*header_count)++;
        lua_pop(L, 1);
    }
    if (*header_count > SOLUA_HTTP_MAX_REQUEST_HEADERS) {
        luaL_error(L, "too many HTTP headers");
    }
    if (*header_count == 0) {
        return NULL;
    }

    solar_os_http_header_t *headers = solar_os_memory_calloc(
        *header_count,
        sizeof(*headers),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "lua.http.headers");
    if (headers == NULL) {
        luaL_error(L, "%s", esp_err_to_name(ESP_ERR_NO_MEM));
    }

    size_t current = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        headers[current].name = lua_tostring(L, -2);
        headers[current].value = lua_tostring(L, -1);
        current++;
        lua_pop(L, 1);
    }
    return headers;
}

static size_t solua_http_max_bytes(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY;
    }
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || (lua_Unsigned)value > SOLUA_HTTP_MAX_BODY) {
        luaL_error(L, "HTTP max_bytes must be 0..262144");
    }
    return (size_t)value;
}

static uint32_t solua_http_timeout_ms(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return SOLUA_HTTP_DEFAULT_TIMEOUT_MS;
    }
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > INT_MAX) {
        luaL_error(L, "HTTP timeout_ms must be 0..2147483647");
    }
    return (uint32_t)value;
}

static int solua_http_push_response(
    lua_State *L,
    const solar_os_http_buffered_response_t *response)
{
    lua_newtable(L);
    const int result = lua_gettop(L);
    solua_set_int(L, result, "status_code", response->response.status_code);
    solua_set_int(L, result, "content_length", response->response.content_length);
    solua_set_int(L, result, "bytes_received", response->response.bytes_received);
    solua_set_int(L, result, "duration_ms", response->response.duration_ms);
    solua_set_bool(L, result, "truncated", response->body_truncated);
    solua_set_bool(L, result, "headers_truncated", response->headers_truncated);

    lua_newtable(L);
    const int headers = lua_gettop(L);
    for (size_t i = 0; i < response->header_count; i++) {
        lua_pushstring(L, response->headers[i].value);
        lua_setfield(L, headers, response->headers[i].name);
    }
    lua_setfield(L, result, "headers");

    lua_pushlstring(L,
                    response->body != NULL ? (const char *)response->body : "",
                    response->body_len);
    lua_setfield(L, result, "body");
    return 1;
}

static int solua_http_perform(lua_State *L,
                              solar_os_http_method_t method,
                              int url_index,
                              int body_index,
                              int headers_index,
                              int timeout_index,
                              int max_bytes_index,
                              int redirects_index)
{
    size_t url_len = 0;
    const char *url = luaL_checklstring(L, url_index, &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return luaL_error(L, "expected http:// or https:// URL");
    }
    size_t body_len = 0;
    const char *body = lua_isnoneornil(L, body_index) ?
        NULL : luaL_checklstring(L, body_index, &body_len);
    const uint32_t timeout_ms = solua_http_timeout_ms(L, timeout_index);
    const size_t max_bytes = solua_http_max_bytes(L, max_bytes_index);
    const bool follow_redirects = lua_isnoneornil(L, redirects_index) ||
        lua_toboolean(L, redirects_index);
    size_t header_count = 0;
    solar_os_http_header_t *headers =
        solua_http_headers_from_table(L, headers_index, &header_count);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .user_agent = "SolarOS/" SOLAR_OS_VERSION " script",
        .follow_redirects = follow_redirects,
        .timeout_ms = timeout_ms,
        .read_poll_ms = SOLUA_HTTP_READ_POLL_MS,
        .deadline_ms = timeout_ms,
        .should_cancel = solua_should_cancel,
    };
    solar_os_http_buffered_response_t response = {0};
    const esp_err_t err = solar_os_http_perform_buffered(&options,
                                                         method == SOLAR_OS_HTTP_METHOD_HEAD ?
                                                             0U : max_bytes,
                                                         &response);
    solar_os_memory_free(headers);
    if (err != ESP_OK) {
        solar_os_http_buffered_response_clear(&response);
        return solua_check_esp(L, err);
    }

    const int result_count = solua_http_push_response(L, &response);
    solar_os_http_buffered_response_clear(&response);
    return result_count;
}

static int solua_http_session_open(lua_State *L)
{
    size_t origin_len = 0;
    const char *origin = luaL_checklstring(L, 1, &origin_len);
    if (strlen(origin) != origin_len) {
        return luaL_error(L, "invalid HTTP origin");
    }
    uint32_t handle = 0;
    (void)solua_check_esp(L,
                          solar_os_http_session_open(
                              solua_http_session_get(L),
                              origin,
                              "SolarOS/" SOLAR_OS_VERSION " script",
                              &handle));
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_http_session_request(lua_State *L)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(luaL_checkstring(L, 2), &method)) {
        return luaL_error(L, "expected GET, POST, PUT, PATCH, DELETE, or HEAD");
    }
    size_t url_len = 0;
    const char *url = luaL_checklstring(L, 3, &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return luaL_error(L, "expected http:// or https:// URL");
    }
    size_t body_len = 0;
    const char *body = lua_isnoneornil(L, 4) ?
        NULL : luaL_checklstring(L, 4, &body_len);
    size_t header_count = 0;
    solar_os_http_header_t *headers = solua_http_headers_from_table(
        L,
        5,
        &header_count);
    const uint32_t timeout_ms = solua_http_timeout_ms(L, 6);
    const size_t max_bytes = solua_http_max_bytes(L, 7);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .timeout_ms = timeout_ms,
        .deadline_ms = timeout_ms,
    };
    solar_os_http_buffered_response_t response = {0};
    const esp_err_t err = solar_os_http_session_request(
        solua_http_session_get(L),
        solua_check_u32(L, 1),
        &options,
        method == SOLAR_OS_HTTP_METHOD_HEAD ? 0U : max_bytes,
        &response);
    solar_os_memory_free(headers);
    if (err != ESP_OK) {
        solar_os_http_buffered_response_clear(&response);
        return solua_check_esp(L, err);
    }
    const int result_count = solua_http_push_response(L, &response);
    solar_os_http_buffered_response_clear(&response);
    return result_count;
}

static int solua_http_session_close(lua_State *L)
{
    if (solua_http_session_context != NULL) {
        const esp_err_t err = solar_os_http_session_close(
            solua_http_session_context,
            solua_check_u32(L, 1));
        if (err != ESP_ERR_NOT_FOUND) {
            (void)solua_check_esp(L, err);
        }
    }
    return 0;
}

static int solua_http_session_close_all(lua_State *L)
{
    (void)L;
    if (solua_http_session_context != NULL) {
        solar_os_http_session_close_all(solua_http_session_context);
    }
    return 0;
}

static int solua_http_request(lua_State *L)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(luaL_checkstring(L, 1), &method)) {
        return luaL_error(L, "expected GET, POST, PUT, PATCH, DELETE, or HEAD");
    }
    return solua_http_perform(L, method, 2, 3, 4, 5, 6, 7);
}

static int solua_http_get(lua_State *L)
{
    return solua_http_perform(L, SOLAR_OS_HTTP_METHOD_GET, 1, 0, 2, 3, 4, 5);
}

static int solua_http_head(lua_State *L)
{
    return solua_http_perform(L, SOLAR_OS_HTTP_METHOD_HEAD, 1, 0, 2, 3, 4, 5);
}

#define SOLUA_HTTP_BODY_METHOD(name, method) \
    static int solua_http_##name(lua_State *L) \
    { \
        return solua_http_perform(L, method, 1, 2, 3, 4, 5, 6); \
    }

SOLUA_HTTP_BODY_METHOD(post, SOLAR_OS_HTTP_METHOD_POST)
SOLUA_HTTP_BODY_METHOD(put, SOLAR_OS_HTTP_METHOD_PUT)
SOLUA_HTTP_BODY_METHOD(patch, SOLAR_OS_HTTP_METHOD_PATCH)
SOLUA_HTTP_BODY_METHOD(delete, SOLAR_OS_HTTP_METHOD_DELETE)
#undef SOLUA_HTTP_BODY_METHOD

static int solua_http_stream_open(lua_State *L)
{
    solar_os_http_method_t method;
    if (!solar_os_http_method_parse(luaL_checkstring(L, 1), &method)) {
        return luaL_error(L, "expected GET, POST, PUT, PATCH, DELETE, or HEAD");
    }
    size_t url_len = 0;
    const char *url = luaL_checklstring(L, 2, &url_len);
    if (strlen(url) != url_len ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return luaL_error(L, "expected http:// or https:// URL");
    }
    size_t body_len = 0;
    const char *body = lua_isnoneornil(L, 3) ?
        NULL : luaL_checklstring(L, 3, &body_len);
    size_t header_count = 0;
    solar_os_http_header_t *headers = solua_http_headers_from_table(
        L,
        4,
        &header_count);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .user_agent = "SolarOS/" SOLAR_OS_VERSION " script",
        .follow_redirects = lua_isnoneornil(L, 6) || lua_toboolean(L, 6),
        .timeout_ms = solua_http_timeout_ms(L, 5),
        .read_poll_ms = SOLUA_HTTP_READ_POLL_MS,
    };
    uint32_t handle = 0;
    const esp_err_t err = solar_os_http_stream_open(solua_http_stream_get(L),
                                                    &options,
                                                    &handle);
    solar_os_memory_free(headers);
    (void)solua_check_esp(L, err);
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_http_push_stream_event(
    lua_State *L,
    const solar_os_http_stream_event_t *event)
{
    lua_newtable(L);
    const int result = lua_gettop(L);
    solua_set_str(L,
                  result,
                  "type",
                  solar_os_http_stream_event_type_name(event->type));
    solua_set_int(L, result, "status_code", event->status_code);
    if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_RESPONSE) {
        solua_set_int(L, result, "content_length", event->content_length);
    } else if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_HEADER) {
        solua_set_str(L, result, "name", event->header_name);
        solua_set_str(L, result, "value", event->header_value);
        solua_set_bool(L, result, "truncated", event->truncated);
    } else if (event->type == SOLAR_OS_HTTP_STREAM_EVENT_DATA) {
        lua_pushlstring(L, (const char *)event->data, event->data_len);
        lua_setfield(L, result, "data");
    } else {
        solua_set_int(L, result, "content_length", event->content_length);
        solua_set_int(L, result, "bytes_received", event->bytes_received);
        solua_set_int(L, result, "duration_ms", event->duration_ms);
        solua_set_int(L, result, "error", event->error);
        solua_set_str(L, result, "error_name", esp_err_to_name(event->error));
        solua_set_bool(L, result, "cancelled", event->cancelled);
        solua_set_bool(L,
                       result,
                       "deadline_exceeded",
                       event->deadline_exceeded);
    }
    return 1;
}

static int solua_http_stream_read(lua_State *L)
{
    if (solua_http_stream_session == NULL) {
        lua_pushnil(L);
        return 1;
    }
    solar_os_http_stream_event_t event;
    const esp_err_t err = solar_os_http_stream_read(
        solua_http_stream_session,
        solua_check_u32(L, 1),
        solua_optional_u32(L, 2, 0),
        &event);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
        lua_pushnil(L);
        return 1;
    }
    (void)solua_check_esp(L, err);
    return solua_http_push_stream_event(L, &event);
}

static int solua_http_stream_close(lua_State *L)
{
    if (solua_http_stream_session != NULL) {
        const esp_err_t err = solar_os_http_stream_close(
            solua_http_stream_session,
            solua_check_u32(L, 1));
        if (err != ESP_ERR_NOT_FOUND) {
            (void)solua_check_esp(L, err);
        }
    }
    return 0;
}

static int solua_http_stream_close_all(lua_State *L)
{
    (void)L;
    if (solua_http_stream_session != NULL) {
        solar_os_http_stream_close_all(solua_http_stream_session);
    }
    return 0;
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

#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
static void solua_push_control_info(lua_State *L,
                                    const solar_os_control_info_t *info)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_str(L, item, "name", info->config.name);
    solua_set_str(L, item, "source",
                  info->config.source[0] != '\0' ? info->config.source : NULL);
    solua_set_num(L, item, "input_min", info->config.input_minimum);
    solua_set_num(L, item, "input_max", info->config.input_maximum);
    solua_set_num(L, item, "deadband", info->config.deadband);
    solua_set_int(L, item, "smoothing_ms", info->config.smoothing_ms);
    solua_set_bool(L, item, "inverted", info->config.inverted);
    solua_set_bool(L, item, "has_value", info->has_value);
    solua_set_num(L, item, "source_value", info->source_value);
    solua_set_int(L, item, "generation", info->generation);
    solua_set_int(L, item, "samples", info->samples);
    solua_set_int(L, item, "updates", info->updates);
    solua_set_int(L, item, "read_errors", info->read_errors);
    solua_set_int(L, item, "last_error", info->last_error);
    solua_set_str(L, item, "last_error_name", esp_err_to_name(info->last_error));
    if (info->has_value) {
        solua_set_num(L, item, "value",
                      (lua_Number)info->normalized /
                          SOLAR_OS_CONTROL_NORMALIZED_MAX);
    }
}

static int solua_controls_list(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    const size_t count = solar_os_control_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_control_info_t info;
        if (!solar_os_control_get_info(i, &info)) {
            continue;
        }
        solua_push_control_info(L, &info);
        lua_rawseti(L, list, out++);
    }
    return 1;
}

static int solua_controls_get(lua_State *L)
{
    uint16_t value = 0U;
    (void)solua_check_esp(L,
                         solar_os_control_get(luaL_checkstring(L, 1), &value));
    lua_pushnumber(L, (lua_Number)value / SOLAR_OS_CONTROL_NORMALIZED_MAX);
    return 1;
}

static int solua_controls_set(lua_State *L)
{
    const lua_Number value = luaL_checknumber(L, 2);
    if (!isfinite((double)value) || value < 0.0 || value > 1.0) {
        return luaL_error(L, "control value must be 0.0..1.0");
    }
    (void)solua_check_esp(
        L,
        solar_os_control_set(
            luaL_checkstring(L, 1),
            (uint16_t)lround((double)value *
                             SOLAR_OS_CONTROL_NORMALIZED_MAX)));
    return 0;
}

static int solua_controls_create(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *source = solua_optional_str(L, 2, NULL);
    if (strlen(name) >= SOLAR_OS_CONTROL_NAME_MAX ||
        (source != NULL && strlen(source) >= SOLAR_OS_STREAM_ID_MAX)) {
        return luaL_error(L, "control name or source is too long");
    }
    solar_os_control_config_t config = {
        .input_minimum = (float)luaL_optnumber(L, 3, 0.0),
        .input_maximum = (float)luaL_optnumber(L, 4, 1.0),
        .smoothing_ms = solua_optional_u32(L, 5, 0U),
        .deadband = (float)luaL_optnumber(L, 6, 0.0),
        .inverted = !lua_isnoneornil(L, 7) && lua_toboolean(L, 7),
    };
    strlcpy(config.name, name, sizeof(config.name));
    if (source != NULL) {
        strlcpy(config.source, source, sizeof(config.source));
    }
    (void)solua_check_esp(L, solar_os_control_create(&config));
    solar_os_control_info_t info;
    (void)solua_check_esp(L, solar_os_control_find(config.name, &info));
    solua_push_control_info(L, &info);
    return 1;
}

static int solua_controls_delete(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_control_delete(luaL_checkstring(L, 1)));
}

static int solua_controls_clear(lua_State *L)
{
    const size_t removed = solar_os_control_count();
    solar_os_control_clear();
    lua_pushinteger(L, (lua_Integer)removed);
    return 1;
}

static void solua_push_control_binding(
    lua_State *L, const solar_os_control_binding_info_t *info)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_int(L, item, "id", info->id);
    solua_set_str(L, item, "control", info->control);
    solua_set_str(L, item, "target",
                  solar_os_control_target_name(info->target));
    if (info->target == SOLAR_OS_CONTROL_TARGET_PARAMETER) {
        solua_set_str(L, item, "parameter", info->parameter);
    } else {
        solua_set_int(L, item, "midi_channel", info->midi_channel);
        solua_set_int(L, item, "midi_controller", info->midi_controller);
    }
    solua_set_bool(L, item, "pickup", info->pickup);
    solua_set_bool(L, item, "pickup_seen", info->pickup_seen);
    solua_set_bool(L, item, "pickup_latched", info->pickup_latched);
    solua_set_int(L, item, "pickup_previous", info->pickup_previous);
    solua_set_int(L, item, "last_target_value", info->last_target_value);
    solua_set_int(L, item, "last_generation", info->last_generation);
    solua_set_int(L, item, "applied", info->applied);
    solua_set_int(L, item, "errors", info->errors);
    solua_set_int(L, item, "last_error", info->last_error);
    solua_set_str(L, item, "last_error_name", esp_err_to_name(info->last_error));
}

static int solua_controls_bindings(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    const size_t count = solar_os_control_binding_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_control_binding_info_t info;
        if (solar_os_control_binding_get(i, &info)) {
            solua_push_control_binding(L, &info);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_controls_bind_parameter(lua_State *L)
{
    uint32_t id = 0U;
    (void)solua_check_esp(L, solar_os_control_bind_parameter(
        luaL_checkstring(L, 1), luaL_checkstring(L, 2),
        !lua_isnoneornil(L, 3) && lua_toboolean(L, 3), &id));
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int solua_controls_bind_midi(lua_State *L)
{
    const lua_Integer channel = luaL_checkinteger(L, 2);
    const lua_Integer controller = luaL_checkinteger(L, 3);
    if (channel < 1 || channel > 16 || controller < 0 || controller > 127) {
        return luaL_error(L, "expected channel 1..16 and controller 0..127");
    }
    uint32_t id = 0U;
    (void)solua_check_esp(L, solar_os_control_bind_midi_cc(
        luaL_checkstring(L, 1), (uint8_t)channel, (uint8_t)controller, &id));
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int solua_controls_unbind(lua_State *L)
{
    size_t removed = 0U;
    (void)solua_check_esp(L, solar_os_control_unbind(
        luaL_checkstring(L, 1), &removed));
    lua_pushinteger(L, (lua_Integer)removed);
    return 1;
}

static void solua_push_parameter_info(lua_State *L,
                                      const solar_os_parameter_info_t *info)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_str(L, item, "path", info->path);
    solua_set_str(L, item, "owner", info->owner);
    solua_set_str(L, item, "name", info->name);
    solua_set_str(L, item, "label", info->label);
    solua_set_str(L, item, "unit", info->unit);
    solua_set_num(L, item, "minimum", info->minimum);
    solua_set_num(L, item, "maximum", info->maximum);
    solua_set_num(L, item, "step", info->step);
    solua_set_str(L, item, "curve", solar_os_parameter_curve_name(info->curve));
    float value = 0.0f;
    const esp_err_t err = solar_os_parameter_get(info->path, &value);
    solua_set_bool(L, item, "readable", err == ESP_OK);
    if (err == ESP_OK) {
        solua_set_num(L, item, "value", value);
    }
    solua_set_int(L, item, "error", err);
    solua_set_str(L, item, "error_name", esp_err_to_name(err));
}

static int solua_parameters_list(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    const size_t count = solar_os_parameter_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_parameter_info_t info;
        if (solar_os_parameter_get_info(i, &info)) {
            solua_push_parameter_info(L, &info);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_parameters_get(lua_State *L)
{
    float value = 0.0f;
    (void)solua_check_esp(L, solar_os_parameter_get(
        luaL_checkstring(L, 1), &value));
    lua_pushnumber(L, value);
    return 1;
}

static int solua_parameters_set(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    float value = (float)luaL_checknumber(L, 2);
    (void)solua_check_esp(L, solar_os_parameter_set(path, value));
    (void)solua_check_esp(L, solar_os_parameter_get(path, &value));
    lua_pushnumber(L, value);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MIDI
static const char *solua_midi_message_type(uint8_t status)
{
    if (status >= 0xf0U) {
        return "system";
    }
    switch (status & 0xf0U) {
    case 0x80U: return "note_off";
    case 0x90U: return "note_on";
    case 0xa0U: return "poly_pressure";
    case 0xb0U: return "cc";
    case 0xc0U: return "program";
    case 0xd0U: return "channel_pressure";
    case 0xe0U: return "pitch_bend";
    default: return "unknown";
    }
}

static void solua_push_midi_message(lua_State *L,
                                    const solar_os_midi_message_t *message)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_int(L, item, "status", message->status);
    solua_set_int(L, item, "length", message->length);
    solua_set_str(L, item, "type", solua_midi_message_type(message->status));
    if (message->status < 0xf0U) {
        solua_set_int(L, item, "channel", (message->status & 0x0fU) + 1U);
    }
    if (message->length > 1U) {
        solua_set_int(L, item, "data1", message->data1);
    }
    if (message->length > 2U) {
        solua_set_int(L, item, "data2", message->data2);
    }
}

static int solua_midi_status(lua_State *L)
{
    solar_os_midi_status_t status;
    solar_os_midi_get_status(&status);
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_bool(L, item, "running", status.running);
    solua_set_str(L, item, "bus",
                  status.bus_name[0] != '\0' ? status.bus_name : NULL);
    solua_set_int(L, item, "rx_bytes", status.rx_bytes);
    solua_set_int(L, item, "rx_messages", status.rx_messages);
    solua_set_int(L, item, "tx_bytes", status.tx_bytes);
    solua_set_int(L, item, "tx_messages", status.tx_messages);
    solua_set_int(L, item, "parser_unsupported", status.parser_unsupported);
    solua_set_int(L, item, "subscriber_drops", status.subscriber_drops);
    solua_set_int(L, item, "tx_drops", status.tx_drops);
    solua_set_int(L, item, "last_error", status.last_error);
    solua_set_str(L, item, "last_error_name", esp_err_to_name(status.last_error));
    solua_set_int(L, item, "cc_streams", solar_os_midi_cc_stream_count());
    solua_set_bool(L, item, "subscribed", solua_midi_subscribed);
    return 1;
}

static uint8_t solua_midi_data(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > 127) {
        luaL_error(L, "expected MIDI data 0..127");
    }
    return (uint8_t)value;
}

static uint8_t solua_midi_channel(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 1 || value > 16) {
        luaL_error(L, "expected MIDI channel 1..16");
    }
    return (uint8_t)value;
}

static int solua_midi_send_checked(lua_State *L,
                                   const solar_os_midi_message_t *message)
{
    (void)solua_check_esp(L, solar_os_midi_send(message));
    solua_push_midi_message(L, message);
    return 1;
}

static int solua_midi_send(lua_State *L)
{
    const lua_Integer status_value = luaL_checkinteger(L, 1);
    if (status_value < 0 || status_value > 255) {
        return luaL_error(L, "expected MIDI status 0..255");
    }
    const uint8_t status = (uint8_t)status_value;
    const size_t length = solar_os_midi_message_length(status);
    if (length == 0U || lua_gettop(L) != (int)length) {
        return luaL_error(L, "unsupported status or wrong data byte count");
    }
    solar_os_midi_message_t message = {
        .status = status,
        .length = (uint8_t)length,
    };
    if (length > 1U) {
        message.data1 = solua_midi_data(L, 2);
    }
    if (length > 2U) {
        message.data2 = solua_midi_data(L, 3);
    }
    return solua_midi_send_checked(L, &message);
}

static int solua_midi_note_on(lua_State *L)
{
    const uint8_t channel = solua_midi_channel(L, 1);
    const solar_os_midi_message_t message = {
        .status = (uint8_t)(0x90U | (channel - 1U)),
        .data1 = solua_midi_data(L, 2),
        .data2 = lua_isnoneornil(L, 3) ? 100U : solua_midi_data(L, 3),
        .length = 3U,
    };
    return solua_midi_send_checked(L, &message);
}

static int solua_midi_note_off(lua_State *L)
{
    const uint8_t channel = solua_midi_channel(L, 1);
    const solar_os_midi_message_t message = {
        .status = (uint8_t)(0x80U | (channel - 1U)),
        .data1 = solua_midi_data(L, 2),
        .data2 = lua_isnoneornil(L, 3) ? 64U : solua_midi_data(L, 3),
        .length = 3U,
    };
    return solua_midi_send_checked(L, &message);
}

static int solua_midi_cc(lua_State *L)
{
    const uint8_t channel = solua_midi_channel(L, 1);
    const solar_os_midi_message_t message = {
        .status = (uint8_t)(0xb0U | (channel - 1U)),
        .data1 = solua_midi_data(L, 2),
        .data2 = solua_midi_data(L, 3),
        .length = 3U,
    };
    return solua_midi_send_checked(L, &message);
}

static int solua_midi_program(lua_State *L)
{
    const uint8_t channel = solua_midi_channel(L, 1);
    const solar_os_midi_message_t message = {
        .status = (uint8_t)(0xc0U | (channel - 1U)),
        .data1 = solua_midi_data(L, 2),
        .length = 2U,
    };
    return solua_midi_send_checked(L, &message);
}

static int solua_midi_subscribe(lua_State *L)
{
    if (!solua_midi_subscribed) {
        (void)solua_check_esp(L, solar_os_midi_subscribe(
            solua_runner_control != NULL ? "lua.runner" : "lua.app",
            &solua_midi_subscription));
        solua_midi_subscribed = true;
    }
    return 0;
}

static void solua_midi_destroy(void)
{
    if (solua_midi_subscribed) {
        (void)solar_os_midi_unsubscribe(&solua_midi_subscription);
        solua_midi_subscription = (solar_os_midi_subscription_t)
            SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
        solua_midi_subscribed = false;
    }
}

static int solua_midi_read(lua_State *L)
{
    const uint32_t timeout_ms = solua_optional_u32(L, 1, 0U);
    if (timeout_ms > SOLUA_MIDI_READ_MAX_MS) {
        return luaL_error(L, "MIDI read limited to 60000 ms");
    }
    (void)solua_midi_subscribe(L);
    const TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    for (;;) {
        solar_os_midi_message_t message;
        const esp_err_t err = solar_os_midi_receive(
            &solua_midi_subscription, &message);
        if (err == ESP_OK) {
            solua_push_midi_message(L, &message);
            return 1;
        }
        if (err != ESP_ERR_TIMEOUT) {
            return solua_check_esp(L, err);
        }
        if (timeout_ms == 0U ||
            (xTaskGetTickCount() - started) >= timeout_ticks) {
            lua_pushnil(L);
            return 1;
        }
        if (solua_should_cancel(NULL)) {
            solua.interrupt_requested = true;
            solua.interrupted = true;
            return luaL_error(L, "interrupted");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int solua_midi_close(lua_State *L)
{
    const bool was_subscribed = solua_midi_subscribed;
    solua_midi_destroy();
    lua_pushboolean(L, was_subscribed);
    return 1;
}

static void solua_push_midi_stream(lua_State *L,
                                   const solar_os_midi_cc_stream_info_t *info)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_str(L, item, "id", info->id);
    solua_set_int(L, item, "channel", info->channel);
    solua_set_int(L, item, "controller", info->controller);
    solua_set_bool(L, item, "has_value", info->has_value);
    if (info->has_value) {
        solua_set_int(L, item, "value", info->value);
    }
    solua_set_int(L, item, "updates", info->updates);
}

static int solua_midi_streams(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    const size_t count = solar_os_midi_cc_stream_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_midi_cc_stream_info_t info;
        if (solar_os_midi_cc_stream_get(i, &info)) {
            solua_push_midi_stream(L, &info);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_midi_stream_add(lua_State *L)
{
    const uint8_t channel = solua_midi_channel(L, 1);
    const uint8_t controller = solua_midi_data(L, 2);
    (void)solua_check_esp(L,
                          solar_os_midi_cc_stream_add(channel, controller));
    lua_pushfstring(L, "midi.cc.%d.%d", (int)channel, (int)controller);
    return 1;
}

static int solua_midi_stream_remove(lua_State *L)
{
    return solua_check_esp(L, solar_os_midi_cc_stream_remove(
        solua_midi_channel(L, 1), solua_midi_data(L, 2)));
}

static int solua_midi_stream_clear(lua_State *L)
{
    size_t removed = 0U;
    (void)solua_check_esp(L, solar_os_midi_cc_stream_clear(&removed));
    lua_pushinteger(L, (lua_Integer)removed);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_OSC
static void solua_push_osc_binding(lua_State *L,
                                   const solar_os_osc_binding_info_t *info)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_int(L, item, "id", info->id);
    solua_set_str(L, item, "name", info->config.name);
    solua_set_str(L, item, "source_type",
                  solar_os_osc_source_name(info->config.source_type));
    solua_set_str(L, item, "value_type",
                  solar_os_osc_value_name(info->config.value_type));
    solua_set_str(L, item, "source", info->config.source);
    solua_set_str(L, item, "address", info->config.address);
    solua_set_int(L, item, "interval_ms", info->config.interval_ms);
    solua_set_num(L, item, "rate_hz",
                  1000.0 / (lua_Number)info->config.interval_ms);
    solua_set_num(L, item, "delta", info->config.delta);
    solua_set_bool(L, item, "send_always", info->config.send_always);
    solua_set_str(L, item, "edge", solar_os_osc_edge_name(info->config.edge));
    solua_set_bool(L, item, "source_available", info->source_available);
    solua_set_bool(L, item, "has_value", info->has_value);
    if (info->has_value) {
        solua_set_num(L, item, "last_value", info->last_value);
    }
    solua_set_bool(L, item, "has_sent_value", info->has_sent_value);
    if (info->has_sent_value) {
        solua_set_num(L, item, "last_sent_value", info->last_sent_value);
    }
    solua_set_int(L, item, "last_sample_ms", (lua_Integer)info->last_sample_ms);
    solua_set_int(L, item, "last_send_ms", (lua_Integer)info->last_send_ms);
    solua_set_int(L, item, "sent", info->sent);
    solua_set_int(L, item, "send_errors", info->send_errors);
    solua_set_int(L, item, "source_errors", info->source_errors);
    solua_set_int(L, item, "last_error", info->last_error);
    solua_set_str(L, item, "last_error_name", esp_err_to_name(info->last_error));
}

static int solua_osc_bindings(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    int out = 1;
    const size_t count = solar_os_osc_binding_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_osc_binding_info_t info;
        if (solar_os_osc_binding_get(i, &info)) {
            solua_push_osc_binding(L, &info);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static uint32_t solua_osc_interval(lua_State *L, int index)
{
    const float rate = (float)luaL_checknumber(L, index);
    if (!isfinite(rate) || rate * 1000.0f < SOLAR_OS_OSC_RATE_MIN_MILLIHZ ||
        rate * 1000.0f > SOLAR_OS_OSC_RATE_MAX_MILLIHZ) {
        luaL_error(L, "expected OSC rate 0.1..100 Hz");
    }
    return (uint32_t)lroundf(1000.0f / rate);
}

static void solua_osc_binding_strings(lua_State *L,
                                      solar_os_osc_binding_config_t *config)
{
    const char *name = luaL_checkstring(L, 1);
    const char *source = luaL_checkstring(L, 2);
    const char *address = luaL_checkstring(L, 3);
    if (strlen(name) >= sizeof(config->name) ||
        strlen(source) >= sizeof(config->source) ||
        strlen(address) >= sizeof(config->address)) {
        luaL_error(L, "OSC name, source, or address is too long");
    }
    strlcpy(config->name, name, sizeof(config->name));
    strlcpy(config->source, source, sizeof(config->source));
    strlcpy(config->address, address, sizeof(config->address));
}

static int solua_osc_bind_config(lua_State *L,
                                 const solar_os_osc_binding_config_t *config)
{
    uint32_t id = 0U;
    (void)solua_check_esp(L, solar_os_osc_bind(config, &id));
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int solua_osc_bind_stream(lua_State *L)
{
    solar_os_osc_binding_config_t config = {
        .source_type = SOLAR_OS_OSC_SOURCE_STREAM,
        .value_type = SOLAR_OS_OSC_VALUE_SCALAR,
        .interval_ms = lua_isnoneornil(L, 4) ? 20U :
                                                  solua_osc_interval(L, 4),
        .delta = (float)luaL_optnumber(L, 5, 0.0),
        .send_always = !lua_isnoneornil(L, 6) && lua_toboolean(L, 6),
        .edge = SOLAR_OS_OSC_EDGE_BOTH,
    };
    solua_osc_binding_strings(L, &config);
    return solua_osc_bind_config(L, &config);
}

static solar_os_osc_edge_t solua_osc_edge(lua_State *L, int index)
{
    const char *edge = luaL_checkstring(L, index);
    if (strcmp(edge, "rising") == 0) {
        return SOLAR_OS_OSC_EDGE_RISING;
    }
    if (strcmp(edge, "falling") == 0) {
        return SOLAR_OS_OSC_EDGE_FALLING;
    }
    if (strcmp(edge, "both") == 0) {
        return SOLAR_OS_OSC_EDGE_BOTH;
    }
    luaL_error(L, "expected edge rising, falling, or both");
}

static int solua_osc_bind_event(lua_State *L)
{
    solar_os_osc_binding_config_t config = {
        .source_type = SOLAR_OS_OSC_SOURCE_STREAM,
        .value_type = SOLAR_OS_OSC_VALUE_EVENT,
        .interval_ms = lua_isnoneornil(L, 5) ? 20U :
                                                  solua_osc_interval(L, 5),
        .edge = lua_isnoneornil(L, 4) ? SOLAR_OS_OSC_EDGE_BOTH :
                                        solua_osc_edge(L, 4),
    };
    solua_osc_binding_strings(L, &config);
    return solua_osc_bind_config(L, &config);
}

static int solua_osc_bind_control(lua_State *L)
{
    solar_os_osc_binding_config_t config = {
        .source_type = SOLAR_OS_OSC_SOURCE_CONTROL,
        .value_type = SOLAR_OS_OSC_VALUE_SCALAR,
        .interval_ms = lua_isnoneornil(L, 4) ? 20U :
                                                  solua_osc_interval(L, 4),
        .send_always = !lua_isnoneornil(L, 5) && lua_toboolean(L, 5),
        .edge = SOLAR_OS_OSC_EDGE_BOTH,
    };
    solua_osc_binding_strings(L, &config);
    return solua_osc_bind_config(L, &config);
}

static int solua_osc_unbind(lua_State *L)
{
    return solua_check_esp(L, solar_os_osc_unbind(luaL_checkstring(L, 1)));
}

static int solua_osc_clear(lua_State *L)
{
    const size_t removed = solar_os_osc_binding_count();
    solar_os_osc_clear();
    lua_pushinteger(L, (lua_Integer)removed);
    return 1;
}

static int solua_osc_encode_float(lua_State *L)
{
    uint8_t packet[SOLAR_OS_OSC_PACKET_MAX];
    size_t length = 0U;
    (void)solua_check_esp(L, solar_os_osc_encode_float(
        luaL_checkstring(L, 1), (float)luaL_checknumber(L, 2),
        packet, sizeof(packet), &length));
    lua_pushlstring(L, (const char *)packet, length);
    return 1;
}

static int solua_osc_encode_int(lua_State *L)
{
    const lua_Integer value = luaL_checkinteger(L, 2);
    if (value < INT32_MIN || value > INT32_MAX) {
        return luaL_error(L, "OSC integer out of range");
    }
    uint8_t packet[SOLAR_OS_OSC_PACKET_MAX];
    size_t length = 0U;
    (void)solua_check_esp(L, solar_os_osc_encode_int(
        luaL_checkstring(L, 1), (int32_t)value,
        packet, sizeof(packet), &length));
    lua_pushlstring(L, (const char *)packet, length);
    return 1;
}

static int solua_osc_dispatch(lua_State *L)
{
    size_t length = 0U;
    const char *packet = luaL_checklstring(L, 1, &length);
    solar_os_osc_dispatch_result_t result;
    (void)solua_check_esp(L, solar_os_osc_dispatch_packet(
        (const uint8_t *)packet, length, &result));
    lua_newtable(L);
    solua_set_int(L, -1, "messages", result.messages);
    solua_set_int(L, -1, "applied", result.applied);
    solua_set_int(L, -1, "unknown_paths", result.unknown_paths);
    solua_set_int(L, -1, "rejected_values", result.rejected_values);
    return 1;
}

static int solua_osc_limits(lua_State *L)
{
    lua_newtable(L);
    solua_set_int(L, -1, "packet_max", SOLAR_OS_OSC_PACKET_MAX);
    solua_set_int(L, -1, "address_max", SOLAR_OS_OSC_ADDRESS_MAX - 1U);
    solua_set_int(L, -1, "bindings_max", SOLAR_OS_OSC_BINDING_MAX);
    solua_set_int(L, -1, "bundle_depth_max", SOLAR_OS_OSC_BUNDLE_DEPTH_MAX);
    solua_set_int(L, -1, "packet_updates_max", SOLAR_OS_OSC_PACKET_UPDATE_MAX);
    solua_set_num(L, -1, "rate_min_hz",
                  SOLAR_OS_OSC_RATE_MIN_MILLIHZ / 1000.0);
    solua_set_num(L, -1, "rate_max_hz",
                  SOLAR_OS_OSC_RATE_MAX_MILLIHZ / 1000.0);
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
         protocol <= SOLAR_OS_BUS_PROTOCOL_MIDI;
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
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        solua_set_int(L, -1, "port", info->config.uart.port);
        solua_set_int(L, -1, "tx_pin", info->config.uart.tx_pin);
        solua_set_int(L, -1, "rx_pin", info->config.uart.rx_pin);
        solua_set_int(L, -1, "baud_rate", info->config.uart.baud_rate);
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        solua_set_int(L, -1, "pin", info->config.onewire.pin);
        break;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        solua_set_int(L, -1, "clock_pin", info->config.ps2.clock_pin);
        solua_set_int(L, -1, "data_pin", info->config.ps2.data_pin);
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

#if SOLAR_OS_PACKAGE_SERVICE_PS2
static int solua_buses_create_ps2(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    solar_os_bus_definition_t definition = {
        .name = name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_PS2,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = SOLAR_OS_BUS_EXCLUSIVE,
        .config.ps2 = {
            .clock_pin = solua_table_int(L, 2, "clock", true, -1),
            .data_pin = solua_table_int(L, 2, "data", true, -1),
        },
    };
    (void)solua_check_esp(L, solar_os_bus_register(&definition));
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_PS2, &info)) {
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

static int solua_buses_create_midi(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    solar_os_bus_definition_t definition = {
        .name = name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_MIDI,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = SOLAR_OS_BUS_EXCLUSIVE,
        .config.uart = {
            .port = -1,
            .tx_pin = solua_table_int(L, 2, "tx", true, -1),
            .rx_pin = solua_table_int(L, 2, "rx", true, -1),
            .baud_rate = (uint32_t)solua_table_int(L,
                                                   2,
                                                   "baud_rate",
                                                   false,
                                                   SOLAR_OS_BUS_MIDI_DEFAULT_BAUD_RATE),
        },
    };
    if (definition.config.uart.baud_rate < SOLAR_OS_BUS_UART_MIN_BAUD_RATE ||
        definition.config.uart.baud_rate > SOLAR_OS_BUS_UART_MAX_BAUD_RATE) {
        return luaL_error(L, "expected baud_rate 300..921600");
    }

    (void)solua_check_esp(L, solar_os_bus_register(&definition));
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_MIDI, &info)) {
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
        solua_set_str(L,
                      -1,
                      "origin",
                      solar_os_expansion_origin_name(device.origin));
        solua_set_bool(L, -1, "ready", device.ready);
        solua_set_bool(L, -1, "autostart", device.autostart);
        solua_set_bool(L, -1, "detachable", device.detachable);
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
        "spi", "cs", "ce", "i2c", "addr", "uart", "ps2", "gpio", "irq", "reset",
        "rst", "data", "bck", "din", "rck", "dc", "busy", "adc", "pwm",
        "count", "keys", "x", "y", "min", "center", "max", "deadzone",
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
    const char *ps2 = solua_table_optional_string(L, 3, "ps2");
    const char *x = solua_table_optional_string(L, 3, "x");
    const char *y = solua_table_optional_string(L, 3, "y");
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
    if (ps2 != NULL) {
        if (!solar_os_bus_find(ps2, SOLAR_OS_BUS_PROTOCOL_PS2, NULL)) {
            return solua_check_esp(L, ESP_ERR_NOT_FOUND);
        }
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_PS2_BUS,
                                    "",
                                    ps2,
                                    -1,
                                    -1);
    }
    if (x != NULL) {
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM,
                                    "x",
                                    x,
                                    -1,
                                    -1);
    }
    if (y != NULL) {
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM,
                                    "y",
                                    y,
                                    -1,
                                    -1);
    }

    lua_getfield(L, 3, "keys");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char *role = luaL_checkstring(L, -2);
            uint8_t parsed_key = 0;
            if (!solar_os_key_parse(role, &parsed_key)) {
                return luaL_error(L, "unknown key %s", role);
            }
            solua_expansion_add_binding(L,
                                        bindings,
                                        &binding_count,
                                        SOLAR_OS_EXPANSION_BINDING_GPIO,
                                        role,
                                        "",
                                        (int)luaL_checkinteger(L, -1),
                                        -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    static const struct {
        const char *key;
        const char *role;
        solar_os_expansion_binding_kind_t kind;
    } pin_bindings[] = {
        {"gpio", "gpio", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"irq", "irq", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"reset", "reset", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"rst", "reset", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"data", "data", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"bck", "bck", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"din", "din", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"rck", "rck", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"mclk", "mclk", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"ws", "ws", SOLAR_OS_EXPANSION_BINDING_GPIO},
        {"dout", "dout", SOLAR_OS_EXPANSION_BINDING_GPIO},
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

    if (solua_table_optional_int(L, 3, "count", &value)) {
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_PARAMETER,
                                    "count",
                                    "",
                                    value,
                                    -1);
    }
    static const char *const parameter_bindings[] = {
        "min", "center", "max", "deadzone",
    };
    for (size_t i = 0;
         i < sizeof(parameter_bindings) / sizeof(parameter_bindings[0]);
         i++) {
        if (!solua_table_optional_int(L, 3, parameter_bindings[i], &value)) {
            continue;
        }
        solua_expansion_add_binding(L,
                                    bindings,
                                    &binding_count,
                                    SOLAR_OS_EXPANSION_BINDING_PARAMETER,
                                    parameter_bindings[i],
                                    "",
                                    value,
                                    -1);
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

#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
static int solua_neopixel_list(lua_State *L)
{
    lua_newtable(L);
    int out = 1;
    const size_t count = solar_os_neopixel_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_neopixel_info_t info;
        if (!solar_os_neopixel_get(i, &info)) {
            continue;
        }
        lua_newtable(L);
        solua_set_str(L, -1, "name", info.name);
        solua_set_int(L, -1, "data_pin", info.data_pin);
        solua_set_int(L, -1, "count", info.pixel_count);
        lua_rawseti(L, -2, out++);
    }
    return 1;
}

static int solua_neopixel_set(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_neopixel_set(luaL_checkstring(L, 1),
                                                 solua_check_size(L, 2),
                                                 solua_check_u8(L, 3),
                                                 solua_check_u8(L, 4),
                                                 solua_check_u8(L, 5)));
}

static int solua_neopixel_fill(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_neopixel_fill(luaL_checkstring(L, 1),
                                                  solua_check_u8(L, 2),
                                                  solua_check_u8(L, 3),
                                                  solua_check_u8(L, 4)));
}

static int solua_neopixel_show(lua_State *L)
{
    return solua_check_esp(L, solar_os_neopixel_show(luaL_checkstring(L, 1)));
}

static int solua_neopixel_clear(lua_State *L)
{
    return solua_check_esp(L, solar_os_neopixel_clear(luaL_checkstring(L, 1)));
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

static int solua_audio_tone_async(lua_State *L)
{
    const solar_os_audio_tone_step_t step = {
        .frequency_hz = solua_check_u32(L, 1),
        .duration_ms = solua_check_u32(L, 2),
    };
    const solar_os_audio_tone_request_t request = {
        .steps = &step,
        .step_count = 1,
        .volume = solua_optional_u8(L, 3, SOLAR_OS_AUDIO_VOLUME_GLOBAL),
    };
    uint32_t request_id = 0;
    (void)solua_check_esp(L, solar_os_audio_tone_enqueue(&request, &request_id));
    lua_pushinteger(L, request_id);
    return 1;
}

static int solua_audio_cancel(lua_State *L)
{
    return solua_check_esp(L, solar_os_audio_tone_cancel(solua_check_u32(L, 1)));
}

static int solua_audio_queue_status(lua_State *L)
{
    solar_os_audio_tone_queue_status_t status;
    solar_os_audio_tone_queue_get_status(&status);

    lua_newtable(L);
    solua_set_bool(L, -1, "worker_running", status.worker_running);
    solua_set_bool(L, -1, "playing", status.playing);
    solua_set_int(L, -1, "queued", status.queued);
    solua_set_int(L, -1, "current_id", status.current_id);
    solua_set_int(L, -1, "completed", status.completed);
    solua_set_int(L, -1, "cancelled", status.cancelled);
    solua_set_int(L, -1, "dropped", status.dropped);
    solua_set_int(L, -1, "failed", status.failed);
    return 1;
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

static int solua_audio_capture(lua_State *L)
{
    const size_t frames = solua_check_size(L, 1);
    if (frames == 0U || frames > SOLAR_OS_AUDIO_CAPTURE_MAX_FRAMES) {
        return luaL_error(L, "expected frames 1..4096");
    }

    const size_t sample_capacity =
        frames * SOLAR_OS_AUDIO_CAPTURE_MAX_CHANNELS;
    int16_t *samples = solar_os_memory_alloc(
        sample_capacity * sizeof(*samples),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "lua.audio.capture");
    if (samples == NULL) {
        return solua_check_esp(L, ESP_ERR_NO_MEM);
    }

    solar_os_audio_stream_format_t format;
    const esp_err_t err = solar_os_audio_capture(
        "lua", frames, samples, sample_capacity, &format);
    if (err != ESP_OK) {
        solar_os_memory_free(samples);
        return solua_check_esp(L, err);
    }

    lua_pushlstring(L,
                    (const char *)samples,
                    frames * format.channels * sizeof(*samples));
    solar_os_memory_free(samples);

    lua_newtable(L);
    lua_pushstring(L, solar_os_stream_audio_sample_format_name(format.sample_format));
    lua_setfield(L, -2, "sample_format");
    solua_set_int(L, -1, "sample_rate", format.sample_rate);
    solua_set_int(L, -1, "channels", format.channels);
    solua_set_int(L, -1, "bits_per_sample", format.bits_per_sample);
    return 2;
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

#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
#define SOLUA_SYNTH_OWNER "lua"

static int solua_synth_status(lua_State *L)
{
    solar_os_synth_voice_status_t status;
    solar_os_synth_voice_get_status(&status);

    lua_newtable(L);
    solua_set_bool(L, -1, "running", status.running);
    solua_set_str(L, -1, "owner", status.owner);
    solua_set_str(L,
                  -1,
                  "waveform",
                  solar_os_synth_waveform_name(status.config.waveform));
    solua_set_int(L, -1, "attack_ms", status.config.attack_ms);
    solua_set_int(L, -1, "decay_ms", status.config.decay_ms);
    solua_set_int(L, -1, "sustain_percent", status.config.sustain_percent);
    solua_set_int(L, -1, "release_ms", status.config.release_ms);
    solua_set_str(
        L,
        -1,
        "oscillator2_waveform",
        solar_os_synth_waveform_name(status.config.oscillator2.waveform));
    solua_set_int(L, -1, "oscillator2_octave", status.config.oscillator2.octave);
    solua_set_int(L,
                  -1,
                  "oscillator2_detune_cents",
                  status.config.oscillator2.detune_cents);
    solua_set_int(L,
                  -1,
                  "oscillator2_mix_percent",
                  status.config.oscillator2.mix_percent);
    solua_set_int(L, -1, "filter_cutoff_hz", status.config.filter.cutoff_hz);
    solua_set_int(L,
                  -1,
                  "filter_resonance_percent",
                  status.config.filter.resonance_percent);
    solua_set_int(L,
                  -1,
                  "filter_envelope_amount_percent",
                  status.config.filter.envelope_amount_percent);
    solua_set_int(L, -1, "filter_attack_ms", status.config.filter.attack_ms);
    solua_set_int(L, -1, "filter_decay_ms", status.config.filter.decay_ms);
    solua_set_int(L,
                  -1,
                  "filter_sustain_percent",
                  status.config.filter.sustain_percent);
    solua_set_int(L, -1, "filter_release_ms", status.config.filter.release_ms);
    solua_set_bool(L, -1, "mono", status.performance.mono);
    solua_set_int(L, -1, "glide_ms", status.performance.glide_ms);
    solua_set_int(L, -1, "active_voices", status.active_voices);
    solua_set_int(L, -1, "max_voices", SOLAR_OS_SYNTH_VOICE_MAX);
    solua_set_int(L, -1, "stolen_voices", status.stolen_voices);
    solua_set_int(L, -1, "sample_rate", status.sample_rate);
    solua_set_int(L,
                  -1,
                  "render_deadline_misses",
                  status.render_deadline_misses);
    solua_set_str(L,
                  -1,
                  "pcm_waveform",
                  solar_os_synth_waveform_name(status.pcm_waveform));
    solua_set_int(L, -1, "pcm_generation", status.pcm_generation);
    solua_set_int(L, -1, "pcm_hash", status.pcm_hash);
    solua_set_int(L, -1, "pcm_mean_abs", status.pcm_mean_abs);
    solua_set_int(L, -1, "pcm_peak", status.pcm_peak);
    solua_set_int(L, -1, "pcm_rms", status.pcm_rms);
    solua_set_int(L, -1, "pcm_min", status.pcm_min);
    solua_set_int(L, -1, "pcm_max", status.pcm_max);
    lua_newtable(L);
    const int pcm_samples = lua_gettop(L);
    for (size_t i = 0; i < status.pcm_sample_count; i++) {
        lua_pushinteger(L, status.pcm_samples[i]);
        lua_rawseti(L, pcm_samples, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "pcm_samples");
    solua_set_int(L, -1, "last_error", status.last_error);
    solua_set_str(L, -1, "last_error_name", esp_err_to_name(status.last_error));
    return 1;
}

static int solua_synth_configure(lua_State *L)
{
    solar_os_synth_waveform_t waveform;
    if (!solar_os_synth_parse_waveform(luaL_checkstring(L, 1), &waveform)) {
        return luaL_error(L,
                          "expected square, triangle, saw, sine, or noise");
    }
    solar_os_synth_voice_status_t status;
    solar_os_synth_voice_get_status(&status);
    const solar_os_synth_voice_config_t config = {
        .waveform = waveform,
        .attack_ms = solua_optional_u32(
            L, 2, SOLAR_OS_SYNTH_VOICE_DEFAULT_ATTACK_MS),
        .decay_ms = solua_optional_u32(
            L, 3, SOLAR_OS_SYNTH_VOICE_DEFAULT_DECAY_MS),
        .sustain_percent = solua_optional_u8(
            L, 4, SOLAR_OS_SYNTH_VOICE_DEFAULT_SUSTAIN_PERCENT),
        .release_ms = solua_optional_u32(
            L, 5, SOLAR_OS_SYNTH_VOICE_DEFAULT_RELEASE_MS),
        .oscillator2 = status.config.oscillator2,
        .filter = status.config.filter,
    };
    return solua_check_esp(
        L, solar_os_synth_voice_configure(SOLUA_SYNTH_OWNER, &config));
}

static int solua_synth_configure_oscillator2(lua_State *L)
{
    solar_os_synth_waveform_t waveform;
    if (!solar_os_synth_parse_waveform(luaL_checkstring(L, 1), &waveform)) {
        return luaL_error(L,
                          "expected square, triangle, saw, sine, or noise");
    }
    const lua_Integer octave = lua_isnoneornil(L, 2)
                                   ? SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_OCTAVE
                                   : luaL_checkinteger(L, 2);
    const lua_Integer detune =
        lua_isnoneornil(L, 3)
            ? SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_DETUNE_CENTS
            : luaL_checkinteger(L, 3);
    if (octave < SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN ||
        octave > SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX) {
        return luaL_error(L, "oscillator2 octave must be -2..2");
    }
    if (detune < SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS ||
        detune > SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS) {
        return luaL_error(L,
                          "oscillator2 detune must be -100..100 cents");
    }

    solar_os_synth_voice_status_t status;
    solar_os_synth_voice_get_status(&status);
    solar_os_synth_voice_config_t config = status.config;
    config.oscillator2 = (solar_os_synth_oscillator_config_t){
        .waveform = waveform,
        .octave = (int8_t)octave,
        .detune_cents = (int16_t)detune,
        .mix_percent = solua_optional_u8(
            L, 4, SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_MIX_PERCENT),
    };
    return solua_check_esp(
        L, solar_os_synth_voice_configure(SOLUA_SYNTH_OWNER, &config));
}

static int solua_synth_configure_performance(lua_State *L)
{
    const uint32_t glide_ms = solua_optional_u32(
        L, 2, SOLAR_OS_SYNTH_VOICE_DEFAULT_GLIDE_MS);
    if (glide_ms > SOLAR_OS_SYNTH_VOICE_GLIDE_MAX_MS) {
        return luaL_error(L, "glide_ms must be 0..2500");
    }
    const solar_os_synth_voice_performance_t performance = {
        .mono = lua_isnoneornil(L, 1)
                    ? SOLAR_OS_SYNTH_VOICE_DEFAULT_MONO
                    : lua_toboolean(L, 1) != 0,
        .glide_ms = (uint16_t)glide_ms,
    };
    return solua_check_esp(
        L,
        solar_os_synth_voice_configure_performance(SOLUA_SYNTH_OWNER,
                                                   &performance));
}

static int solua_synth_configure_filter(lua_State *L)
{
    solar_os_synth_voice_status_t status;
    solar_os_synth_voice_get_status(&status);
    solar_os_synth_voice_config_t config = status.config;
    config.filter = (solar_os_synth_filter_config_t){
        .cutoff_hz = solua_check_u32(L, 1),
        .resonance_percent = solua_optional_u8(
            L, 2, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RESONANCE_PERCENT),
        .envelope_amount_percent = solua_optional_u8(
            L,
            3,
            SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ENVELOPE_AMOUNT_PERCENT),
        .attack_ms = solua_optional_u32(
            L, 4, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ATTACK_MS),
        .decay_ms = solua_optional_u32(
            L, 5, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_DECAY_MS),
        .sustain_percent = solua_optional_u8(
            L, 6, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_SUSTAIN_PERCENT),
        .release_ms = solua_optional_u32(
            L, 7, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RELEASE_MS),
    };
    return solua_check_esp(
        L, solar_os_synth_voice_configure(SOLUA_SYNTH_OWNER, &config));
}

static int solua_synth_note_on(lua_State *L)
{
    return solua_check_esp(
        L,
        solar_os_synth_voice_note_on(
            SOLUA_SYNTH_OWNER,
            solua_check_u32(L, 1),
            solua_optional_u8(L,
                              2,
                              SOLAR_OS_SYNTH_VOICE_DEFAULT_VELOCITY)));
}

static int solua_synth_note_off(lua_State *L)
{
    return solua_check_esp(
        L,
        solar_os_synth_voice_note_off(SOLUA_SYNTH_OWNER,
                                      solua_check_u32(L, 1)));
}

static int solua_synth_all_notes_off(lua_State *L)
{
    return solua_check_esp(
        L, solar_os_synth_voice_all_notes_off(SOLUA_SYNTH_OWNER));
}

static int solua_synth_stop(lua_State *L)
{
    return solua_check_esp(L, solar_os_synth_voice_stop(SOLUA_SYNTH_OWNER));
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

#if SOLAR_OS_PACKAGE_SERVICE_HID
static int solua_hid_status(lua_State *L)
{
    solar_os_hid_status_t status;
    solar_os_hid_get_status(&status);
    lua_createtable(L, 0, 2);
    solua_set_bool(L, -1, "initialized", status.initialized);
    solua_set_bool(L, -1, "connected", status.connected);
    return 1;
}

static size_t solua_hid_keys(lua_State *L, uint16_t *keys, size_t capacity)
{
    const int count = lua_gettop(L);
    if (count < 1 || (size_t)count > capacity) {
        luaL_error(L, "expected 1..8 keys");
    }
    for (int index = 0; index < count; index++) {
        const lua_Integer value = luaL_checkinteger(L, index + 1);
        if (value < 0 || value > UINT16_MAX) {
            luaL_error(L, "invalid HID key");
        }
        keys[index] = (uint16_t)value;
    }
    return (size_t)count;
}

static int solua_hid_keyboard_press(lua_State *L)
{
    uint16_t keys[8];
    return solua_check_esp(L,
                           solar_os_hid_keyboard_press(
                               keys, solua_hid_keys(L, keys, 8)));
}

static int solua_hid_keyboard_release(lua_State *L)
{
    uint16_t keys[8];
    return solua_check_esp(L,
                           solar_os_hid_keyboard_release(
                               keys, solua_hid_keys(L, keys, 8)));
}

static int solua_hid_keyboard_release_all(lua_State *L)
{
    (void)L;
    return solua_check_esp(L, solar_os_hid_keyboard_release_all());
}

static int solua_hid_mouse_move(lua_State *L)
{
    const lua_Integer x = luaL_checkinteger(L, 1);
    const lua_Integer y = luaL_checkinteger(L, 2);
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
        return luaL_error(L, "mouse delta out of range");
    }
    return solua_check_esp(L, solar_os_hid_mouse_move((int32_t)x, (int32_t)y));
}

static int solua_hid_mouse_button(lua_State *L)
{
    const lua_Integer button = luaL_checkinteger(L, 1);
    if (button < 0 || button > UINT8_MAX) {
        return luaL_error(L, "invalid mouse button");
    }
    return solua_check_esp(L,
                           solar_os_hid_mouse_button((uint8_t)button,
                                                     lua_toboolean(L, 2)));
}

static int solua_hid_gamepad_axis(lua_State *L)
{
    const lua_Integer axis = luaL_checkinteger(L, 1);
    const lua_Integer value = luaL_checkinteger(L, 2);
    if (value < INT16_MIN || value > INT16_MAX) {
        return luaL_error(L, "expected axis value -32768..32767");
    }
    return solua_check_esp(L,
                           solar_os_hid_gamepad_axis((solar_os_hid_axis_t)axis,
                                                     (int16_t)value));
}

static int solua_hid_gamepad_button(lua_State *L)
{
    const lua_Integer button = luaL_checkinteger(L, 1);
    if (button < 0 || button > UINT8_MAX) {
        return luaL_error(L, "invalid gamepad button");
    }
    return solua_check_esp(L,
                           solar_os_hid_gamepad_button((uint8_t)button,
                                                       lua_toboolean(L, 2)));
}

static int solua_hid_gamepad_hat(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_hid_gamepad_hat(
                               (solar_os_hid_hat_t)luaL_checkinteger(L, 1)));
}

static int solua_hid_gamepad_send(lua_State *L)
{
    (void)L;
    return solua_check_esp(L, solar_os_hid_gamepad_send());
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

static int solua_identity_set_user(lua_State *L)
{
    return solua_check_esp(
        L,
        solar_os_identity_set_user(luaL_checkstring(L, 1)));
}

static int solua_identity_set_hostname(lua_State *L)
{
    return solua_check_esp(
        L,
        solar_os_identity_set_hostname(luaL_checkstring(L, 1)));
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

static int solua_net_tcp_connect(lua_State *L)
{
    uint32_t handle = 0;
    (void)solua_check_esp(
        L,
        solar_os_net_session_tcp_connect(
            solua_net_get(L),
            luaL_checkstring(L, 1),
            solua_net_port(L, 2, false),
            solua_net_timeout(L, 3, SOLAR_OS_NET_DEFAULT_CONNECT_TIMEOUT_MS),
            &handle));
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_net_tcp_send(lua_State *L)
{
    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 2, &data_len);
    return solua_check_esp(
        L,
        solar_os_net_session_tcp_send(
            solua_net_get(L),
            solua_check_u32(L, 1),
            data,
            data_len,
            solua_net_timeout(L, 3, SOLAR_OS_NET_DEFAULT_IO_TIMEOUT_MS)));
}

static int solua_net_tcp_receive(lua_State *L)
{
    const size_t max_bytes = solua_net_receive_size(L, 2);
    const uint32_t handle = solua_check_u32(L, 1);
    const uint32_t timeout_ms = solua_net_timeout(
        L, 3, SOLAR_OS_NET_DEFAULT_IO_TIMEOUT_MS);
    uint8_t *buffer = solua_net_buffer(L, max_bytes);
    solar_os_net_receive_result_t result;
    const esp_err_t err = solar_os_net_session_tcp_receive(
        solua_net_get(L),
        handle,
        buffer,
        max_bytes,
        timeout_ms,
        &result);
    if (err != ESP_OK) {
        solar_os_memory_free(buffer);
        return solua_check_esp(L, err);
    }
    if (result.timed_out) {
        solar_os_memory_free(buffer);
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buffer, result.data_len);
    solar_os_memory_free(buffer);
    return 1;
}

static int solua_net_udp_open(lua_State *L)
{
    const uint16_t local_port = lua_isnoneornil(L, 1) ? 0 :
        solua_net_port(L, 1, true);
    uint32_t handle = 0;
    (void)solua_check_esp(L,
                          solar_os_net_session_udp_open(solua_net_get(L),
                                                        local_port,
                                                        &handle));
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_net_udp_send(lua_State *L)
{
    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 4, &data_len);
    return solua_check_esp(
        L,
        solar_os_net_session_udp_send(
            solua_net_get(L),
            solua_check_u32(L, 1),
            luaL_checkstring(L, 2),
            solua_net_port(L, 3, false),
            data,
            data_len,
            solua_net_timeout(L, 5, SOLAR_OS_NET_DEFAULT_IO_TIMEOUT_MS)));
}

static int solua_net_udp_receive(lua_State *L)
{
    const size_t max_bytes = solua_net_receive_size(L, 2);
    const uint32_t handle = solua_check_u32(L, 1);
    const uint32_t timeout_ms = solua_net_timeout(
        L, 3, SOLAR_OS_NET_DEFAULT_IO_TIMEOUT_MS);
    uint8_t *buffer = solua_net_buffer(L, max_bytes);
    solar_os_net_receive_result_t result;
    const esp_err_t err = solar_os_net_session_udp_receive(
        solua_net_get(L),
        handle,
        buffer,
        max_bytes,
        timeout_ms,
        &result);
    if (err != ESP_OK) {
        solar_os_memory_free(buffer);
        return solua_check_esp(L, err);
    }
    if (result.timed_out) {
        solar_os_memory_free(buffer);
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushlstring(L, (const char *)buffer, result.data_len);
    lua_setfield(L, -2, "data");
    solua_set_str(L, -1, "address", result.address);
    solua_set_int(L, -1, "port", result.port);
    solua_set_bool(L, -1, "truncated", result.truncated);
    solua_set_int(L, -1, "datagram_bytes", result.message_len);
    solar_os_memory_free(buffer);
    return 1;
}

static int solua_net_websocket_connect(lua_State *L)
{
    const char *subprotocol = lua_isnoneornil(L, 2) ? NULL :
        luaL_checkstring(L, 2);
    uint32_t handle = 0;
    (void)solua_check_esp(
        L,
        solar_os_net_session_websocket_connect(
            solua_net_get(L),
            luaL_checkstring(L, 1),
            subprotocol,
            solua_net_timeout(L, 3, SOLAR_OS_NET_DEFAULT_CONNECT_TIMEOUT_MS),
            &handle));
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_net_websocket_send(lua_State *L)
{
    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 2, &data_len);
    return solua_check_esp(
        L,
        solar_os_net_session_websocket_send(
            solua_net_get(L),
            solua_check_u32(L, 1),
            data,
            data_len,
            !lua_isnoneornil(L, 3) && lua_toboolean(L, 3),
            solua_net_timeout(L, 4, SOLAR_OS_NET_DEFAULT_IO_TIMEOUT_MS)));
}

static int solua_net_websocket_receive(lua_State *L)
{
    const size_t max_bytes = solua_net_receive_size(L, 2);
    const uint32_t handle = solua_check_u32(L, 1);
    const uint32_t timeout_ms = solua_net_timeout(
        L, 3, SOLAR_OS_NET_DEFAULT_IO_TIMEOUT_MS);
    uint8_t *buffer = solua_net_buffer(L, max_bytes);
    solar_os_net_receive_result_t result;
    const esp_err_t err = solar_os_net_session_websocket_receive(
        solua_net_get(L),
        handle,
        buffer,
        max_bytes,
        timeout_ms,
        &result);
    if (err != ESP_OK) {
        solar_os_memory_free(buffer);
        return solua_check_esp(L, err);
    }
    if (result.timed_out) {
        solar_os_memory_free(buffer);
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushlstring(L, (const char *)buffer, result.data_len);
    lua_setfield(L, -2, "data");
    solua_set_str(L, -1, "type", solar_os_net_ws_opcode_name(result.opcode));
    solua_set_bool(L, -1, "final", result.final);
    solua_set_bool(L, -1, "closed", result.closed);
    solua_set_bool(L, -1, "truncated", result.truncated);
    solua_set_int(L, -1, "frame_bytes", result.message_len);
    solar_os_memory_free(buffer);
    return 1;
}

static int solua_net_close(lua_State *L)
{
    return solua_check_esp(L,
                           solar_os_net_session_close(solua_net_get(L),
                                                      solua_check_u32(L, 1)));
}

static int solua_net_close_all(lua_State *L)
{
    (void)L;
    if (solua_net_session != NULL) {
        solar_os_net_session_close_all(solua_net_session);
    }
    return 0;
}

static int solua_net_limits(lua_State *L)
{
    solar_os_net_session_status_t status;
    solar_os_net_session_get_status(solua_net_get(L), &status);
    lua_newtable(L);
    solua_set_str(L, -1, "owner", status.owner);
    solua_set_int(L, -1, "open_channels", status.open_channels);
    solua_set_int(L, -1, "session_channels", status.session_limit);
    solua_set_int(L, -1, "global_open_channels", status.global_open_channels);
    solua_set_int(L, -1, "global_channels", status.global_limit);
    solua_set_int(L, -1, "max_transfer_bytes", SOLAR_OS_NET_MAX_TRANSFER_BYTES);
    solua_set_int(L, -1, "max_udp_bytes", SOLAR_OS_NET_MAX_UDP_BYTES);
    solua_set_int(L, -1, "max_timeout_ms", SOLAR_OS_NET_MAX_TIMEOUT_MS);
    solua_set_int(L, -1, "poll_slice_ms", SOLAR_OS_NET_POLL_SLICE_MS);
    solua_set_bool(L, -1, "synchronous", true);
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

static int solua_ssh_keys_public_key(lua_State *L)
{
    char public_key[SOLAR_OS_SSH_PUBLIC_KEY_MAX];
    size_t public_key_len = 0;
    (void)solua_check_esp(L,
                          solar_os_ssh_keys_read_public(public_key,
                                                        sizeof(public_key),
                                                        &public_key_len));
    lua_pushlstring(L, public_key, public_key_len);
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

static solar_os_shell_charset_t solua_charset_from_arg(lua_State *L, int index)
{
    solar_os_shell_charset_t charset = SOLAR_OS_SHELL_CHARSET_UTF8;
    if (lua_isnoneornil(L, index)) {
        return charset;
    }
    if (!solar_os_shell_parse_charset(luaL_checkstring(L, index), &charset)) {
        luaL_error(L, "expected character set utf8 or ascii");
    }
    return charset;
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

    lua_getfield(L, index, "charset");
    if (!lua_isnil(L, -1)) {
        options->charset = solua_charset_from_arg(L, -1);
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
    if (argc < 1 || argc > 5) {
        return luaL_error(
            L,
            "usage: solaros.sessions.create_shell(port[, term[, cols, rows[, charset]]])");
    }

    solar_os_port_shell_options_t options = {
        .terminal_profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO,
        .charset = SOLAR_OS_SHELL_CHARSET_UTF8,
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
    if (argc >= 5 && !lua_isnoneornil(L, 5)) {
        options.charset = solua_charset_from_arg(L, 5);
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

static bool solua_input_source_info(solar_os_input_source_t source,
                                    solar_os_input_source_info_t *info)
{
    const size_t count = solar_os_input_source_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_input_source_info_t candidate;
        if (solar_os_input_source_get(i, &candidate) &&
            candidate.source == source) {
            if (info != NULL) {
                *info = candidate;
            }
            return true;
        }
    }
    return false;
}

static void solua_input_set_source(lua_State *L,
                                   solar_os_input_source_t source)
{
    solar_os_input_source_info_t info;
    solua_set_int(L, -1, "source", source);
    if (solua_input_source_info(source, &info)) {
        solua_set_str(L, -1, "source_name", info.name);
        solua_set_int(L, -1, "source_class", info.source_class);
        solua_set_str(L, -1, "source_class_name",
                      solar_os_input_source_class_name(info.source_class));
    } else {
        solua_set_str(L, -1, "source_name", "");
        solua_set_int(L, -1, "source_class", SOLAR_OS_INPUT_SOURCE_OTHER);
        solua_set_str(L, -1, "source_class_name", "other");
    }
}

static void solua_push_input_event(lua_State *L,
                                   const solar_os_event_t *event)
{
    lua_newtable(L);
    if (event->type == SOLAR_OS_EVENT_POINTER) {
        const solar_os_input_pointer_event_t *pointer = &event->data.pointer;
        solua_set_str(L, -1, "type", "pointer");
        solua_input_set_source(L, pointer->source);
        solua_set_int(L, -1, "pointer_id", pointer->pointer_id);
        solua_set_int(L, -1, "mode", pointer->mode);
        solua_set_str(L, -1, "mode_name",
                      solar_os_input_pointer_mode_name(pointer->mode));
        solua_set_int(L, -1, "action", pointer->action);
        solua_set_str(L, -1, "action_name",
                      solar_os_input_pointer_action_name(pointer->action));
        solua_set_int(L, -1, "x", pointer->x);
        solua_set_int(L, -1, "y", pointer->y);
        solua_set_int(L, -1, "delta_x", pointer->delta_x);
        solua_set_int(L, -1, "delta_y", pointer->delta_y);
        solua_set_int(L, -1, "buttons", pointer->buttons);
        solua_set_str(L, -1, "target", pointer->target);
    } else {
        const solar_os_input_axis_event_t *axis = &event->data.axis;
        solua_set_str(L, -1, "type", "axis");
        solua_input_set_source(L, axis->source);
        solua_set_int(L, -1, "axis", axis->axis);
        solua_set_str(L, -1, "axis_name",
                      solar_os_input_axis_name(axis->axis));
        solua_set_int(L, -1, "value", axis->value);
        solua_set_int(L, -1, "delta", axis->delta);
    }
}

static int solua_input_sources(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    const size_t count = solar_os_input_source_count();
    int out = 1;
    for (size_t i = 0; i < count; i++) {
        solar_os_input_source_info_t info;
        if (!solar_os_input_source_get(i, &info)) {
            continue;
        }
        lua_newtable(L);
        solua_set_int(L, -1, "source", info.source);
        solua_set_str(L, -1, "name", info.name);
        solua_set_int(L, -1, "source_class", info.source_class);
        solua_set_str(L, -1, "source_class_name",
                      solar_os_input_source_class_name(info.source_class));
        solua_set_int(L, -1, "capabilities", info.capabilities);
        solua_set_bool(L, -1, "ready", info.ready);
        lua_rawseti(L, list, out++);
    }
    return 1;
}

static int solua_input_read(lua_State *L)
{
    const uint32_t timeout_ms = solua_optional_u32(L, 1, 0U);
    if (timeout_ms > SOLUA_DEVICE_INPUT_READ_MAX_MS) {
        return luaL_error(L, "input read limited to 60000 ms");
    }
    if (solua.device_input == NULL) {
        lua_pushnil(L);
        return 1;
    }

    const TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    for (;;) {
        solar_os_event_t event;
        if (xQueueReceive(solua.device_input, &event, 0) == pdPASS) {
            solua_push_input_event(L, &event);
            return 1;
        }
        if (timeout_ms == 0U ||
            (xTaskGetTickCount() - started) >= timeout_ticks) {
            lua_pushnil(L);
            return 1;
        }
        if (solua_should_cancel(NULL)) {
            solua.interrupt_requested = true;
            solua.interrupted = true;
            return luaL_error(L, "interrupted");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int solua_input_clear(lua_State *L)
{
    size_t cleared = 0;
    solar_os_event_t event;
    if (solua.device_input != NULL) {
        while (xQueueReceive(solua.device_input, &event, 0) == pdPASS) {
            cleared++;
        }
    }
    lua_pushinteger(L, (lua_Integer)cleared);
    return 1;
}

static int solua_input_status(lua_State *L)
{
    const bool available = solua.device_input != NULL;
    uint32_t dropped;
    portENTER_CRITICAL(&solua_runtime_lock);
    dropped = solua.device_input_dropped;
    portEXIT_CRITICAL(&solua_runtime_lock);
    lua_newtable(L);
    solua_set_bool(L, -1, "available", available);
    solua_set_int(
        L, -1, "queued",
        available ? (lua_Integer)uxQueueMessagesWaiting(solua.device_input) : 0);
    solua_set_int(
        L, -1, "capacity", available ? SOLUA_DEVICE_INPUT_QUEUE_LEN : 0);
    solua_set_int(L, -1, "dropped", dropped);
    return 1;
}

static solar_os_shell_io_t *solua_current_io(void)
{
    return solua.session_io;
}

static solar_os_gfx_t *solua_current_gfx(void)
{
    if (solua.claimed_gfx != NULL) {
        return solua.claimed_gfx;
    }
    return solua.session_gfx;
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

static void solua_tui_text_event(lua_State *L,
                                 solua_event_type_t type,
                                 uint16_t row,
                                 uint16_t col,
                                 uint16_t width,
                                 const char *first,
                                 size_t first_len,
                                 const char *second,
                                 size_t second_len,
                                 uint8_t attr,
                                 bool selected,
                                 int32_t cursor,
                                 int32_t view)
{
    if (first_len + (second != NULL ? second_len + 1U : 0U) >= SOLUA_EVENT_DATA_MAX) {
        luaL_error(L, "tui text too long");
    }
    solua_event_t event = {
        .type = type, .row = row, .col = col, .width = width,
        .attr = attr, .success = selected, .x0 = cursor, .x1 = view,
        .data_len = first_len,
    };
    memcpy(event.data, first, first_len);
    event.data[first_len] = '\0';
    if (second != NULL) {
        memcpy(event.data + first_len + 1U, second, second_len);
        event.data[first_len + 1U + second_len] = '\0';
    }
    solua_ui_send_event(L, &event);
}

static void solua_tui_push_rect(lua_State *L, const solar_os_tui_rect_t *rect)
{
    lua_createtable(L, 4, 0);
    lua_pushinteger(L, (lua_Integer)rect->row); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, (lua_Integer)rect->col); lua_rawseti(L, -2, 2);
    lua_pushinteger(L, (lua_Integer)rect->height); lua_rawseti(L, -2, 3);
    lua_pushinteger(L, (lua_Integer)rect->width); lua_rawseti(L, -2, 4);
}

static int solua_tui_layout(lua_State *L)
{
    const size_t tabs = lua_isnoneornil(L, 1) ? 0 : solua_check_size(L, 1);
    const size_t status = lua_isnoneornil(L, 2) ? 0 : solua_check_size(L, 2);
    const size_t input = lua_isnoneornil(L, 3) ? 0 : solua_check_size(L, 3);
    solar_os_shell_io_t *io = solua_current_io();
    solar_os_tui_screen_layout_t layout;
    if (io == NULL || !solar_os_tui_layout_compute(solar_os_shell_io_rows(io),
                                                    solar_os_shell_io_cols(io),
                                                    tabs, status, input, &layout)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 6, 0);
    const solar_os_tui_rect_t *rects[] = {
        &layout.title, &layout.tabs, &layout.body,
        &layout.status, &layout.input, &layout.help,
    };
    for (int i = 0; i < 6; i++) {
        solua_tui_push_rect(L, rects[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int solua_tui_cell(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 4, &len);
    solua_tui_text_event(L, SOLUA_EVENT_TUI_CELL,
                         solua_check_u16_size(L, 1), solua_check_u16_size(L, 2),
                         solua_check_u16_size(L, 3), text, len, NULL, 0,
                         solua_optional_tui_attr(L, 5), false, 0, 0);
    return 0;
}

static int solua_tui_title(lua_State *L)
{
    size_t title_len = 0, detail_len = 0;
    const char *title = luaL_checklstring(L, 1, &title_len);
    const char *detail = lua_isnoneornil(L, 2) ? "" : luaL_checklstring(L, 2, &detail_len);
    solua_tui_text_event(L, SOLUA_EVENT_TUI_TITLE, 0, 0, 0, title, title_len,
                         detail, detail_len, 0, false, 0, 0);
    return 0;
}

static int solua_tui_help(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    solua_tui_text_event(L, SOLUA_EVENT_TUI_HELP, 0, 0, 0, text, len,
                         NULL, 0, 0, false, 0, 0);
    return 0;
}

static int solua_tui_tab(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 4, &len);
    solua_tui_text_event(L, SOLUA_EVENT_TUI_TAB,
                         solua_check_u16_size(L, 1), solua_check_u16_size(L, 2),
                         solua_check_u16_size(L, 3), text, len, NULL, 0, 0,
                         lua_toboolean(L, 5), 0, 0);
    return 0;
}

static int solua_tui_list_move(lua_State *L)
{
    solar_os_tui_viewport_t viewport = {
        .cursor = solua_check_size(L, 1), .top = solua_check_size(L, 2),
    };
    const bool moved = solar_os_tui_viewport_key(&viewport, solua_check_u8(L, 5),
        solua_check_size(L, 3), solua_check_size(L, 4),
        !lua_isnoneornil(L, 6) && lua_toboolean(L, 6));
    lua_pushinteger(L, (lua_Integer)viewport.cursor);
    lua_pushinteger(L, (lua_Integer)viewport.top);
    lua_pushboolean(L, moved);
    return 3;
}

static int solua_tui_input_edit(lua_State *L)
{
    size_t len = 0;
    const char *source = luaL_checklstring(L, 1, &len);
    size_t capacity = solua_check_size(L, 6);
    if (capacity > SOLUA_EVENT_DATA_MAX) capacity = SOLUA_EVENT_DATA_MAX;
    if (capacity < len + 1U) capacity = len + 1U;
    if (capacity > SOLUA_EVENT_DATA_MAX) return luaL_error(L, "input too long");
    char text[SOLUA_EVENT_DATA_MAX];
    memcpy(text, source, len);
    text[len] = '\0';
    solar_os_tui_input_state_t state = {
        .cursor = solua_check_size(L, 2), .view = solua_check_size(L, 3),
    };
    const solar_os_tui_input_action_t action = solar_os_tui_input_key(
        text, capacity, &state, solua_check_u32(L, 4), solua_check_size(L, 5));
    lua_pushstring(L, text);
    lua_pushinteger(L, (lua_Integer)state.cursor);
    lua_pushinteger(L, (lua_Integer)state.view);
    lua_pushinteger(L, (lua_Integer)action);
    return 4;
}

static int solua_tui_input(lua_State *L)
{
    size_t label_len = 0, text_len = 0;
    const char *label = luaL_checklstring(L, 4, &label_len);
    const char *text = luaL_checklstring(L, 5, &text_len);
    solua_tui_text_event(L, SOLUA_EVENT_TUI_INPUT,
                         solua_check_u16_size(L, 1), solua_check_u16_size(L, 2),
                         solua_check_u16_size(L, 3), label, label_len, text, text_len,
                         solua_optional_tui_attr(L, 8),
                         !lua_isnoneornil(L, 9) && lua_toboolean(L, 9),
                         (int32_t)solua_check_size(L, 6),
                         (int32_t)solua_check_size(L, 7));
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
        if (solua.session_gfx == NULL) {
            return luaL_error(
                L,
                "no foreground display; pass a verified target to gfx.begin(name)");
        }
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
        .attr = color,
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
        .attr = solua_gfx_color_from_arg(L, 1),
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

static int solua_gfx_rgb(lua_State *L)
{
    const lua_Integer red = luaL_checkinteger(L, 1);
    const lua_Integer green = luaL_checkinteger(L, 2);
    const lua_Integer blue = luaL_checkinteger(L, 3);
    if (red < 0 || red > 255 || green < 0 || green > 255 ||
        blue < 0 || blue > 255) {
        return luaL_error(L, "RGB components must be 0..255");
    }
    lua_pushinteger(L, solar_os_gfx_rgb((uint8_t)red, (uint8_t)green,
                                       (uint8_t)blue));
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

static int solua_gfx_bitmap(lua_State *L)
{
    const uint16_t width = solua_check_u16_size(L, 3);
    const uint16_t height = solua_check_u16_size(L, 4);
    if (width == 0 || height == 0) {
        return luaL_error(L, "bitmap dimensions must be positive");
    }

    const size_t required = (((size_t)width + 7U) / 8U) * (size_t)height;
    if (required > SOLUA_EVENT_DATA_MAX) {
        return luaL_error(L, "bitmap too large (maximum %u packed bytes)",
                          (unsigned)SOLUA_EVENT_DATA_MAX);
    }

    size_t len = 0;
    const char *bitmap = luaL_checklstring(L, 5, &len);
    if (len != required) {
        return luaL_error(L, "bitmap data must contain exactly %u packed bytes",
                          (unsigned)required);
    }

    solua_event_t event = {
        .type = SOLUA_EVENT_GFX_BITMAP,
        .x0 = (int32_t)luaL_checkinteger(L, 1),
        .y0 = (int32_t)luaL_checkinteger(L, 2),
        .width = width,
        .height = height,
        .data_len = len,
    };
    memcpy(event.data, bitmap, len);
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

static void solua_push_contact(lua_State *L,
                               const solar_os_contact_t *contact)
{
    lua_newtable(L);
    lua_pushinteger(L, contact->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, contact->display_name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, contact->flags);
    lua_setfield(L, -2, "flags");
    lua_pushstring(L, solar_os_contact_trust_name(contact->primary_trust));
    lua_setfield(L, -2, "trust");
    lua_pushstring(
        L,
        solar_os_messaging_provider_name(contact->primary_provider));
    lua_setfield(L, -2, "provider");
    lua_newtable(L);
    solar_os_endpoint_t *endpoints =
        solar_os_memory_calloc(SOLAR_OS_ENDPOINT_CAPACITY,
                               sizeof(*endpoints),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "lua.contacts.endpoints");
    if (endpoints != NULL) {
        const size_t count =
            solar_os_contacts_endpoint_snapshot(contact->id,
                                                endpoints,
                                                SOLAR_OS_ENDPOINT_CAPACITY);
        for (size_t i = 0; i < count; i++) {
            lua_pushinteger(L, endpoints[i].id);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        solar_os_memory_free(endpoints);
    }
    lua_setfield(L, -2, "endpoint_ids");
}

static int solua_contacts_list(lua_State *L)
{
    solar_os_contact_t *contacts =
        solar_os_memory_calloc(SOLAR_OS_CONTACT_CAPACITY,
                               sizeof(*contacts),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "lua.contacts.list");
    if (contacts == NULL) {
        return solua_check_esp(L, ESP_ERR_NO_MEM);
    }
    const size_t count =
        solar_os_contacts_snapshot(contacts,
                                   SOLAR_OS_CONTACT_CAPACITY,
                                   false,
                                   SOLAR_OS_CONTACT_TRUST_DISCOVERED,
                                   NULL);
    lua_newtable(L);
    for (size_t i = 0; i < count; i++) {
        solua_push_contact(L, &contacts[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    solar_os_memory_free(contacts);
    return 1;
}

static int solua_contacts_get(lua_State *L)
{
    solar_os_contact_t contact;
    const esp_err_t error =
        solar_os_contacts_get((uint32_t)luaL_checkinteger(L, 1), &contact);
    if (error != ESP_OK) {
        return solua_check_esp(L, error);
    }
    solua_push_contact(L, &contact);
    return 1;
}

static void solua_push_conversation(
    lua_State *L,
    const solar_os_messaging_conversation_t *conversation)
{
    lua_newtable(L);
    lua_pushinteger(L, conversation->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(
        L,
        solar_os_messaging_provider_name(conversation->provider));
    lua_setfield(L, -2, "provider");
    lua_pushstring(L, solar_os_conversation_kind_name(conversation->kind));
    lua_setfield(L, -2, "kind");
    lua_pushstring(L, conversation->title);
    lua_setfield(L, -2, "title");
    lua_pushinteger(L, conversation->contact_id);
    lua_setfield(L, -2, "contact_id");
    lua_pushinteger(L, conversation->endpoint_id);
    lua_setfield(L, -2, "endpoint_id");
    lua_pushinteger(L, conversation->group_ref);
    lua_setfield(L, -2, "group_ref");
    lua_pushinteger(L, conversation->unread_count);
    lua_setfield(L, -2, "unread");
    lua_pushinteger(L, (lua_Integer)conversation->last_message_ms);
    lua_setfield(L, -2, "last_message_ms");
    lua_pushinteger(L, conversation->security_flags);
    lua_setfield(L, -2, "security_flags");
}

static int solua_messages_conversations(lua_State *L)
{
    solar_os_messaging_conversation_t *conversations =
        solar_os_memory_calloc(SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY,
                               sizeof(*conversations),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "lua.messages.conversations");
    if (conversations == NULL) {
        return solua_check_esp(L, ESP_ERR_NO_MEM);
    }
    const size_t count = solar_os_messaging_conversation_snapshot(
        conversations,
        SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY);
    lua_newtable(L);
    for (size_t i = 0; i < count; i++) {
        solua_push_conversation(L, &conversations[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    solar_os_memory_free(conversations);
    return 1;
}

typedef struct {
    lua_State *state;
    size_t index;
} solua_messages_list_context_t;

static bool solua_messages_list_visit(
    const solar_os_messaging_message_t *message,
    void *user)
{
    solua_messages_list_context_t *context = user;
    lua_State *L = context->state;
    lua_newtable(L);
    char key[17];
    snprintf(key, sizeof(key), "%016" PRIx64, message->key);
    lua_pushstring(L, key);
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, message->conversation_id);
    lua_setfield(L, -2, "conversation_id");
    lua_pushstring(
        L,
        message->direction == SOLAR_OS_MESSAGE_INBOUND ? "inbound" :
                                                        "outbound");
    lua_setfield(L, -2, "direction");
    lua_pushstring(L, solar_os_delivery_state_name(message->delivery));
    lua_setfield(L, -2, "delivery");
    lua_pushstring(L, message->sender);
    lua_setfield(L, -2, "sender");
    lua_pushstring(L, message->body);
    lua_setfield(L, -2, "body");
    lua_pushinteger(L, (lua_Integer)message->timestamp_ms);
    lua_setfield(L, -2, "timestamp_ms");
    lua_pushinteger(L, message->security_flags);
    lua_setfield(L, -2, "security_flags");
    lua_pushboolean(L, message->unread);
    lua_setfield(L, -2, "unread");
    lua_pushboolean(L, message->truncated);
    lua_setfield(L, -2, "truncated");
    lua_pushstring(L, message->error);
    lua_setfield(L, -2, "error");
    lua_rawseti(L, -2, (lua_Integer)++context->index);
    return true;
}

static int solua_messages_list(lua_State *L)
{
    const uint32_t conversation_id =
        (uint32_t)luaL_checkinteger(L, 1);
    lua_newtable(L);
    solua_messages_list_context_t context = {
        .state = L,
    };
    (void)solar_os_messaging_message_visit(conversation_id,
                                           0,
                                           solua_messages_list_visit,
                                           &context,
                                           NULL);
    return 1;
}

static int solua_messages_send(lua_State *L)
{
    const uint32_t conversation_id =
        (uint32_t)luaL_checkinteger(L, 1);
    const char *body = luaL_checkstring(L, 2);
    const bool allow_untrusted =
        lua_gettop(L) >= 3 ? lua_toboolean(L, 3) : false;
    solar_os_message_key_t key = 0;
    const esp_err_t error =
        solar_os_messaging_send(conversation_id,
                                body,
                                allow_untrusted,
                                &key);
    if (error != ESP_OK) {
        return solua_check_esp(L, error);
    }
    char text[17];
    snprintf(text, sizeof(text), "%016" PRIx64, key);
    lua_pushstring(L, text);
    return 1;
}

static int solua_messages_mark_read(lua_State *L)
{
    return solua_check_esp(
        L,
        solar_os_messaging_mark_read(
            (uint32_t)luaL_checkinteger(L, 1)));
}

static int solua_messages_cancel(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    char *end = NULL;
    const unsigned long long key = strtoull(text, &end, 16);
    if (end == text || *end != '\0' || key == 0) {
        return luaL_error(L, "expected hexadecimal message id");
    }
    return solua_check_esp(L, solar_os_messaging_cancel((uint64_t)key));
}

static void solua_new_submodule(lua_State *L, int parent, const char *name)
{
    parent = lua_absindex(L, parent);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, parent, name);
}

#if SOLAR_OS_PACKAGE_SERVICE_DSP
#include "solar_os_lua_dsp.inc"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_FTP
#include "solar_os_lua_ftp.inc"
#endif

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
#if SOLAR_OS_PACKAGE_SERVICE_DSP
    solua_dsp_register_type(L);
#endif
    lua_newtable(L);
    const int solaros = lua_gettop(L);
    solua_set_func(L, solaros, "write", solua_solaros_write);
    solua_set_func(L, solaros, "version", solua_solaros_version);
    solua_set_func(L, solaros, "should_exit", solua_solaros_should_exit);
    solua_set_func(L, solaros, "tick_interval", solua_solaros_tick_interval);
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solua_set_func(L, solaros, "battery_status", solua_solaros_battery_status);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solua_set_func(L, solaros, "wifi_status", solua_solaros_wifi_status_short);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    solua_set_func(L, solaros, "environment", solua_solaros_environment);
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
#define SOLAR_OS_SCRIPT_API_FUNCTION_NAMED( \
    module_name, public_name, python_native, lua_native) \
    solua_set_func(L, \
                   script_module, \
                   SOLAR_OS_SCRIPT_API_STRINGIFY(public_name), \
                   lua_native)
#define SOLAR_OS_SCRIPT_API_SUBMODULE_BEGIN(module_name, submodule_name) \
    { \
        solua_new_submodule( \
            L, script_module, SOLAR_OS_SCRIPT_API_STRINGIFY(submodule_name)); \
        const int script_submodule = lua_gettop(L)
#define SOLAR_OS_SCRIPT_API_SUBMODULE_FUNCTION( \
    module_name, submodule_name, public_name, native_name) \
    solua_set_func( \
        L, \
        script_submodule, \
        SOLAR_OS_SCRIPT_API_STRINGIFY(public_name), \
        solua_##module_name##_##submodule_name##_##native_name)
#define SOLAR_OS_SCRIPT_API_SUBMODULE_END(module_name, submodule_name) \
        lua_pop(L, 1); \
    }
#define SOLAR_OS_SCRIPT_API_MODULE_END(module_name) \
        lua_pop(L, 1); \
    }
#include "solar_os_script_api.inc"
#undef SOLAR_OS_SCRIPT_API_MODULE_END
#undef SOLAR_OS_SCRIPT_API_SUBMODULE_END
#undef SOLAR_OS_SCRIPT_API_SUBMODULE_FUNCTION
#undef SOLAR_OS_SCRIPT_API_SUBMODULE_BEGIN
#undef SOLAR_OS_SCRIPT_API_FUNCTION_NAMED
#undef SOLAR_OS_SCRIPT_API_FUNCTION
#undef SOLAR_OS_SCRIPT_API_UINT
#undef SOLAR_OS_SCRIPT_API_INT
#undef SOLAR_OS_SCRIPT_API_MODULE_BEGIN
#undef SOLAR_OS_SCRIPT_API_STRINGIFY
#undef SOLAR_OS_SCRIPT_API_STRINGIFY_INNER

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

esp_err_t solar_os_lua_run(const solar_os_script_run_request_t *request,
                           solar_os_script_run_result_t *result)
{
    solar_os_script_run_control_t control;
    esp_err_t err = solar_os_script_run_begin(request, result, &control);
    if (err != ESP_OK) {
        return err;
    }
    if (!solua_runtime_claim(SOLUA_RUNTIME_OWNER_RUNNER)) {
        solar_os_script_run_error(&control,
                                  ESP_ERR_INVALID_STATE,
                                  "Lua runtime is already in use");
        return result->status;
    }

    memset(&solua, 0, sizeof(solua));
    solua.ctx = request->context;
    solua.argc = request->argc;
    for (int i = 0; i < request->argc; i++) {
        if (request->argv[i] == NULL ||
            strlcpy(solua.argv[i],
                    request->argv[i],
                    sizeof(solua.argv[i])) >= sizeof(solua.argv[i])) {
            solar_os_script_run_error(&control,
                                      ESP_ERR_INVALID_ARG,
                                      "Lua argument is too long");
            goto cleanup;
        }
    }

    size_t source_len = request->input_len;
    if (request->input_type == SOLAR_OS_SCRIPT_INPUT_SOURCE) {
        if (source_len == 0) {
            source_len = strlen(request->input);
        }
        if (source_len > SOLAR_OS_SCRIPT_SOURCE_MAX_BYTES) {
            solar_os_script_run_error(&control,
                                      ESP_ERR_INVALID_SIZE,
                                      "Lua source is too large");
            goto cleanup;
        }
    } else if (request->input_type == SOLAR_OS_SCRIPT_INPUT_FILE) {
        struct stat st;
        if (stat(request->input, &st) != 0 || !S_ISREG(st.st_mode)) {
            solar_os_script_run_error(&control, ESP_ERR_NOT_FOUND, "Lua file not found");
            goto cleanup;
        }
        if (st.st_size < 0 ||
            (uint64_t)st.st_size > SOLAR_OS_SCRIPT_SOURCE_MAX_BYTES) {
            solar_os_script_run_error(&control,
                                      ESP_ERR_INVALID_SIZE,
                                      "Lua source is too large");
            goto cleanup;
        }
    } else {
        solar_os_script_run_error(&control, ESP_ERR_INVALID_ARG, "invalid Lua input type");
        goto cleanup;
    }

    solua_runner_control = &control;
    lua_State *L = lua_newstate(solua_alloc, NULL);
    if (L == NULL) {
        solar_os_script_run_error(&control, ESP_ERR_NO_MEM, "Lua heap allocation failed");
        goto cleanup;
    }

    lua_atpanic(L, solua_panic);
    solua_open_libs(L);
    solua_open_solaros(L);
    lua_sethook(L, solua_hook, LUA_MASKCOUNT, SOLUA_HOOK_INSTRUCTION_COUNT);
    solua_set_args(L);

    int status = LUA_ERRRUN;
    if (solar_os_script_run_should_cancel(&control)) {
        status = LUA_ERRRUN;
    } else if (request->input_type == SOLAR_OS_SCRIPT_INPUT_FILE) {
        status = luaL_loadfilex(L, request->input, "t");
    } else {
        status = luaL_loadbufferx(L,
                                  request->input,
                                  source_len,
                                  request->source_name != NULL
                                      ? request->source_name
                                      : "=script",
                                  "t");
    }

    bool success = false;
    if (status == LUA_OK) {
        success = solua_call_loaded(L, false);
    } else if (!result->cancelled && !result->timed_out) {
        solua_report_error(lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    if (result->timed_out) {
        solar_os_script_run_error(&control, ESP_ERR_TIMEOUT, "Lua deadline exceeded");
    } else if (result->cancelled) {
        solar_os_script_run_error(&control, ESP_ERR_INVALID_STATE, "Lua run cancelled");
    } else if (success) {
        result->success = true;
        result->status = ESP_OK;
    } else if (result->status == ESP_OK) {
        solar_os_script_run_error(&control, ESP_FAIL, "Lua execution failed");
    }

#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
    (void)solar_os_synth_voice_stop(SOLUA_SYNTH_OWNER);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
    solua_midi_destroy();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
    solua_net_destroy();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
    solua_http_stream_destroy();
    solua_http_session_destroy();
#endif
    solar_os_rtc_release_owner("lua");
    lua_close(L);

cleanup:
#if SOLAR_OS_PACKAGE_SERVICE_HID
    solar_os_hid_release_all();
#endif
    solua_runner_control = NULL;
    solua_runtime_release(SOLUA_RUNTIME_OWNER_RUNNER);
    return result->status;
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
#if SOLAR_OS_PACKAGE_SERVICE_HID
        solar_os_hid_release_all();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
        (void)solar_os_synth_voice_stop(SOLUA_SYNTH_OWNER);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
        solua_midi_destroy();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
        solua_net_destroy();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
        solua_http_stream_destroy();
        solua_http_session_destroy();
#endif
        solar_os_rtc_release_owner("lua");
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

static void solua_write_output_line(solar_os_context_t *ctx,
                                    solar_os_shell_io_t *io,
                                    const char *text)
{
    (void)ctx;
    solar_os_shell_io_writeln(io, text);
}

static void solua_render_usage(solar_os_context_t *ctx,
                               solar_os_shell_io_t *io)
{
    solua_write_output_line(ctx, io, "usage: lua [file.lua] [args...]");
    solua_write_output_line(ctx, io, "  lua");
    solua_write_output_line(ctx, io, "  lua hello.lua");
    solua_write_output_line(ctx, io, "  lua /sdcard/apps/demo/main.lua arg");
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
    const int argc = solar_os_context_argc(ctx);
    const bool repl_mode = argc < 2;
    solar_os_context_set_app_class(
        ctx,
        repl_mode ? SOLAR_OS_APP_CLASS_TUI : SOLAR_OS_APP_CLASS_COMMAND);
    solar_os_shell_io_t *io = solua_io(ctx);
    if (!solua_runtime_claim(SOLUA_RUNTIME_OWNER_APP)) {
        solar_os_shell_io_writeln(io, "lua: runtime is already in use");
        solar_os_shell_io_flush(io);
        solua_return_to_shell(ctx, 1, "lua: runtime is already in use");
        return ESP_OK;
    }

    memset(&solua, 0, sizeof(solua));
    solua.ctx = ctx;
    solua.session_terminal = solar_os_context_terminal(ctx);
    solua.session_gfx = solar_os_context_gfx(ctx);

    io = solua_io(ctx);
    solua.session_io = io;
    if (argc > SOLAR_OS_APP_ARG_MAX) {
        solar_os_shell_io_writeln(io, "lua: too many arguments");
        solar_os_shell_io_flush(io);
        solua_return_to_shell(ctx, 2, "lua: too many arguments");
        solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
        return ESP_OK;
    }

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
            solua_render_usage(ctx, io);
            solua_return_to_shell(ctx, 2, NULL);
            solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
            return ESP_OK;
        }

        esp_err_t path_err = solar_os_storage_resolve_path(script_arg,
                                                           solua.path,
                                                           sizeof(solua.path));
        if (path_err != ESP_OK) {
            solar_os_shell_io_printf(io, "lua: invalid path: %s\n", esp_err_to_name(path_err));
            solar_os_shell_io_flush(io);
            char message[96];
            snprintf(message,
                     sizeof(message),
                     "lua: invalid path: %s",
                     esp_err_to_name(path_err));
            solua_return_to_shell(ctx, 1, message);
            solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
            return ESP_OK;
        }
        if (!solua_path_has_suffix(solua.path, ".lua")) {
            solar_os_shell_io_writeln(io, "lua: expected .lua file");
            solar_os_shell_io_flush(io);
            solua_return_to_shell(ctx, 2, "lua: expected .lua file");
            solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
            return ESP_OK;
        }

        struct stat st;
        if (stat(solua.path, &st) != 0 || !S_ISREG(st.st_mode)) {
            solar_os_shell_io_printf(io, "lua: not found: %s\n", solua.path);
            solar_os_shell_io_flush(io);
            char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
            snprintf(message, sizeof(message), "lua: not found: %s", solua.path);
            solua_return_to_shell(ctx, 1, message);
            solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
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
            solua_return_to_shell(ctx, 1, "lua: out of memory");
        }
        solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
        return ESP_OK;
    }

    solua.device_input = solar_os_queue_create(
        SOLUA_DEVICE_INPUT_QUEUE_LEN, sizeof(solar_os_event_t));
    if (solua.device_input == NULL) {
        solar_os_queue_delete(solua.events);
        solua.events = NULL;
        solar_os_shell_io_writeln(io, "lua: out of memory");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            solua_return_to_shell(ctx, 1, "lua: out of memory");
        }
        solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
        return ESP_OK;
    }

    solua.key_input = solar_os_queue_create(SOLUA_KEY_QUEUE_LEN, sizeof(char));
    if (solua.key_input == NULL) {
        solar_os_queue_delete(solua.device_input);
        solua.device_input = NULL;
        solar_os_queue_delete(solua.events);
        solua.events = NULL;
        solar_os_shell_io_writeln(io, "lua: out of memory");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            solua_return_to_shell(ctx, 1, "lua: out of memory");
        }
        solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
        return ESP_OK;
    }

    if (repl_mode) {
        solua.input = solar_os_queue_create(SOLUA_INPUT_QUEUE_LEN,
                                             sizeof(solua_input_t));
        if (solua.input == NULL) {
            solar_os_queue_delete(solua.key_input);
            solua.key_input = NULL;
            solar_os_queue_delete(solua.device_input);
            solua.device_input = NULL;
            solar_os_queue_delete(solua.events);
            solua.events = NULL;
            solar_os_shell_io_writeln(io, "lua: out of memory");
            solar_os_shell_io_flush(io);
            solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
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
        if (solua.device_input != NULL) {
            solar_os_queue_delete(solua.device_input);
            solua.device_input = NULL;
        }
        solar_os_queue_delete(solua.events);
        solua.events = NULL;
        solua.running = false;
        solar_os_shell_io_writeln(io, "lua: task create failed");
        solar_os_shell_io_flush(io);
        solua_return_to_shell(ctx, 1, "lua: task create failed");
        solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
    }

    return ESP_OK;
}

static void solua_interrupt_current(void)
{
    solua.interrupt_requested = true;
    solua.interrupted = true;
}

static bool solua_task_stopped(void *user)
{
    (void)user;
    return solua.task == NULL || solua.task_done;
}

static void solua_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (!solua_runtime_is_owned_by(SOLUA_RUNTIME_OWNER_APP)) {
        return;
    }

    solua.stop_requested = true;
    if (solua.input != NULL) {
        solua_input_t input = {
            .exit = true,
        };
        (void)xQueueSend(solua.input, &input, 0);
    }

    if (solua.task != NULL && !solua.task_done) {
        if (!solar_os_script_wait_for_stop(solua_task_stopped,
                                           NULL,
                                           SOLUA_STOP_WAIT_MS,
                                           20U)) {
            SOLAR_OS_LOGW(TAG, "force stopping unresponsive Lua task");
            solar_os_task_delete(solua.task);
            solua.task = NULL;
            solua.task_done = true;
            solua.vm_active = false;
        }
    }

    if (solua.tui_active) {
        solar_os_tui_end(&solua.tui);
        solua.tui_active = false;
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
    if (solua.device_input != NULL) {
        solar_os_queue_delete(solua.device_input);
        solua.device_input = NULL;
    }
    solua_gfx_release_target();
#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
    (void)solar_os_synth_voice_stop(SOLUA_SYNTH_OWNER);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HID
    solar_os_hid_release_all();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
    solua_midi_destroy();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
    solua_net_destroy();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
    solua_http_stream_destroy();
    solua_http_session_destroy();
#endif
    solua_runtime_release(SOLUA_RUNTIME_OWNER_APP);
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
    if (event == NULL) {
        return;
    }
    if (!solua.tui_active) {
        if (solar_os_tui_screen_begin(&solua.tui, ctx) != ESP_OK) {
            return;
        }
        solua.tui_active = true;
    }
    solar_os_tui_t *tui = &solua.tui;

    switch (event->type) {
    case SOLUA_EVENT_TUI_CLEAR:
        solar_os_tui_clear(tui);
        break;
    case SOLUA_EVENT_TUI_REFRESH:
        solar_os_tui_refresh(tui);
        break;
    case SOLUA_EVENT_TUI_MOVE:
        solar_os_tui_move(tui, event->row, event->col);
        break;
    case SOLUA_EVENT_TUI_WRITE:
        solar_os_tui_write(tui, event->data, event->attr);
        break;
    case SOLUA_EVENT_TUI_PUTCH:
        solar_os_tui_putch(tui, event->row, event->col, event->codepoint, event->attr);
        break;
    case SOLUA_EVENT_TUI_HLINE:
        solar_os_tui_hline(tui, event->row, event->col, event->width, 0, event->attr);
        break;
    case SOLUA_EVENT_TUI_VLINE:
        solar_os_tui_vline(tui, event->row, event->col, event->height, 0, event->attr);
        break;
    case SOLUA_EVENT_TUI_VRULE:
        solar_os_tui_vrule(tui, event->row, event->col, event->height, event->width, event->attr);
        break;
    case SOLUA_EVENT_TUI_BOX:
        solar_os_tui_box(tui, event->row, event->col, event->height, event->width, event->attr);
        break;
    case SOLUA_EVENT_TUI_FILL:
        solar_os_tui_fill(tui,
                          event->row,
                          event->col,
                          event->height,
                          event->width,
                          event->codepoint,
                          event->attr);
        break;
    case SOLUA_EVENT_TUI_CELL:
        solar_os_tui_write_cell(tui, event->row, event->col, event->width,
                                event->data, event->attr);
        break;
    case SOLUA_EVENT_TUI_TITLE:
        solar_os_tui_draw_title(tui, event->data,
                                event->data_len + 1U < sizeof(event->data) ?
                                    event->data + event->data_len + 1U : "");
        break;
    case SOLUA_EVENT_TUI_HELP:
        solar_os_tui_draw_help(tui, event->data);
        break;
    case SOLUA_EVENT_TUI_TAB:
        solar_os_tui_draw_tab(tui, event->row, event->col, event->width,
                              event->data, event->success);
        break;
    case SOLUA_EVENT_TUI_INPUT: {
        solar_os_tui_input_state_t state = {
            .cursor = event->x0 >= 0 ? (size_t)event->x0 : 0,
            .view = event->x1 >= 0 ? (size_t)event->x1 : 0,
        };
        const char *text = event->data_len + 1U < sizeof(event->data) ?
            event->data + event->data_len + 1U : "";
        solar_os_tui_draw_input_ex(tui, event->row, event->col, event->width,
                                   event->data, text, &state, event->attr,
                                   event->success);
        break;
    }
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
        if (solua.session_terminal != NULL) {
            solar_os_terminal_draw(solua.session_terminal);
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
    case SOLUA_EVENT_GFX_BITMAP:
        solar_os_gfx_bitmap(gfx,
                            (int)event->x0,
                            (int)event->y0,
                            (int)event->width,
                            (int)event->height,
                            (const uint8_t *)event->data);
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
    uint32_t drain_limit = SOLUA_DRAIN_EVENTS_PER_TICK;
    while (drained < drain_limit &&
           xQueueReceive(solua.events, &event, 0) == pdPASS) {
        drained++;
        if (event.type >= SOLUA_EVENT_TUI_CLEAR &&
            event.type <= SOLUA_EVENT_TUI_INPUT) {
            drain_limit = event.type == SOLUA_EVENT_TUI_REFRESH ?
                SOLUA_DRAIN_EVENTS_PER_TICK :
                SOLUA_DRAIN_TUI_EVENTS_PER_TICK;
        }
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
        case SOLUA_EVENT_TUI_CELL:
        case SOLUA_EVENT_TUI_TITLE:
        case SOLUA_EVENT_TUI_HELP:
        case SOLUA_EVENT_TUI_TAB:
        case SOLUA_EVENT_TUI_INPUT:
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
        case SOLUA_EVENT_GFX_BITMAP:
        case SOLUA_EVENT_GFX_TEXT:
            solua_apply_gfx_event(ctx, &event);
            break;
        case SOLUA_EVENT_DONE:
            solua.running = false;
            solua.task_done = true;
            if (solua.tui_active) {
                solar_os_tui_end(&solua.tui);
                solua.tui_active = false;
            }
            solua_gfx_release_target();
            solar_os_context_set_graphics_active(ctx, false);
            if (solua.mode == SOLUA_MODE_SCRIPT || solua.repl_exit_requested) {
                solua_finish_terminal_line(ctx, io);
                solua_flush_io(ctx, io);
                const int exit_code = solua.interrupted ? 130 :
                    (solua.exit_code != 0 ? solua.exit_code :
                        (event.success ? 0 : 1));
                solua_return_to_shell(
                    ctx,
                    exit_code,
                    event.success ? NULL :
                        (solua.interrupted ? "lua: stopped" : "lua: failed"));
                break;
            }
            solua_finish_terminal_line(ctx, io);
            solua_flush_io(ctx, io);
            solua_return_to_shell(
                ctx,
                event.success ? 0 : 1,
                event.success ? "lua: stopped" : "lua: failed");
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

static void solua_queue_device_input(const solar_os_event_t *event)
{
    if (event == NULL || solua.device_input == NULL) {
        return;
    }
    if (xQueueSend(solua.device_input, event, 0) == pdPASS) {
        return;
    }

    solar_os_event_t dropped_event;
    if (xQueueReceive(solua.device_input, &dropped_event, 0) == pdPASS) {
        portENTER_CRITICAL(&solua_runtime_lock);
        if (solua.device_input_dropped != UINT32_MAX) {
            solua.device_input_dropped++;
        }
        portEXIT_CRITICAL(&solua_runtime_lock);
    }
    (void)xQueueSend(solua.device_input, event, 0);
}

static bool solua_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_POINTER ||
        event->type == SOLAR_OS_EVENT_AXIS) {
        solua_queue_device_input(event);
        return true;
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
        solua_return_to_shell(ctx, 130, "lua: stopped");
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
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_POINTER_EVENTS | SOLAR_OS_APP_FLAG_AXIS_EVENTS,
    .start = solua_start,
    .stop = solua_stop,
    .event = solua_event,
    .state_slot = &solua_state,
    .state_size = sizeof(solua_cold_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = SOLUA_TASK_STACK,
    .requested_tick_interval_ms = solua_requested_tick_interval_ms,
};
