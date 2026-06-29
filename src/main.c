#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#if SOLAR_OS_BOARD_HAS_DISPLAY
#include "solar_os_board_display.h"
#endif
#include "solar_os_board_caps.h"
#include "solar_os.h"
#include "solar_os_adc.h"
#include "solar_os_audio.h"
#include "solar_os_app_registry.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_chat.h"
#endif
#include "solar_os_cdc.h"
#include "solar_os_gpio.h"
#include "solar_os_gfx_internal.h"
#include "solar_os_fonts.h"
#include "solar_os_i2c.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_mqtt.h"
#endif
#include "solar_os_ota.h"
#include "solar_os_port.h"
#include "solar_os_power.h"
#include "solar_os_pwm.h"
#include "solar_os_sensors.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_splash.h"
#include "solar_os_storage.h"
#include "solar_os_terminal_internal.h"
#include "solar_os_time.h"
#include "solar_os_uart.h"
#include "solar_os_wifi.h"
#include "solar_os_board.h"

#ifndef SOLAR_OS_BOARD_PIN_KEY
#define SOLAR_OS_BOARD_PIN_KEY 0
#endif
#ifndef SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#endif
#ifndef SOLAR_OS_BOARD_KEY_PULL_UP
#define SOLAR_OS_BOARD_KEY_PULL_UP 0
#endif
#ifndef SOLAR_OS_BOARD_KEY_PULL_DOWN
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0
#endif

#define KEY_SHORT_PRESS_MIN_MS 30
#define KEY_LONG_PRESS_MS 1200
#define KEY_RELEASE_STABLE_MS 60
#define KEY_RELEASE_STABLE_TIMEOUT_MS 600
#define KEY_WAKE_MASK (1ULL << SOLAR_OS_BOARD_PIN_KEY)
#if SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL == 0
#define KEY_WAKE_MODE ESP_EXT1_WAKEUP_ANY_LOW
#else
#define KEY_WAKE_MODE ESP_EXT1_WAKEUP_ANY_HIGH
#endif
#define BLE_SLEEP_DISCONNECT_TIMEOUT_MS 500
#define BLE_RESUME_PM_HOLDOFF_MS 15000
#define APP_TICK_INTERVAL_MS 25
#define STATUS_UPDATE_INTERVAL_MS 1000
#define APP_SESSION_MAX 8
#define APP_SESSION_TITLE_MAX 48
#define SESSION_OVERLAY_MS 900

static const char *TAG = "solar_os";

#if SOLAR_OS_BOARD_HAS_DISPLAY
static solar_os_board_display_t board_display;
#endif
static solar_os_terminal_t *terminal;
static solar_os_terminal_t *shell_terminal;
static u8g2_t *display_u8g2;
static solar_os_gfx_t gfx;
static solar_os_context_t os_ctx;
static const solar_os_app_t *foreground_app;
static bool foreground_app_claimed;
typedef struct {
    bool used;
    bool started;
    bool suspended;
    bool claimed;
    bool owns_terminal;
    uint8_t id;
    const solar_os_app_t *app;
    solar_os_terminal_t *terminal;
    char title[APP_SESSION_TITLE_MAX];
} app_session_t;

static app_session_t app_sessions[APP_SESSION_MAX];
static app_session_t *foreground_session;
static bool alt_prefix_pending;
static uint32_t session_overlay_until_ms;
static char session_overlay_title[APP_SESSION_TITLE_MAX];
static volatile bool key_irq_pending;
static bool key_interrupt_ready;
static bool key_pressed;
static bool key_long_press_fired;
static bool key_ignore_until_released;
static uint32_t key_pressed_ms;
static uint32_t last_app_tick_ms;
static uint32_t last_status_update_ms;

static void process_app_requests(void);
static void maybe_enter_idle_sleep(void);
static void update_status(void);
static bool switch_to_app(const solar_os_app_t *app);
static const char *app_display_name(const solar_os_app_t *app);
static bool app_is_resumable(const solar_os_app_t *app);
static void session_prepare_context(app_session_t *session);
static void restore_foreground_context(void);
static void session_mark_dirty(app_session_t *session);
static app_session_t *ensure_shell_session(void);
static bool switch_to_session(app_session_t *session, bool show_overlay);
static bool close_session(app_session_t *session, bool preserve_context);
static void session_cycle_next(void);

static uint32_t millis_u32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool board_has(solar_os_board_capability_t capability)
{
    return solar_os_board_has(capability);
}

static bool key_level_is_pressed(int level)
{
    return level == SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL;
}

static bool key_button_is_pressed(void)
{
    return key_level_is_pressed(gpio_get_level(SOLAR_OS_BOARD_PIN_KEY));
}

static bool key_rtc_is_pressed(void)
{
    return key_level_is_pressed(rtc_gpio_get_level(SOLAR_OS_BOARD_PIN_KEY));
}

static uint8_t wifi_level_from_rssi(int8_t rssi)
{
    if (rssi >= -60) {
        return 3;
    }
    if (rssi >= -75) {
        return 2;
    }
    if (rssi >= -90) {
        return 1;
    }
    return 0;
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        if (ret == ESP_ERR_INVALID_STATE) {
            return ESP_OK;
        }
    }
    return ret;
}

static void print_boot_summary(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    SOLAR_OS_LOGI(TAG, "%s starter", SOLAR_OS_BOARD_NAME);
    SOLAR_OS_LOGI(TAG, "Board target: %s", SOLAR_OS_BOARD_ID);
#ifdef SOLAR_OS_BOARD_MODULE_NAME
    SOLAR_OS_LOGI(TAG, "Module: %s", SOLAR_OS_BOARD_MODULE_NAME);
#endif
    SOLAR_OS_LOGI(TAG, "Cores: %d, revision: %d", chip_info.cores, chip_info.revision);
    SOLAR_OS_LOGI(TAG,
                  "Features: Wi-Fi=%s BLE=%s",
                  (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
                  (chip_info.features & CHIP_FEATURE_BLE) ? "yes" : "no");
    SOLAR_OS_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
#if SOLAR_OS_BOARD_HAS_PSRAM
    SOLAR_OS_LOGI(TAG,
                  "PSRAM: declared %u bytes, heap %u bytes",
                  (unsigned)SOLAR_OS_BOARD_PSRAM_BYTES,
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
    SOLAR_OS_LOGI(TAG,
                  "PSRAM: not declared, heap %u bytes",
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#endif

    char caps[192];
    solar_os_board_capabilities_format(caps, sizeof(caps));
    SOLAR_OS_LOGI(TAG, "Board capabilities: %s", caps);

#ifdef SOLAR_OS_BOARD_DISPLAY_CONTROLLER
    SOLAR_OS_LOGI(TAG,
                  "Display: %s %dx%d",
                  SOLAR_OS_BOARD_DISPLAY_CONTROLLER,
                  SOLAR_OS_BOARD_DISPLAY_WIDTH,
                  SOLAR_OS_BOARD_DISPLAY_HEIGHT);
    SOLAR_OS_LOGI(TAG,
                  "Display pins: MOSI=%d SCK=%d DC=%d CS=%d RST=%d TE=%d",
                  SOLAR_OS_BOARD_PIN_LCD_MOSI,
                  SOLAR_OS_BOARD_PIN_LCD_SCK,
                  SOLAR_OS_BOARD_PIN_LCD_DC,
                  SOLAR_OS_BOARD_PIN_LCD_CS,
                  SOLAR_OS_BOARD_PIN_LCD_RST,
                  SOLAR_OS_BOARD_PIN_LCD_TE);
#endif
#ifdef SOLAR_OS_BOARD_I2C_PORT
    SOLAR_OS_LOGI(TAG,
                  "I2C%d pins: SDA=%d SCL=%d",
                  (int)SOLAR_OS_BOARD_I2C_PORT,
                  SOLAR_OS_BOARD_PIN_I2C_SDA,
                  SOLAR_OS_BOARD_PIN_I2C_SCL);
#endif
#ifdef SOLAR_OS_BOARD_PIN_SDMMC_CLK
    SOLAR_OS_LOGI(TAG,
                  "SDMMC pins: CLK=%d CMD=%d D0=%d",
                  SOLAR_OS_BOARD_PIN_SDMMC_CLK,
                  SOLAR_OS_BOARD_PIN_SDMMC_CMD,
                  SOLAR_OS_BOARD_PIN_SDMMC_D0);
#endif
#ifdef SOLAR_OS_BOARD_UART_PORT
    SOLAR_OS_LOGI(TAG,
                  "UART%d pins: TX=%d RX=%d",
                  (int)SOLAR_OS_BOARD_UART_PORT,
                  SOLAR_OS_BOARD_PIN_UART_TX,
                  SOLAR_OS_BOARD_PIN_UART_RX);
#endif
#ifdef SOLAR_OS_BOARD_EXPANSION_GPIO_LIST
    SOLAR_OS_LOGI(TAG, "Expansion GPIOs: %s", SOLAR_OS_BOARD_EXPANSION_GPIO_LIST);
#endif
#ifdef SOLAR_OS_BOARD_USER_GPIO_LIST
    SOLAR_OS_LOGI(TAG, "Runtime GPIOs: %s", SOLAR_OS_BOARD_USER_GPIO_LIST);
#endif
    if (board_has(SOLAR_OS_BOARD_CAP_KEY)) {
        SOLAR_OS_LOGI(TAG, "KEY pin: %d", SOLAR_OS_BOARD_PIN_KEY);
    }
}

static void IRAM_ATTR key_button_isr(void *arg)
{
    (void)arg;
    key_irq_pending = true;
}

static void draw_terminal_if_needed(void)
{
    if (!solar_os_context_graphics_active(&os_ctx) &&
        terminal != NULL &&
        solar_os_terminal_needs_draw(terminal)) {
        solar_os_terminal_draw(terminal);
    }
}

static void draw_session_overlay_if_needed(void)
{
    if (display_u8g2 == NULL || session_overlay_until_ms == 0) {
        return;
    }

    const uint32_t now_ms = millis_u32();
    if ((int32_t)(now_ms - session_overlay_until_ms) >= 0) {
        session_overlay_until_ms = 0;
        session_overlay_title[0] = '\0';
        if (foreground_session != NULL) {
            session_mark_dirty(foreground_session);
        } else if (terminal != NULL) {
            terminal->dirty = true;
        }
        return;
    }

    u8g2_t *u8g2 = display_u8g2;
    const int display_width = (int)u8g2_GetDisplayWidth(u8g2);
    const int display_height = (int)u8g2_GetDisplayHeight(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_solar_os_default_b_14_tf);
    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetFontPosBaseline(u8g2);

    int text_width = (int)u8g2_GetUTF8Width(u8g2, session_overlay_title);
    int box_width = text_width + 28;
    if (box_width < 96) {
        box_width = 96;
    }
    if (box_width > display_width - 24) {
        box_width = display_width - 24;
    }
    const int box_height = 38;
    const int box_x = (display_width - box_width) / 2;
    const int box_y = (display_height - box_height) / 2;
    int text_x = box_x + (box_width - text_width) / 2;
    if (text_x < box_x + 8) {
        text_x = box_x + 8;
    }
    const int text_y = box_y + 24;

    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawBox(u8g2, box_x, box_y, box_width, box_height);
    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawFrame(u8g2, box_x, box_y, box_width, box_height);
    u8g2_DrawUTF8(u8g2, text_x, text_y, session_overlay_title);
    u8g2_SendBuffer(u8g2);
}

static void dispatch_app_resume(uint32_t now_ms)
{
    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_RESUME,
        .data.tick_ms = now_ms,
    };

    if (foreground_app != NULL && foreground_app->event != NULL) {
        foreground_app->event(&os_ctx, &event);
        process_app_requests();
    }
}

static void resume_display_after_sleep(uint32_t now_ms)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    (void)now_ms;
    return;
#else
    if (!solar_os_board_display_ready(&board_display)) {
        return;
    }

    const esp_err_t err = solar_os_board_display_resume(&board_display);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "display resume failed: %s", esp_err_to_name(err));
        return;
    }

    if (solar_os_context_graphics_active(&os_ctx)) {
        dispatch_app_resume(now_ms);
    } else if (terminal != NULL) {
        terminal->dirty = true;
        draw_terminal_if_needed();
    }
#endif
}

static esp_err_t key_button_configure_gpio(void)
{
    const gpio_config_t key_config = {
        .pin_bit_mask = KEY_WAKE_MASK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = SOLAR_OS_BOARD_KEY_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = SOLAR_OS_BOARD_KEY_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    return gpio_config(&key_config);
}

static esp_err_t key_prepare_rtc_wakeup(void)
{
    esp_err_t err = rtc_gpio_init(SOLAR_OS_BOARD_PIN_KEY);
    if (err != ESP_OK) {
        return err;
    }
    err = rtc_gpio_set_direction(SOLAR_OS_BOARD_PIN_KEY, RTC_GPIO_MODE_INPUT_ONLY);
    if (err != ESP_OK) {
        return err;
    }
#if SOLAR_OS_BOARD_KEY_PULL_UP
    err = rtc_gpio_pullup_en(SOLAR_OS_BOARD_PIN_KEY);
#else
    err = rtc_gpio_pullup_dis(SOLAR_OS_BOARD_PIN_KEY);
#endif
    if (err != ESP_OK) {
        return err;
    }
#if SOLAR_OS_BOARD_KEY_PULL_DOWN
    return rtc_gpio_pulldown_en(SOLAR_OS_BOARD_PIN_KEY);
#else
    return rtc_gpio_pulldown_dis(SOLAR_OS_BOARD_PIN_KEY);
#endif
}

static void key_restore_gpio_after_rtc(void)
{
    (void)rtc_gpio_deinit(SOLAR_OS_BOARD_PIN_KEY);
    const esp_err_t err = key_button_configure_gpio();
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "KEY digital GPIO restore failed: %s", esp_err_to_name(err));
    }
}

static bool wait_key_released_stable(uint32_t stable_ms, uint32_t timeout_ms)
{
    const uint32_t start_ms = millis_u32();
    uint32_t released_since_ms = 0;

    while ((millis_u32() - start_ms) < timeout_ms) {
        const bool released = !key_button_is_pressed();
        const uint32_t now_ms = millis_u32();
        if (released) {
            if (released_since_ms == 0) {
                released_since_ms = now_ms;
            } else if ((now_ms - released_since_ms) >= stable_ms) {
                return true;
            }
        } else {
            released_since_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return false;
}

static bool wait_key_rtc_released_stable(uint32_t stable_ms, uint32_t timeout_ms)
{
    const uint32_t start_ms = millis_u32();
    uint32_t released_since_ms = 0;

    while ((millis_u32() - start_ms) < timeout_ms) {
        const bool released = !key_rtc_is_pressed();
        const uint32_t now_ms = millis_u32();
        if (released) {
            if (released_since_ms == 0) {
                released_since_ms = now_ms;
            } else if ((now_ms - released_since_ms) >= stable_ms) {
                return true;
            }
        } else {
            released_since_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return false;
}

static void enter_light_sleep(const char *reason)
{
    if (!board_has(SOLAR_OS_BOARD_CAP_KEY)) {
        SOLAR_OS_LOGW(TAG, "%s: light sleep needs a KEY wake source", reason);
        return;
    }

    if (!wait_key_released_stable(KEY_RELEASE_STABLE_MS, KEY_RELEASE_STABLE_TIMEOUT_MS)) {
        SOLAR_OS_LOGW(TAG, "%s: sleep cancelled, key release was not stable", reason);
        key_pressed = key_button_is_pressed();
        key_long_press_fired = false;
        key_pressed_ms = millis_u32();
        solar_os_power_note_activity(key_pressed_ms);
        return;
    }

    update_status();
    draw_terminal_if_needed();
    key_irq_pending = false;

    SOLAR_OS_LOGI(TAG, "%s: entering light sleep", reason);

    esp_err_t err = solar_os_power_begin_explicit_sleep();
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "%s: explicit sleep power policy failed: %s",
                      reason,
                      esp_err_to_name(err));
    }

    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    err = key_prepare_rtc_wakeup();
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "KEY RTC wake GPIO setup failed: %s", esp_err_to_name(err));
        key_restore_gpio_after_rtc();
        (void)solar_os_power_end_explicit_sleep();
        return;
    }

    if (!wait_key_rtc_released_stable(KEY_RELEASE_STABLE_MS, KEY_RELEASE_STABLE_TIMEOUT_MS)) {
        SOLAR_OS_LOGW(TAG, "%s: sleep cancelled, RTC key release was not stable", reason);
        key_restore_gpio_after_rtc();
        (void)solar_os_power_end_explicit_sleep();
        key_pressed = key_button_is_pressed();
        key_long_press_fired = false;
        key_pressed_ms = millis_u32();
        solar_os_power_note_activity(key_pressed_ms);
        return;
    }

    err = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "KEY sleep RTC power setup failed: %s", esp_err_to_name(err));
        key_restore_gpio_after_rtc();
        (void)solar_os_power_end_explicit_sleep();
        return;
    }

    err = esp_sleep_enable_ext1_wakeup_io(KEY_WAKE_MASK, KEY_WAKE_MODE);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "KEY sleep source setup failed: %s", esp_err_to_name(err));
        (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
        key_restore_gpio_after_rtc();
        (void)solar_os_power_end_explicit_sleep();
        return;
    }

    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        const esp_err_t ble_sleep_err =
            solar_os_ble_keyboard_prepare_sleep(BLE_SLEEP_DISCONNECT_TIMEOUT_MS);
        if (ble_sleep_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "BLE keyboard sleep prepare failed: %s",
                          esp_err_to_name(ble_sleep_err));
        }
    }

    solar_os_power_note_sleep_enter(millis_u32());
    err = esp_light_sleep_start();

    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    const uint64_t wake_ext1 = esp_sleep_get_ext1_wakeup_status();
    (void)esp_sleep_disable_ext1_wakeup_io(KEY_WAKE_MASK);
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
    key_restore_gpio_after_rtc();

    const uint32_t now_ms = millis_u32();
    last_app_tick_ms = now_ms;
    last_status_update_ms = 0;
    key_irq_pending = false;
    key_pressed = false;
    key_long_press_fired = false;
    key_ignore_until_released = key_button_is_pressed();

    if (err == ESP_OK) {
        SOLAR_OS_LOGI(TAG,
                      "wake from light sleep: cause=%d ext1=0x%016" PRIx64,
                      (int)wake_cause,
                      wake_ext1);
        solar_os_power_note_sleep_exit(now_ms, (int)wake_cause, wake_ext1, true);
    } else {
        SOLAR_OS_LOGW(TAG, "light sleep rejected: %s", esp_err_to_name(err));
        solar_os_power_note_sleep_exit(now_ms, (int)wake_cause, wake_ext1, false);
    }
    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        solar_os_ble_keyboard_resume();
        (void)solar_os_power_hold_automatic_light_sleep(BLE_RESUME_PM_HOLDOFF_MS);
    }
    (void)solar_os_power_end_explicit_sleep();

    update_status();
    resume_display_after_sleep(now_ms);
}

static void handle_key_short_press(void)
{
    solar_os_power_status_t power_status;
    solar_os_power_get_status(&power_status);

    switch (power_status.key_action) {
    case SOLAR_OS_POWER_KEY_ACTION_OFF:
        SOLAR_OS_LOGI(TAG, "KEY short press: sleep disabled");
        break;
    case SOLAR_OS_POWER_KEY_ACTION_LIGHT:
        enter_light_sleep("KEY short press");
        break;
    default:
        SOLAR_OS_LOGW(TAG, "KEY short press: unknown power action");
        break;
    }
}

static void key_button_init(void)
{
    if (!board_has(SOLAR_OS_BOARD_CAP_KEY)) {
        return;
    }

    ESP_ERROR_CHECK(key_button_configure_gpio());

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        SOLAR_OS_LOGW(TAG, "KEY interrupt service unavailable: %s", esp_err_to_name(err));
        return;
    }

    err = gpio_isr_handler_add(SOLAR_OS_BOARD_PIN_KEY, key_button_isr, NULL);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "KEY interrupt handler unavailable: %s", esp_err_to_name(err));
        return;
    }

    key_interrupt_ready = true;
}

static void poll_key_button(void)
{
    if (!board_has(SOLAR_OS_BOARD_CAP_KEY)) {
        return;
    }

    if (key_interrupt_ready && !key_irq_pending && !key_pressed && !key_ignore_until_released) {
        return;
    }
    key_irq_pending = false;

    const bool down = key_button_is_pressed();
    const uint32_t now_ms = millis_u32();

    if (key_ignore_until_released) {
        if (!down) {
            key_ignore_until_released = false;
            key_pressed = false;
            key_long_press_fired = false;
        }
        return;
    }

    if (down && !key_pressed) {
        key_pressed = true;
        key_long_press_fired = false;
        key_pressed_ms = now_ms;
        solar_os_power_note_activity(now_ms);
    } else if (!down && key_pressed) {
        const uint32_t press_ms = now_ms - key_pressed_ms;
        const bool short_press = !key_long_press_fired &&
            press_ms >= KEY_SHORT_PRESS_MIN_MS &&
            press_ms < KEY_LONG_PRESS_MS;
        key_pressed = false;
        solar_os_power_note_activity(now_ms);
        if (short_press) {
            handle_key_short_press();
        }
    }

    if (!down || key_long_press_fired || (now_ms - key_pressed_ms) < KEY_LONG_PRESS_MS) {
        return;
    }

    key_long_press_fired = true;
    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        const bool cancel_pairing = solar_os_ble_keyboard_is_pairing();
        const esp_err_t err = cancel_pairing ?
            solar_os_ble_keyboard_cancel_pairing() :
            solar_os_ble_keyboard_start_pairing();
        last_status_update_ms = 0;
        update_status();
        draw_terminal_if_needed();
        if (err == ESP_OK) {
            SOLAR_OS_LOGI(TAG,
                          "KEY long press: BLE keyboard pairing %s",
                          cancel_pairing ? "cancelled" : "started");
        } else {
            SOLAR_OS_LOGW(TAG,
                          "KEY long press: pairing %s failed: %s",
                          cancel_pairing ? "cancel" : "start",
                          esp_err_to_name(err));
        }
    }
}

static void dispatch_char_to_foreground(char ch)
{
    if (foreground_app == NULL || foreground_app->event == NULL) {
        return;
    }

    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_CHAR,
        .data.ch = ch,
    };

    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        SOLAR_OS_LOGI(TAG,
                      "dispatch app-exit key to %s",
                      foreground_app->name != NULL ? foreground_app->name : "?");
    }
    foreground_app->event(&os_ctx, &event);
}

static void dispatch_keyboard_chars(void)
{
    char chars[32];
    size_t count;

    if (!board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        return;
    }

    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        solar_os_power_note_activity(millis_u32());
        for (size_t i = 0; i < count; i++) {
            const char ch = chars[i];

            if ((uint8_t)ch == SOLAR_OS_KEY_ALT_PREFIX) {
                if (alt_prefix_pending) {
                    dispatch_char_to_foreground((char)SOLAR_OS_KEY_ALT_PREFIX);
                }
                alt_prefix_pending = true;
                continue;
            }

            if (alt_prefix_pending) {
                alt_prefix_pending = false;
                if (ch == '\t') {
                    session_cycle_next();
                    process_app_requests();
                    continue;
                }
                dispatch_char_to_foreground((char)SOLAR_OS_KEY_ALT_PREFIX);
            }

            dispatch_char_to_foreground(ch);
            process_app_requests();
        }
    }
}

static void dispatch_session_event(app_session_t *session, const solar_os_event_t *event)
{
    if (session == NULL || !session->used || session->app == NULL ||
        session->app->event == NULL || event == NULL) {
        return;
    }

    session_prepare_context(session);
    session->app->event(&os_ctx, event);
    if (solar_os_context_take_exit_request(&os_ctx)) {
        (void)close_session(session, false);
    }
}

static void dispatch_app_tick(void)
{
    const uint32_t now_ms = millis_u32();
    if ((now_ms - last_app_tick_ms) < APP_TICK_INTERVAL_MS) {
        return;
    }

    last_app_tick_ms = now_ms;
    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_TICK,
        .data.tick_ms = now_ms,
    };

    for (size_t i = 0; i < APP_SESSION_MAX; i++) {
        app_session_t *session = &app_sessions[i];
        if (!session->used || session->app == NULL || session == foreground_session) {
            continue;
        }
        dispatch_session_event(session, &event);
    }
    restore_foreground_context();

    if (foreground_session != NULL) {
        dispatch_session_event(foreground_session, &event);
        restore_foreground_context();
        process_app_requests();
    } else if (foreground_app != NULL && foreground_app->event != NULL) {
        foreground_app->event(&os_ctx, &event);
        process_app_requests();
    }

    solar_os_jobs_tick(&os_ctx, now_ms);
    process_app_requests();
}

static void update_status(void)
{
    if (terminal == NULL) {
        return;
    }

    const uint32_t now_ms = millis_u32();
    if (last_status_update_ms != 0 &&
        (now_ms - last_status_update_ms) < STATUS_UPDATE_INTERVAL_MS) {
        return;
    }
    last_status_update_ms = now_ms;

    solar_os_status_bar_t status = {0};

    solar_os_battery_status_t battery;
    if (board_has(SOLAR_OS_BOARD_CAP_BATTERY) &&
        solar_os_battery_get_status(&battery) == ESP_OK) {
        status.battery_valid = true;
        status.battery_percent = battery.percent;
        status.battery_external_power = battery.external_power;
    }

    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        status.ble_connected = solar_os_ble_keyboard_is_connected();
        status.ble_scanning = solar_os_ble_keyboard_is_scanning();
    }
    if (board_has(SOLAR_OS_BOARD_CAP_SD)) {
        status.sd_mounted = solar_os_storage_is_mounted();
    }

    solar_os_audio_status_t audio;
    if (board_has(SOLAR_OS_BOARD_CAP_AUDIO)) {
        solar_os_audio_get_status(&audio);
        status.audio_enabled = audio.initialized;
        status.audio_volume = audio.volume;
    }

    solar_os_wifi_status_t wifi;
    if (board_has(SOLAR_OS_BOARD_CAP_WIFI)) {
        solar_os_wifi_get_status(&wifi);
        status.wifi_started = wifi.started;
        status.wifi_connected = wifi.connected;
        status.wifi_has_ip = wifi.has_ip;
        if (wifi.connected && wifi.has_ip) {
            status.wifi_level = wifi_level_from_rssi(wifi.rssi);
        }
    }

    solar_os_datetime_t datetime;
    if (board_has(SOLAR_OS_BOARD_CAP_RTC) &&
        solar_os_time_get_datetime(&datetime) == ESP_OK &&
        solar_os_time_datetime_is_valid(&datetime) &&
        datetime.clock_integrity) {
        status.time_valid = true;
        status.hour = datetime.hour;
        status.minute = datetime.minute;
    }

    solar_os_terminal_set_status_bar(terminal, &status);
}

static void init_peripherals(void)
{
    const esp_err_t port_err = solar_os_port_init();
    if (port_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Port service unavailable: %s", esp_err_to_name(port_err));
    }

    if (board_has(SOLAR_OS_BOARD_CAP_CDC)) {
        const esp_err_t cdc_err = solar_os_cdc_init();
        if (cdc_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "CDC port unavailable: %s", esp_err_to_name(cdc_err));
        }
    }

    const esp_err_t power_err = solar_os_power_init();
    if (power_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Power service unavailable: %s", esp_err_to_name(power_err));
    }
    solar_os_power_note_activity(millis_u32());

    if (board_has(SOLAR_OS_BOARD_CAP_SD)) {
        const esp_err_t sd_err = solar_os_storage_init();
        if (sd_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "SD card unavailable: %s", esp_err_to_name(sd_err));
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_BATTERY)) {
        const esp_err_t battery_err = solar_os_battery_init();
        if (battery_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Battery monitor unavailable: %s", esp_err_to_name(battery_err));
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_WIFI)) {
        const esp_err_t wifi_err = solar_os_wifi_init();
        if (wifi_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_err));
        }
    }

    const esp_err_t ota_err = solar_os_ota_init();
    if (ota_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "OTA service unavailable: %s", esp_err_to_name(ota_err));
    }

#if SOLAR_OS_PACKAGE_NET
    const esp_err_t mqtt_err = solar_os_mqtt_init();
    if (mqtt_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "MQTT service unavailable: %s", esp_err_to_name(mqtt_err));
    }

    const esp_err_t chat_err = solar_os_chat_init();
    if (chat_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Chat service unavailable: %s", esp_err_to_name(chat_err));
    }
#endif

    if (board_has(SOLAR_OS_BOARD_CAP_UART)) {
        const esp_err_t uart_err = solar_os_uart_init();
        if (uart_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "UART unavailable: %s", esp_err_to_name(uart_err));
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_GPIO)) {
        const esp_err_t gpio_err = solar_os_gpio_init();
        if (gpio_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "GPIO service unavailable: %s", esp_err_to_name(gpio_err));
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_ADC)) {
        const esp_err_t adc_err = solar_os_adc_init();
        if (adc_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "ADC service unavailable: %s", esp_err_to_name(adc_err));
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_PWM)) {
        const esp_err_t pwm_err = solar_os_pwm_init();
        if (pwm_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "PWM service unavailable: %s", esp_err_to_name(pwm_err));
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        const esp_err_t i2c_err = solar_os_i2c_init();
        if (i2c_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "I2C unavailable: %s", esp_err_to_name(i2c_err));
        } else {
            if (board_has(SOLAR_OS_BOARD_CAP_RTC)) {
                const esp_err_t rtc_err = solar_os_time_init();
                if (rtc_err != ESP_OK) {
                    SOLAR_OS_LOGW(TAG, "RTC unavailable: %s", esp_err_to_name(rtc_err));
                }
            }

            if (board_has(SOLAR_OS_BOARD_CAP_TEMPERATURE) ||
                board_has(SOLAR_OS_BOARD_CAP_HUMIDITY)) {
                const esp_err_t sensors_err = solar_os_sensors_init();
                if (sensors_err != ESP_OK) {
                    SOLAR_OS_LOGW(TAG, "Sensors unavailable: %s", esp_err_to_name(sensors_err));
                }
            }
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        const esp_err_t ble_err = solar_os_ble_keyboard_init();
        if (ble_err != ESP_OK) {
            SOLAR_OS_LOGE(TAG, "BLE keyboard init failed: %s", esp_err_to_name(ble_err));
        }
    }
}

static const char *app_display_name(const solar_os_app_t *app)
{
    return app != NULL && app->name != NULL ? app->name : "?";
}

static bool app_is_resumable(const solar_os_app_t *app)
{
    return app != NULL && (app->flags & SOLAR_OS_APP_FLAG_RESUMABLE) != 0;
}

static bool launch_should_use_display_sessions(void)
{
    if (shell_terminal == NULL) {
        return false;
    }

    solar_os_shell_io_t *io = solar_os_context_shell_io(&os_ctx);
    return io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT;
}

static void session_owner_name(const app_session_t *session, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (session == NULL) {
        strlcpy(buffer, "session", buffer_len);
        return;
    }
    snprintf(buffer, buffer_len, "session %u", (unsigned)session->id);
}

static void session_prepare_context(app_session_t *session)
{
    if (session == NULL) {
        return;
    }

    terminal = session->terminal;
    os_ctx.terminal = session->terminal;
    solar_os_context_set_shell_io(&os_ctx, NULL);
    solar_os_context_set_shell_session(&os_ctx, NULL);
}

static void restore_foreground_context(void)
{
    if (foreground_session != NULL) {
        session_prepare_context(foreground_session);
    }
}

static void session_update_title(app_session_t *session)
{
    if (session == NULL || session->app == NULL) {
        return;
    }

    if (session->app->title != NULL) {
        session_prepare_context(session);
        session->app->title(&os_ctx, session->title, sizeof(session->title));
    }
    if (session->title[0] == '\0') {
        strlcpy(session->title, app_display_name(session->app), sizeof(session->title));
    }
    restore_foreground_context();
}

static app_session_t *session_by_id(uint8_t id)
{
    if (id >= APP_SESSION_MAX || !app_sessions[id].used) {
        return NULL;
    }
    return &app_sessions[id];
}

static app_session_t *session_find_by_app(const solar_os_app_t *app)
{
    if (app == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < APP_SESSION_MAX; i++) {
        if (app_sessions[i].used && app_sessions[i].app == app) {
            return &app_sessions[i];
        }
    }
    return NULL;
}

static app_session_t *session_alloc(const solar_os_app_t *app)
{
    if (app == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < APP_SESSION_MAX; i++) {
        if (app_sessions[i].used) {
            continue;
        }
        app_session_t *session = &app_sessions[i];
        memset(session, 0, sizeof(*session));
        session->used = true;
        session->id = (uint8_t)i;
        session->app = app;
        strlcpy(session->title, app_display_name(app), sizeof(session->title));
        return session;
    }
    return NULL;
}

static void session_free_terminal(app_session_t *session)
{
    if (session == NULL || !session->owns_terminal || session->terminal == NULL) {
        return;
    }

    heap_caps_free(session->terminal);
    session->terminal = NULL;
    session->owns_terminal = false;
}

static esp_err_t session_ensure_terminal(app_session_t *session)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->terminal != NULL) {
        return ESP_OK;
    }
    if (session->app == solar_os_shell_app()) {
        session->terminal = shell_terminal;
        return session->terminal != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (display_u8g2 == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_terminal_t *session_terminal = solar_os_psram_calloc(1, sizeof(*session_terminal));
    if (session_terminal == NULL) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_terminal_init(session_terminal, display_u8g2);
    session->terminal = session_terminal;
    session->owns_terminal = true;
    return ESP_OK;
}

static void session_mark_dirty(app_session_t *session)
{
    if (session != NULL && session->terminal != NULL) {
        session->terminal->dirty = true;
    }
}

static bool session_claim_display(app_session_t *session)
{
    if (session == NULL || session->app == NULL || session->app == solar_os_shell_app()) {
        return true;
    }
    if (session->claimed) {
        return true;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];
    session_owner_name(session, owner, sizeof(owner));
    const esp_err_t err =
        solar_os_app_registry_claim(session->app, owner, busy_owner, sizeof(busy_owner));
    if (err == ESP_OK) {
        session->claimed = solar_os_app_registry_find_by_app(session->app) != NULL;
        return true;
    }

    solar_os_shell_io_t *io = solar_os_context_shell_io(&os_ctx);
    if (io != NULL && solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_printf(io,
                                 "%s: already running on %s\n",
                                 app_display_name(session->app),
                                 busy_owner[0] != '\0' ? busy_owner : "another session");
        solar_os_shell_io_flush(io);
    }
    return false;
}

static void session_release_display(app_session_t *session)
{
    if (session == NULL || !session->claimed || session->app == NULL) {
        return;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    session_owner_name(session, owner, sizeof(owner));
    solar_os_app_registry_release(session->app, owner);
    session->claimed = false;
}

static bool display_claim_app(const solar_os_app_t *app, bool *claimed)
{
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];

    if (claimed != NULL) {
        *claimed = false;
    }

    const esp_err_t err =
        solar_os_app_registry_claim(app, "display", busy_owner, sizeof(busy_owner));
    if (err == ESP_OK) {
        if (claimed != NULL) {
            *claimed = solar_os_app_registry_find_by_app(app) != NULL;
        }
        return true;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(&os_ctx);
        if (io != NULL && solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_NONE) {
            solar_os_shell_io_printf(io,
                                     "%s: already running on %s\n",
                                     app != NULL && app->name != NULL ? app->name : "app",
                                     busy_owner[0] != '\0' ? busy_owner : "another session");
            solar_os_shell_io_flush(io);
        }
    } else {
        SOLAR_OS_LOGW(TAG,
                      "App %s claim failed: %s",
                      app != NULL && app->name != NULL ? app->name : "?",
                      esp_err_to_name(err));
    }
    return false;
}

static void display_release_app(const solar_os_app_t *app)
{
    if (!foreground_app_claimed || app == NULL) {
        return;
    }

    solar_os_app_registry_release(app, "display");
    foreground_app_claimed = false;
}

static void display_prompt_after_failed_launch(void)
{
    if (foreground_app != solar_os_shell_app()) {
        return;
    }

    solar_os_shell_session_t *session = solar_os_context_shell_session(&os_ctx);
    if (session != NULL) {
        solar_os_shell_session_prompt(&os_ctx, session);
    }
}

static void show_session_overlay(const app_session_t *session)
{
    if (session == NULL || session->title[0] == '\0' || display_u8g2 == NULL) {
        return;
    }

    strlcpy(session_overlay_title, session->title, sizeof(session_overlay_title));
    session_overlay_until_ms = millis_u32() + SESSION_OVERLAY_MS;
}

static void stop_legacy_foreground(void)
{
    if (foreground_app != NULL && foreground_app->stop != NULL) {
        SOLAR_OS_LOGI(TAG, "stop app: %s", app_display_name(foreground_app));
        foreground_app->stop(&os_ctx);
    }
    display_release_app(foreground_app);
    solar_os_context_set_graphics_active(&os_ctx, false);
}

static void suspend_foreground_session(void)
{
    if (foreground_session == NULL) {
        stop_legacy_foreground();
        return;
    }

    app_session_t *session = foreground_session;
    session_prepare_context(session);
    if (session->app != NULL && session->app->suspend != NULL) {
        session->app->suspend(&os_ctx);
    }
    session->suspended = true;
    session_update_title(session);
}

static bool start_or_resume_session(app_session_t *session)
{
    if (session == NULL || session->app == NULL) {
        return false;
    }
    if (session_ensure_terminal(session) != ESP_OK || !session_claim_display(session)) {
        return false;
    }

    session_prepare_context(session);
    solar_os_context_set_graphics_active(&os_ctx, false);

    if (!session->started) {
        if (session->app->start != NULL) {
            const esp_err_t app_err = session->app->start(&os_ctx);
            if (app_err != ESP_OK) {
                SOLAR_OS_LOGE(TAG,
                              "App %s failed to start: %s",
                              app_display_name(session->app),
                              esp_err_to_name(app_err));
                session_release_display(session);
                session_free_terminal(session);
                memset(session, 0, sizeof(*session));
                return false;
            }
        }
        session->started = true;
    } else if (session->app->resume != NULL) {
        session->app->resume(&os_ctx);
    }

    foreground_session = session;
    foreground_app = session->app;
    foreground_app_claimed = false;
    session->suspended = false;
    session_update_title(session);
    session_mark_dirty(session);
    last_status_update_ms = 0;
    return true;
}

static bool switch_to_session(app_session_t *session, bool show_overlay)
{
    if (session == NULL || session->app == NULL) {
        return false;
    }
    if (session == foreground_session && foreground_app == session->app) {
        return true;
    }

    SOLAR_OS_LOGI(TAG,
                  "switch session: %s -> %s",
                  foreground_app != NULL ? app_display_name(foreground_app) : "(none)",
                  app_display_name(session->app));
    app_session_t *previous_session = foreground_session;
    suspend_foreground_session();
    if (!start_or_resume_session(session)) {
        if (previous_session != NULL && previous_session->used) {
            (void)start_or_resume_session(previous_session);
        }
        return false;
    }
    if (show_overlay) {
        show_session_overlay(session);
    }
    return true;
}

static app_session_t *ensure_shell_session(void)
{
    app_session_t *session = &app_sessions[0];
    if (!session->used) {
        memset(session, 0, sizeof(*session));
        session->used = true;
        session->id = 0;
        session->app = solar_os_shell_app();
        session->terminal = shell_terminal;
        strlcpy(session->title, "shell", sizeof(session->title));
    }
    return session;
}

static bool switch_to_app(const solar_os_app_t *app)
{
    bool new_app_claimed = false;

    if (app == NULL) {
        return false;
    }
    if (app == foreground_app) {
        return true;
    }

    const bool use_display_session = launch_should_use_display_sessions();

    if (use_display_session && app == solar_os_shell_app()) {
        return switch_to_session(ensure_shell_session(), false);
    }

    if (use_display_session && app_is_resumable(app)) {
        app_session_t *session = session_find_by_app(app);
        if (session == NULL) {
            session = session_alloc(app);
        }
        if (session == NULL) {
            SOLAR_OS_LOGW(TAG, "No free app session for %s", app_display_name(app));
            return false;
        }
        return switch_to_session(session, false);
    }

    if (app != solar_os_shell_app() && !display_claim_app(app, &new_app_claimed)) {
        return false;
    }

    SOLAR_OS_LOGI(TAG,
                  "switch app: %s -> %s",
                  foreground_app != NULL && foreground_app->name != NULL ? foreground_app->name : "(none)",
                  app->name != NULL ? app->name : "?");

    suspend_foreground_session();

    solar_os_context_set_graphics_active(&os_ctx, false);
    foreground_session = NULL;
    foreground_app = app;
    foreground_app_claimed = new_app_claimed;
    if (foreground_app->start == NULL) {
        return true;
    }

    const esp_err_t app_err = foreground_app->start(&os_ctx);
    if (app_err == ESP_OK) {
        return true;
    }

    SOLAR_OS_LOGE(TAG, "App %s failed to start: %s", foreground_app->name, esp_err_to_name(app_err));
    display_release_app(foreground_app);
    solar_os_context_set_graphics_active(&os_ctx, false);
    foreground_app = NULL;
    foreground_session = NULL;
    (void)switch_to_session(ensure_shell_session(), false);
    return false;
}

static void session_print_list_to_io(solar_os_shell_io_t *io, void *user)
{
    (void)user;

    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        return;
    }

    solar_os_shell_io_writeln(io, "ID  STATE       APP       TITLE");
    for (size_t i = 0; i < APP_SESSION_MAX; i++) {
        app_session_t *session = &app_sessions[i];
        if (!session->used || session->app == NULL) {
            continue;
        }
        session_update_title(session);
        const char *state = session == foreground_session ? "active" :
            session->suspended ? "suspended" : "ready";
        solar_os_shell_io_printf(io,
                                 "%-3u %-11s %-9s %s\n",
                                 (unsigned)session->id,
                                 state,
                                 app_display_name(session->app),
                                 session->title);
    }
    solar_os_shell_io_flush(io);
}

static bool close_session(app_session_t *session, bool preserve_context)
{
    if (session == NULL || !session->used || session->app == solar_os_shell_app()) {
        return false;
    }

    const bool was_foreground = session == foreground_session;
    solar_os_terminal_t *previous_terminal = NULL;
    solar_os_shell_io_t *previous_shell_io = NULL;
    solar_os_shell_session_t *previous_shell_session = NULL;

    if (preserve_context && !was_foreground) {
        previous_terminal = terminal;
        previous_shell_io = solar_os_context_shell_io(&os_ctx);
        previous_shell_session = solar_os_context_shell_session(&os_ctx);
    }

    if (was_foreground) {
        foreground_session = NULL;
        foreground_app = NULL;
    }

    session_prepare_context(session);
    if (session->app != NULL && session->app->stop != NULL) {
        SOLAR_OS_LOGI(TAG, "close session %u: %s", (unsigned)session->id, app_display_name(session->app));
        session->app->stop(&os_ctx);
    }
    session_release_display(session);
    session_free_terminal(session);
    memset(session, 0, sizeof(*session));

    if (was_foreground) {
        return switch_to_session(ensure_shell_session(), false);
    }
    if (preserve_context) {
        terminal = previous_terminal;
        os_ctx.terminal = previous_terminal;
        solar_os_context_set_shell_io(&os_ctx, previous_shell_io);
        solar_os_context_set_shell_session(&os_ctx, previous_shell_session);
        return true;
    }
    restore_foreground_context();
    return true;
}

static void session_prompt_if_shell_active(void)
{
    if (foreground_app != solar_os_shell_app()) {
        return;
    }

    solar_os_shell_session_t *session = solar_os_context_shell_session(&os_ctx);
    if (session != NULL) {
        solar_os_shell_session_prompt(&os_ctx, session);
    }
}

static void handle_session_request(void)
{
    solar_os_session_request_type_t request = SOLAR_OS_SESSION_REQUEST_NONE;
    uint8_t session_id = 0;

    while (solar_os_context_take_session_request(&os_ctx, &request, &session_id)) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(&os_ctx);

        switch (request) {
        case SOLAR_OS_SESSION_REQUEST_LIST:
            session_print_list_to_io(io, NULL);
            session_prompt_if_shell_active();
            break;
        case SOLAR_OS_SESSION_REQUEST_FG: {
            app_session_t *session = session_by_id(session_id);
            if (session == NULL) {
                if (io != NULL) {
                    solar_os_shell_io_printf(io, "fg: no such session: %u\n", (unsigned)session_id);
                    solar_os_shell_io_flush(io);
                }
                session_prompt_if_shell_active();
                break;
            }
            if (!switch_to_session(session, true) && io != NULL) {
                solar_os_shell_io_printf(io, "fg: failed: %u\n", (unsigned)session_id);
                solar_os_shell_io_flush(io);
                session_prompt_if_shell_active();
            }
            break;
        }
        case SOLAR_OS_SESSION_REQUEST_CLOSE: {
            app_session_t *session = session_by_id(session_id);
            if (session == NULL || session->app == solar_os_shell_app()) {
                if (io != NULL) {
                    solar_os_shell_io_printf(io, "close: no such closable session: %u\n", (unsigned)session_id);
                    solar_os_shell_io_flush(io);
                }
                session_prompt_if_shell_active();
                break;
            }
            if (close_session(session, true)) {
                if (io != NULL) {
                    solar_os_shell_io_printf(io, "closed session %u\n", (unsigned)session_id);
                    solar_os_shell_io_flush(io);
                }
            }
            session_prompt_if_shell_active();
            break;
        }
        case SOLAR_OS_SESSION_REQUEST_NONE:
        default:
            break;
        }
    }
}

static app_session_t *session_next_in_ring(void)
{
    uint8_t start = foreground_session != NULL ? foreground_session->id : 0;
    for (size_t step = 1; step <= APP_SESSION_MAX; step++) {
        const uint8_t index = (uint8_t)((start + step) % APP_SESSION_MAX);
        if (app_sessions[index].used && app_sessions[index].app != NULL) {
            return &app_sessions[index];
        }
    }
    return NULL;
}

static void session_cycle_next(void)
{
    app_session_t *next = session_next_in_ring();
    if (next != NULL) {
        (void)switch_to_session(next, true);
    }
}

static void process_app_requests(void)
{
    handle_session_request();

    if (solar_os_context_take_exit_request(&os_ctx)) {
        SOLAR_OS_LOGI(TAG,
                      "exit request for foreground app: %s",
                      foreground_app != NULL && foreground_app->name != NULL ? foreground_app->name : "(none)");
        if (foreground_session != NULL &&
            foreground_session->app != solar_os_shell_app() &&
            app_is_resumable(foreground_session->app)) {
            (void)close_session(foreground_session, false);
        } else if (foreground_app != solar_os_shell_app()) {
            switch_to_app(solar_os_shell_app());
        }
    }

    const solar_os_app_t *requested_app = solar_os_context_take_launch_request(&os_ctx);
    if (requested_app != NULL) {
        if (!switch_to_app(requested_app)) {
            display_prompt_after_failed_launch();
        }
    }

    if (solar_os_context_take_sleep_request(&os_ctx)) {
        enter_light_sleep("shell sleep");
    }

    handle_session_request();
}

static void maybe_enter_idle_sleep(void)
{
    if (!board_has(SOLAR_OS_BOARD_CAP_KEY) ||
        foreground_app != solar_os_shell_app() ||
        key_pressed ||
        key_ignore_until_released) {
        return;
    }

    const uint32_t now_ms = millis_u32();
    if (solar_os_power_should_idle_sleep(now_ms)) {
        enter_light_sleep("power idle");
    }
}

static void start_headless_shell_if_needed(void)
{
    if (terminal != NULL) {
        return;
    }

    static const struct {
        solar_os_board_capability_t capability;
        const char *port_name;
    } fallback_ports[] = {
        {SOLAR_OS_BOARD_CAP_UART, SOLAR_OS_UART_PORT_NAME},
        {SOLAR_OS_BOARD_CAP_CDC, SOLAR_OS_CDC_PORT_NAME},
    };

    bool had_candidate = false;
    for (size_t i = 0; i < sizeof(fallback_ports) / sizeof(fallback_ports[0]); i++) {
        if (!board_has(fallback_ports[i].capability)) {
            continue;
        }
        had_candidate = true;

        char *argv[] = {
            (char *)"shell",
            (char *)fallback_ports[i].port_name,
        };
        const esp_err_t err = solar_os_jobs_start(&os_ctx, "shell", 2, argv);
        if (err == ESP_OK) {
            SOLAR_OS_LOGI(TAG, "Headless shell started on %s", fallback_ports[i].port_name);
            return;
        }
        SOLAR_OS_LOGW(TAG,
                      "Headless shell on %s failed: %s",
                      fallback_ports[i].port_name,
                      esp_err_to_name(err));
    }

    if (!had_candidate) {
        SOLAR_OS_LOGW(TAG,
                      "No display terminal and no byte-stream capability; no interactive shell started");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    const esp_err_t log_err = solar_os_log_init();
    if (log_err != ESP_OK) {
        ESP_LOGW(TAG, "Log service unavailable: %s", esp_err_to_name(log_err));
    }
    print_boot_summary();
    key_button_init();

    solar_os_context_init(&os_ctx, NULL, NULL);
    solar_os_context_set_session_list_handler(&os_ctx, session_print_list_to_io, NULL);

    if (board_has(SOLAR_OS_BOARD_CAP_DISPLAY)) {
#if SOLAR_OS_BOARD_HAS_DISPLAY
        const esp_err_t display_err = solar_os_board_display_init(&board_display);
        if (display_err == ESP_OK) {
            display_u8g2 = solar_os_board_display_u8g2(&board_display);
            solar_os_gfx_init(&gfx, display_u8g2);
            solar_os_splash_clear(&gfx);

            shell_terminal = solar_os_psram_calloc(1, sizeof(*shell_terminal));
            if (shell_terminal != NULL) {
                solar_os_terminal_init(shell_terminal, display_u8g2);
                terminal = shell_terminal;
                solar_os_context_init(&os_ctx, terminal, &gfx);
                solar_os_context_set_session_list_handler(&os_ctx, session_print_list_to_io, NULL);
                (void)ensure_shell_session();
                solar_os_splash_draw(&gfx, "starting services");
            } else {
                ESP_LOGE(TAG, "Terminal allocation failed; continuing without display shell");
                solar_os_board_display_deinit(&board_display);
            }
        } else {
            ESP_LOGE(TAG,
                     "Display init failed: %s; continuing without display shell",
                     esp_err_to_name(display_err));
        }
#else
        ESP_LOGE(TAG, "Display capability set, but no display driver was compiled");
#endif
    } else {
        SOLAR_OS_LOGI(TAG, "No display capability; booting headless");
    }

    ESP_ERROR_CHECK(solar_os_jobs_init());

    init_peripherals();
    update_status();

    if (terminal != NULL) {
        switch_to_app(solar_os_shell_app());
    } else {
        start_headless_shell_if_needed();
    }

    SOLAR_OS_LOGI(TAG, "SolarOS runtime started");

    while (true) {
        solar_os_power_poll();
        poll_key_button();
        dispatch_keyboard_chars();
        dispatch_app_tick();
        dispatch_keyboard_chars();
        process_app_requests();
        update_status();

        draw_terminal_if_needed();
        draw_session_overlay_if_needed();
        maybe_enter_idle_sleep();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
