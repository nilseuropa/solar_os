#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

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
#include "rlcd_st7305.h"
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
#define APP_TICK_INTERVAL_MS 25
#define STATUS_UPDATE_INTERVAL_MS 1000

static const char *TAG = "solar_os";

static rlcd_st7305_t lcd;
static solar_os_terminal_t *terminal;
static solar_os_gfx_t gfx;
static solar_os_context_t os_ctx;
static const solar_os_app_t *foreground_app;
static bool foreground_app_claimed;
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
    SOLAR_OS_LOGI(TAG, "Module: %s", SOLAR_OS_BOARD_MODULE_NAME);
    SOLAR_OS_LOGI(TAG, "Cores: %d, revision: %d", chip_info.cores, chip_info.revision);
    SOLAR_OS_LOGI(TAG,
                  "Features: Wi-Fi=%s BLE=%s",
                  (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
                  (chip_info.features & CHIP_FEATURE_BLE) ? "yes" : "no");
    SOLAR_OS_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    SOLAR_OS_LOGI(TAG,
                  "PSRAM heap: %u bytes",
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

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
    SOLAR_OS_LOGI(TAG,
                  "I2C%d pins: SDA=%d SCL=%d",
                  (int)SOLAR_OS_BOARD_I2C_PORT,
                  SOLAR_OS_BOARD_PIN_I2C_SDA,
                  SOLAR_OS_BOARD_PIN_I2C_SCL);
    SOLAR_OS_LOGI(TAG,
                  "SDMMC pins: CLK=%d CMD=%d D0=%d",
                  SOLAR_OS_BOARD_PIN_SDMMC_CLK,
                  SOLAR_OS_BOARD_PIN_SDMMC_CMD,
                  SOLAR_OS_BOARD_PIN_SDMMC_D0);
    SOLAR_OS_LOGI(TAG,
                  "UART%d pins: TX=%d RX=%d",
                  (int)SOLAR_OS_BOARD_UART_PORT,
                  SOLAR_OS_BOARD_PIN_UART_TX,
                  SOLAR_OS_BOARD_PIN_UART_RX);
    SOLAR_OS_LOGI(TAG, "Expansion GPIOs: %s", SOLAR_OS_BOARD_EXPANSION_GPIO_LIST);
    SOLAR_OS_LOGI(TAG, "Runtime GPIOs: %s", SOLAR_OS_BOARD_USER_GPIO_LIST);
    SOLAR_OS_LOGI(TAG, "KEY pin: %d", SOLAR_OS_BOARD_PIN_KEY);
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
    const esp_err_t err = rlcd_st7305_resume(&lcd);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "display resume failed: %s", esp_err_to_name(err));
        return;
    }

    if (solar_os_context_graphics_active(&os_ctx)) {
        dispatch_app_resume(now_ms);
    } else {
        terminal->dirty = true;
        draw_terminal_if_needed();
    }
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

    esp_err_t err = key_prepare_rtc_wakeup();
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "KEY RTC wake GPIO setup failed: %s", esp_err_to_name(err));
        key_restore_gpio_after_rtc();
        return;
    }

    if (!wait_key_rtc_released_stable(KEY_RELEASE_STABLE_MS, KEY_RELEASE_STABLE_TIMEOUT_MS)) {
        SOLAR_OS_LOGW(TAG, "%s: sleep cancelled, RTC key release was not stable", reason);
        key_restore_gpio_after_rtc();
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
        return;
    }

    err = esp_sleep_enable_ext1_wakeup_io(KEY_WAKE_MASK, KEY_WAKE_MODE);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "KEY sleep source setup failed: %s", esp_err_to_name(err));
        (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
        key_restore_gpio_after_rtc();
        return;
    }

    const esp_err_t ble_sleep_err =
        solar_os_ble_keyboard_prepare_sleep(BLE_SLEEP_DISCONNECT_TIMEOUT_MS);
    if (ble_sleep_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "BLE keyboard sleep prepare failed: %s", esp_err_to_name(ble_sleep_err));
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
        solar_os_ble_keyboard_resume();
    } else {
        SOLAR_OS_LOGW(TAG, "light sleep rejected: %s", esp_err_to_name(err));
        solar_os_power_note_sleep_exit(now_ms, (int)wake_cause, wake_ext1, false);
        solar_os_ble_keyboard_resume();
    }

    update_status();
    resume_display_after_sleep(now_ms);
}

static void enter_key_light_sleep(void)
{
    enter_light_sleep("KEY short press");
}

static void key_button_init(void)
{
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
            enter_key_light_sleep();
        }
    }

    if (!down || key_long_press_fired || (now_ms - key_pressed_ms) < KEY_LONG_PRESS_MS) {
        return;
    }

    key_long_press_fired = true;
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

static void dispatch_keyboard_chars(void)
{
    char chars[32];
    size_t count;

    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        solar_os_power_note_activity(millis_u32());
        for (size_t i = 0; i < count; i++) {
            const solar_os_event_t event = {
                .type = SOLAR_OS_EVENT_CHAR,
                .data.ch = chars[i],
            };

            if (foreground_app != NULL && foreground_app->event != NULL) {
                if ((uint8_t)chars[i] == SOLAR_OS_KEY_APP_EXIT) {
                    SOLAR_OS_LOGI(TAG,
                                  "dispatch app-exit key to %s",
                                  foreground_app->name != NULL ? foreground_app->name : "?");
                }
                foreground_app->event(&os_ctx, &event);
            }
            process_app_requests();
        }
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

    if (foreground_app != NULL && foreground_app->event != NULL) {
        foreground_app->event(&os_ctx, &event);
        process_app_requests();
    }

    solar_os_jobs_tick(&os_ctx, now_ms);
    process_app_requests();
}

static void update_status(void)
{
    const uint32_t now_ms = millis_u32();
    if (last_status_update_ms != 0 &&
        (now_ms - last_status_update_ms) < STATUS_UPDATE_INTERVAL_MS) {
        return;
    }
    last_status_update_ms = now_ms;

    solar_os_status_bar_t status = {0};

    solar_os_battery_status_t battery;
    if (solar_os_battery_get_status(&battery) == ESP_OK) {
        status.battery_valid = true;
        status.battery_percent = battery.percent;
        status.battery_external_power = battery.external_power;
    }

    status.ble_connected = solar_os_ble_keyboard_is_connected();
    status.ble_scanning = solar_os_ble_keyboard_is_scanning();
    status.sd_mounted = solar_os_storage_is_mounted();

    solar_os_audio_status_t audio;
    solar_os_audio_get_status(&audio);
    status.audio_enabled = audio.initialized;
    status.audio_volume = audio.volume;

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    status.wifi_started = wifi.started;
    status.wifi_connected = wifi.connected;
    status.wifi_has_ip = wifi.has_ip;
    if (wifi.connected && wifi.has_ip) {
        status.wifi_level = wifi_level_from_rssi(wifi.rssi);
    }

    solar_os_datetime_t datetime;
    if (solar_os_time_get_datetime(&datetime) == ESP_OK &&
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

    const esp_err_t cdc_err = solar_os_cdc_init();
    if (cdc_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "CDC port unavailable: %s", esp_err_to_name(cdc_err));
    }

    const esp_err_t power_err = solar_os_power_init();
    if (power_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Power service unavailable: %s", esp_err_to_name(power_err));
    }
    solar_os_power_note_activity(millis_u32());

    const esp_err_t sd_err = solar_os_storage_init();
    if (sd_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "SD card unavailable: %s", esp_err_to_name(sd_err));
    }

    const esp_err_t battery_err = solar_os_battery_init();
    if (battery_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Battery monitor unavailable: %s", esp_err_to_name(battery_err));
    }

    const esp_err_t wifi_err = solar_os_wifi_init();
    if (wifi_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_err));
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

    const esp_err_t uart_err = solar_os_uart_init();
    if (uart_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "UART unavailable: %s", esp_err_to_name(uart_err));
    }

    const esp_err_t gpio_err = solar_os_gpio_init();
    if (gpio_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "GPIO service unavailable: %s", esp_err_to_name(gpio_err));
    }

    const esp_err_t adc_err = solar_os_adc_init();
    if (adc_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "ADC service unavailable: %s", esp_err_to_name(adc_err));
    }

    const esp_err_t pwm_err = solar_os_pwm_init();
    if (pwm_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "PWM service unavailable: %s", esp_err_to_name(pwm_err));
    }

    const esp_err_t i2c_err = solar_os_i2c_init();
    if (i2c_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "I2C unavailable: %s", esp_err_to_name(i2c_err));
    } else {
        const esp_err_t rtc_err = solar_os_time_init();
        if (rtc_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "RTC unavailable: %s", esp_err_to_name(rtc_err));
        }

        const esp_err_t shtc3_err = solar_os_sensors_init();
        if (shtc3_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "SHTC3 unavailable: %s", esp_err_to_name(shtc3_err));
        }
    }

    const esp_err_t ble_err = solar_os_ble_keyboard_init();
    if (ble_err != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "BLE keyboard init failed: %s", esp_err_to_name(ble_err));
    }
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

static bool switch_to_app(const solar_os_app_t *app)
{
    bool new_app_claimed = false;

    if (app == NULL) {
        return false;
    }
    if (app == foreground_app) {
        return true;
    }

    if (app != solar_os_shell_app() && !display_claim_app(app, &new_app_claimed)) {
        return false;
    }

    SOLAR_OS_LOGI(TAG,
                  "switch app: %s -> %s",
                  foreground_app != NULL && foreground_app->name != NULL ? foreground_app->name : "(none)",
                  app->name != NULL ? app->name : "?");

    if (foreground_app != NULL && foreground_app->stop != NULL) {
        SOLAR_OS_LOGI(TAG,
                      "stop app: %s",
                      foreground_app->name != NULL ? foreground_app->name : "?");
        foreground_app->stop(&os_ctx);
    }
    display_release_app(foreground_app);

    solar_os_context_set_graphics_active(&os_ctx, false);
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
    foreground_app = solar_os_shell_app();
    if (foreground_app != NULL && foreground_app->start != NULL) {
        const esp_err_t shell_err = foreground_app->start(&os_ctx);
        if (shell_err != ESP_OK) {
            SOLAR_OS_LOGE(TAG, "Shell failed to start: %s", esp_err_to_name(shell_err));
        }
    }
    return false;
}

static void process_app_requests(void)
{
    if (solar_os_context_take_exit_request(&os_ctx)) {
        SOLAR_OS_LOGI(TAG,
                      "exit request for foreground app: %s",
                      foreground_app != NULL && foreground_app->name != NULL ? foreground_app->name : "(none)");
        if (foreground_app != solar_os_shell_app()) {
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
}

static void maybe_enter_idle_sleep(void)
{
    if (foreground_app != solar_os_shell_app() || key_pressed || key_ignore_until_released) {
        return;
    }

    const uint32_t now_ms = millis_u32();
    if (solar_os_power_should_idle_sleep(now_ms)) {
        enter_light_sleep("power idle");
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

    const esp_err_t display_err = rlcd_st7305_init(&lcd);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(display_err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    solar_os_gfx_init(&gfx, rlcd_st7305_get_u8g2(&lcd));
    solar_os_splash_clear(&gfx);

    terminal = solar_os_psram_calloc(1, sizeof(*terminal));
    if (terminal == NULL) {
        ESP_LOGE(TAG, "Terminal allocation failed");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    solar_os_terminal_init(terminal, rlcd_st7305_get_u8g2(&lcd));
    solar_os_context_init(&os_ctx, terminal, &gfx);
    solar_os_splash_draw(&gfx, "starting services");
    ESP_ERROR_CHECK(solar_os_jobs_init());

    init_peripherals();
    update_status();

    switch_to_app(solar_os_shell_app());

    SOLAR_OS_LOGI(TAG, "SolarOS runtime started");

    while (true) {
        poll_key_button();
        dispatch_keyboard_chars();
        dispatch_app_tick();
        dispatch_keyboard_chars();
        process_app_requests();
        update_status();

        draw_terminal_if_needed();
        maybe_enter_idle_sleep();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
