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
#include "sdkconfig.h"
#include "nvs_flash.h"
#if SOLAR_OS_BOARD_HAS_DISPLAY
#include "solar_os_board_display.h"
#endif
#include "solar_os_board_caps.h"
#include "solar_os.h"
#include "solar_os_adc.h"
#include "solar_os_adc_dpad.h"
#include "solar_os_audio.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_buttons.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_chat.h"
#endif
#include "solar_os_cdc.h"
#include "solar_os_display.h"
#include "solar_os_engines.h"
#include "solar_os_expansion.h"
#include "solar_os_gpio.h"
#include "solar_os_gfx_internal.h"
#include "solar_os_fonts.h"
#include "solar_os_i2c.h"
#include "solar_os_joystick.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_mqtt.h"
#endif
#include "solar_os_ota.h"
#include "solar_os_port.h"
#include "solar_os_port_shell.h"
#include "solar_os_power.h"
#include "solar_os_pwm.h"
#include "solar_os_radio.h"
#include "solar_os_resources.h"
#include "solar_os_sensors.h"
#include "solar_os_sessions.h"
#include "solar_os_shell.h"
#include "solar_os_splash.h"
#include "solar_os_spi.h"
#include "solar_os_storage.h"
#include "solar_os_status_led.h"
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
#if CONFIG_IDF_TARGET_ESP32
#define KEY_WAKE_MODE ESP_EXT1_WAKEUP_ALL_LOW
#else
#define KEY_WAKE_MODE ESP_EXT1_WAKEUP_ANY_LOW
#endif
#else
#define KEY_WAKE_MODE ESP_EXT1_WAKEUP_ANY_HIGH
#endif
#define BLE_SLEEP_DISCONNECT_TIMEOUT_MS 500
#define BLE_RESUME_PM_HOLDOFF_MS 15000
#define APP_TICK_INTERVAL_MS 25
#define STATUS_UPDATE_INTERVAL_MS 1000
#define SESSION_OVERLAY_TITLE_MAX 48
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
static bool alt_prefix_pending;
static uint32_t session_overlay_until_ms;
static char session_overlay_title[SESSION_OVERLAY_TITLE_MAX];
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
#ifdef SOLAR_OS_BOARD_SPI_HOST
#ifdef SOLAR_OS_BOARD_SPI_NAME
    SOLAR_OS_LOGI(TAG,
                  "%s pins: SCK=%d MISO=%d MOSI=%d",
                  SOLAR_OS_BOARD_SPI_NAME,
                  SOLAR_OS_BOARD_PIN_SPI_SCLK,
                  SOLAR_OS_BOARD_PIN_SPI_MISO,
                  SOLAR_OS_BOARD_PIN_SPI_MOSI);
#else
    SOLAR_OS_LOGI(TAG,
                  "SPI%d pins: SCK=%d MISO=%d MOSI=%d",
                  (int)SOLAR_OS_BOARD_SPI_HOST,
                  SOLAR_OS_BOARD_PIN_SPI_SCLK,
                  SOLAR_OS_BOARD_PIN_SPI_MISO,
                  SOLAR_OS_BOARD_PIN_SPI_MOSI);
#endif
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
        if (solar_os_context_graphics_active(&os_ctx)) {
            solar_os_sessions_dispatch_resume(now_ms);
        } else {
            solar_os_sessions_mark_foreground_dirty();
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
    (void)solar_os_display_request_present_mode(u8g2, SOLAR_OS_DISPLAY_PRESENT_TEXT);
    u8g2_SendBuffer(u8g2);
}

static void session_terminal_changed(solar_os_terminal_t *new_terminal, void *user)
{
    (void)user;
    terminal = new_terminal;
}

static void session_overlay_requested(const char *title, void *user)
{
    (void)user;

    if (title == NULL || title[0] == '\0' || display_u8g2 == NULL) {
        return;
    }

    strlcpy(session_overlay_title, title, sizeof(session_overlay_title));
    session_overlay_until_ms = millis_u32() + SESSION_OVERLAY_MS;
}

static void dispatch_app_resume(uint32_t now_ms)
{
    solar_os_sessions_dispatch_resume(now_ms);
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

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        const esp_err_t ble_sleep_err =
            solar_os_ble_keyboard_prepare_sleep(BLE_SLEEP_DISCONNECT_TIMEOUT_MS);
        if (ble_sleep_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "BLE keyboard sleep prepare failed: %s",
                          esp_err_to_name(ble_sleep_err));
        }
    }
#endif

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
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        solar_os_ble_keyboard_resume();
        (void)solar_os_power_hold_automatic_light_sleep(BLE_RESUME_PM_HOLDOFF_MS);
    }
#endif
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
#if SOLAR_OS_PACKAGE_SERVICE_BLE
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
#endif
}

static void dispatch_char_to_foreground(char ch)
{
    const solar_os_app_t *foreground_app = solar_os_sessions_foreground_app();
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
    solar_os_sessions_dispatch_foreground_event(&event);
}

static void dispatch_input_chars(const char *chars, size_t count)
{
    if (chars == NULL || count == 0) {
        return;
    }

    solar_os_power_note_activity(millis_u32());
    for (size_t i = 0; i < count; i++) {
        const char ch = chars[i];

        if ((uint8_t)ch == SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE) {
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
            uint8_t volume = 0;
            const esp_err_t err = solar_os_audio_toggle_mute(&volume);
            if (err == ESP_OK) {
                SOLAR_OS_LOGI(TAG, "audio mute toggle: volume=%u", (unsigned)volume);
                last_status_update_ms = 0;
                update_status();
                draw_terminal_if_needed();
            } else if (err != ESP_ERR_NOT_SUPPORTED) {
                SOLAR_OS_LOGW(TAG, "audio mute toggle failed: %s", esp_err_to_name(err));
            }
#endif
            continue;
        }

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
                solar_os_sessions_cycle_next();
                process_app_requests();
                continue;
            }
            dispatch_char_to_foreground((char)SOLAR_OS_KEY_ALT_PREFIX);
        }

        dispatch_char_to_foreground(ch);
        process_app_requests();
    }
}

static void dispatch_keyboard_chars(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (!board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        return;
    }

    char chars[32];
    size_t count;
    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        dispatch_input_chars(chars, count);
    }
#endif
}

static void dispatch_button_chars(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_BUTTONS
    if (!board_has(SOLAR_OS_BOARD_CAP_BUTTONS)) {
        return;
    }

    char chars[16];
    size_t count;
    while ((count = solar_os_buttons_read_chars(chars, sizeof(chars))) > 0) {
        dispatch_input_chars(chars, count);
    }
#endif
}

static void dispatch_joystick_chars(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_JOYSTICK
    if (!board_has(SOLAR_OS_BOARD_CAP_JOYSTICK)) {
        return;
    }

    char chars[8];
    size_t count;
    while ((count = solar_os_joystick_read_chars(chars, sizeof(chars))) > 0) {
        dispatch_input_chars(chars, count);
    }
#endif
}

static void dispatch_adc_dpad_chars(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
    if (!board_has(SOLAR_OS_BOARD_CAP_ADC_DPAD)) {
        return;
    }

    char chars[8];
    size_t count;
    while ((count = solar_os_adc_dpad_read_chars(chars, sizeof(chars))) > 0) {
        dispatch_input_chars(chars, count);
    }
#endif
}

static void dispatch_input_sources(void)
{
    dispatch_keyboard_chars();
    dispatch_button_chars();
    dispatch_joystick_chars();
    dispatch_adc_dpad_chars();
}

static void dispatch_app_tick(void)
{
    const uint32_t now_ms = millis_u32();
    if ((now_ms - last_app_tick_ms) < APP_TICK_INTERVAL_MS) {
        return;
    }

    last_app_tick_ms = now_ms;
    solar_os_sessions_dispatch_tick(now_ms);

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

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solar_os_battery_status_t battery;
    if (board_has(SOLAR_OS_BOARD_CAP_BATTERY) &&
        solar_os_battery_get_status(&battery) == ESP_OK) {
        status.battery_valid = true;
        status.battery_percent = battery.percent;
        status.battery_external_power = battery.external_power;
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        status.ble_connected = solar_os_ble_keyboard_is_connected();
        status.ble_scanning = solar_os_ble_keyboard_is_scanning();
    }
#endif
    if (board_has(SOLAR_OS_BOARD_CAP_SD)) {
        status.sd_mounted = solar_os_storage_is_mounted();
    }

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solar_os_audio_status_t audio;
    if (board_has(SOLAR_OS_BOARD_CAP_AUDIO)) {
        solar_os_audio_get_status(&audio);
        status.audio_enabled = true;
        status.audio_volume = audio.volume;
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
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
#endif

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

    const esp_err_t storage_err = solar_os_storage_init();
    if (storage_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Default storage unavailable: %s", esp_err_to_name(storage_err));
    }

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const esp_err_t resources_err = solar_os_resources_init();
    if (resources_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Resource claims unavailable: %s", esp_err_to_name(resources_err));
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
    const esp_err_t engines_err = solar_os_engines_init();
    if (engines_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Engine telemetry unavailable: %s", esp_err_to_name(engines_err));
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    if (board_has(SOLAR_OS_BOARD_CAP_BATTERY)) {
        const esp_err_t battery_err = solar_os_battery_init();
        if (battery_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Battery monitor unavailable: %s", esp_err_to_name(battery_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    if (board_has(SOLAR_OS_BOARD_CAP_WIFI)) {
        const esp_err_t wifi_err = solar_os_wifi_init();
        if (wifi_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_err));
        }
    }
#endif

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

#if SOLAR_OS_PACKAGE_SERVICE_UART
    if (board_has(SOLAR_OS_BOARD_CAP_UART)) {
        const esp_err_t uart_err = solar_os_uart_init();
        if (uart_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "UART unavailable: %s", esp_err_to_name(uart_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    if (board_has(SOLAR_OS_BOARD_CAP_GPIO)) {
        const esp_err_t gpio_err = solar_os_gpio_init();
        if (gpio_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "GPIO service unavailable: %s", esp_err_to_name(gpio_err));
        }
    }

    if (board_has(SOLAR_OS_BOARD_CAP_STATUS_LED)) {
        const esp_err_t led_err = solar_os_status_led_init();
        if (led_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(led_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
    if (board_has(SOLAR_OS_BOARD_CAP_ADC)) {
        const esp_err_t adc_err = solar_os_adc_init();
        if (adc_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "ADC service unavailable: %s", esp_err_to_name(adc_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PWM
    if (board_has(SOLAR_OS_BOARD_CAP_PWM)) {
        const esp_err_t pwm_err = solar_os_pwm_init();
        if (pwm_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "PWM service unavailable: %s", esp_err_to_name(pwm_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BUTTONS
    if (board_has(SOLAR_OS_BOARD_CAP_BUTTONS)) {
        const esp_err_t buttons_err = solar_os_buttons_init();
        if (buttons_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Board buttons unavailable: %s", esp_err_to_name(buttons_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_JOYSTICK
    if (board_has(SOLAR_OS_BOARD_CAP_JOYSTICK)) {
        const esp_err_t joystick_err = solar_os_joystick_init();
        if (joystick_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Joystick unavailable: %s", esp_err_to_name(joystick_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
    if (board_has(SOLAR_OS_BOARD_CAP_ADC_DPAD)) {
        const esp_err_t dpad_err = solar_os_adc_dpad_init();
        if (dpad_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "ADC D-pad unavailable: %s", esp_err_to_name(dpad_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_I2C
    if (board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        const esp_err_t i2c_err = solar_os_i2c_init();
        if (i2c_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "I2C unavailable: %s", esp_err_to_name(i2c_err));
        } else {
#if SOLAR_OS_PACKAGE_SERVICE_RTC
            if (board_has(SOLAR_OS_BOARD_CAP_RTC)) {
                const esp_err_t rtc_err = solar_os_time_init();
                if (rtc_err != ESP_OK) {
                    SOLAR_OS_LOGW(TAG, "RTC unavailable: %s", esp_err_to_name(rtc_err));
                }
            }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
            if (board_has(SOLAR_OS_BOARD_CAP_TEMPERATURE) ||
                board_has(SOLAR_OS_BOARD_CAP_HUMIDITY)) {
                const esp_err_t sensors_err = solar_os_sensors_init();
                if (sensors_err != ESP_OK) {
                    SOLAR_OS_LOGW(TAG, "Sensors unavailable: %s", esp_err_to_name(sensors_err));
                }
            }
#endif
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SPI
    if (board_has(SOLAR_OS_BOARD_CAP_SPI)) {
        const esp_err_t spi_err = solar_os_spi_init();
        if (spi_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "SPI unavailable: %s", esp_err_to_name(spi_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (solar_os_expansion_available()) {
        const esp_err_t expansion_err = solar_os_expansion_init();
        if (expansion_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Expansion service unavailable: %s", esp_err_to_name(expansion_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_RADIO
    const esp_err_t radio_err = solar_os_radio_init();
    if (radio_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Radio service unavailable: %s", esp_err_to_name(radio_err));
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        const esp_err_t ble_err = solar_os_ble_keyboard_init();
        if (ble_err != ESP_OK) {
            SOLAR_OS_LOGE(TAG, "BLE keyboard init failed: %s", esp_err_to_name(ble_err));
        }
    }
#endif
}

static void process_app_requests(void)
{
    solar_os_sessions_process_requests();

    if (solar_os_context_take_sleep_request(&os_ctx)) {
        enter_light_sleep("shell sleep");
    }

    solar_os_sessions_process_requests();
}

static void maybe_enter_idle_sleep(void)
{
    if (!board_has(SOLAR_OS_BOARD_CAP_KEY) ||
        !solar_os_sessions_foreground_is_shell() ||
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

        uint8_t session_id = 0;
        const esp_err_t err =
            solar_os_port_shell_start(&os_ctx, fallback_ports[i].port_name, true, &session_id);
        if (err == ESP_OK) {
            SOLAR_OS_LOGI(TAG,
                          "Headless shell session %u started on %s",
                          (unsigned)session_id,
                          fallback_ports[i].port_name);
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
    ESP_ERROR_CHECK(solar_os_sessions_init(&os_ctx,
                                           NULL,
                                           NULL,
                                           session_terminal_changed,
                                           session_overlay_requested,
                                           NULL));

    if (board_has(SOLAR_OS_BOARD_CAP_DISPLAY)) {
#if SOLAR_OS_BOARD_HAS_DISPLAY
        const esp_err_t display_err = solar_os_board_display_init(&board_display);
        if (display_err == ESP_OK) {
            display_u8g2 = solar_os_board_display_u8g2(&board_display);
            const esp_err_t display_service_err = solar_os_display_init(&board_display);
            if (display_service_err != ESP_OK) {
                SOLAR_OS_LOGW(TAG,
                              "Display service unavailable: %s",
                              esp_err_to_name(display_service_err));
            }
            solar_os_gfx_init(&gfx, display_u8g2);
            solar_os_splash_clear(&gfx);

            shell_terminal = solar_os_psram_calloc(1, sizeof(*shell_terminal));
            if (shell_terminal != NULL) {
                solar_os_terminal_init(shell_terminal, display_u8g2);
                terminal = shell_terminal;
                solar_os_context_init(&os_ctx, terminal, &gfx);
                ESP_ERROR_CHECK(solar_os_sessions_init(&os_ctx,
                                                       shell_terminal,
                                                       display_u8g2,
                                                       session_terminal_changed,
                                                       session_overlay_requested,
                                                       NULL));
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
        solar_os_sessions_switch_to_app(solar_os_shell_app());
    } else {
        start_headless_shell_if_needed();
    }

    SOLAR_OS_LOGI(TAG, "SolarOS runtime started");

    while (true) {
        solar_os_power_poll();
        poll_key_button();
        dispatch_input_sources();
        dispatch_app_tick();
        dispatch_input_sources();
        process_app_requests();
        update_status();

        draw_terminal_if_needed();
        draw_session_overlay_if_needed();
        maybe_enter_idle_sleep();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
