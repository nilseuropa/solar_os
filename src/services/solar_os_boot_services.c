#include "solar_os_boot_services.h"

#include "esp_err.h"
#include "solar_os_config.h"
#include "solar_os_adc.h"
#include "solar_os_adc_dpad.h"
#include "solar_os_audio.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_board_caps.h"
#include "solar_os_buttons.h"
#include "solar_os_cdc.h"
#if SOLAR_OS_PACKAGE_SERVICE_CHAT
#include "solar_os_chat.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
#include "solar_os_docs.h"
#endif
#include "solar_os_engines.h"
#include "solar_os_expansion.h"
#include "solar_os_gpio.h"
#if SOLAR_OS_PACKAGE_SERVICE_HID
#include "solar_os_hid.h"
#endif
#include "solar_os_i2c.h"
#include "solar_os_identity.h"
#if SOLAR_OS_PACKAGE_SERVICE_INBOX
#include "solar_os_inbox.h"
#endif
#include "solar_os_joystick.h"
#if SOLAR_OS_PACKAGE_SERVICE_JSON
#include "solar_os_json.h"
#endif
#include "solar_os_log.h"
#if SOLAR_OS_PACKAGE_SERVICE_MQTT
#include "solar_os_mqtt.h"
#endif
#include "solar_os_onewire.h"
#if SOLAR_OS_PACKAGE_SERVICE_OTA
#include "solar_os_ota.h"
#endif
#include "solar_os_port.h"
#include "solar_os_power.h"
#include "solar_os_pointer.h"
#include "solar_os_pwm.h"
#include "solar_os_radio.h"
#include "solar_os_resources.h"
#include "solar_os_sensors.h"
#include "solar_os_spi.h"
#include "solar_os_status_led.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_time.h"
#include "solar_os_uart.h"
#include "solar_os_wifi.h"
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
#include "solar_os_wireguard.h"
#endif

static const char *TAG = "boot_services";

void solar_os_boot_services_init(uint32_t now_ms)
{
#if SOLAR_OS_PACKAGE_SERVICE_JSON
    const esp_err_t json_err = solar_os_json_init();
    if (json_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "JSON service unavailable: %s", esp_err_to_name(json_err));
    }
#endif

    const esp_err_t stream_err = solar_os_stream_init();
    if (stream_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Stream service unavailable: %s",
                      esp_err_to_name(stream_err));
    }

    const esp_err_t port_err = solar_os_port_init();
    if (port_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Port service unavailable: %s", esp_err_to_name(port_err));
    }

    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_CDC)) {
        const esp_err_t cdc_err = solar_os_cdc_init();
        if (cdc_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "CDC port unavailable: %s", esp_err_to_name(cdc_err));
        }
#if SOLAR_OS_PACKAGE_SERVICE_HID
        const esp_err_t hid_err = solar_os_hid_init();
        if (hid_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "USB HID unavailable: %s", esp_err_to_name(hid_err));
        }
#endif
    }

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_AUDIO)) {
        const esp_err_t audio_err = solar_os_audio_register_streams();
        if (audio_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Audio streams unavailable: %s",
                          esp_err_to_name(audio_err));
        }
    }
#endif

    const esp_err_t power_err = solar_os_power_init();
    if (power_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Power service unavailable: %s", esp_err_to_name(power_err));
    }
    solar_os_power_note_activity(now_ms);

    const esp_err_t storage_err = solar_os_storage_init();
    if (storage_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Default storage unavailable: %s", esp_err_to_name(storage_err));
    }
    const esp_err_t identity_err = solar_os_identity_init();
    if (identity_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Identity unavailable: %s", esp_err_to_name(identity_err));
    }
#if SOLAR_OS_PACKAGE_SERVICE_INBOX
    const esp_err_t inbox_err = solar_os_inbox_init();
    if (inbox_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Inbox service unavailable: %s", esp_err_to_name(inbox_err));
    }
#endif

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
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_BATTERY)) {
        const esp_err_t battery_err = solar_os_battery_init();
        if (battery_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Battery monitor unavailable: %s", esp_err_to_name(battery_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_WIFI)) {
        const esp_err_t wifi_err = solar_os_wifi_init();
        if (wifi_err == ESP_ERR_NOT_ALLOWED) {
            SOLAR_OS_LOGI(TAG, "Wi-Fi disabled by saved boot setting");
        } else if (wifi_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_WIFI)) {
        const esp_err_t wireguard_err = solar_os_wireguard_init();
        if (wireguard_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "WireGuard unavailable: %s",
                          esp_err_to_name(wireguard_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_OTA
    const esp_err_t ota_err = solar_os_ota_init();
    if (ota_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "OTA service unavailable: %s", esp_err_to_name(ota_err));
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    const esp_err_t docs_err = solar_os_docs_init();
    if (docs_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "External documentation unavailable: %s",
                      esp_err_to_name(docs_err));
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MQTT
    const esp_err_t mqtt_err = solar_os_mqtt_init();
    if (mqtt_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "MQTT service unavailable: %s", esp_err_to_name(mqtt_err));
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_CHAT
    const esp_err_t chat_err = solar_os_chat_init();
    if (chat_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Chat service unavailable: %s", esp_err_to_name(chat_err));
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UART
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_UART)) {
        const esp_err_t uart_err = solar_os_uart_init();
        if (uart_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "UART unavailable: %s", esp_err_to_name(uart_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_GPIO)) {
        const esp_err_t gpio_err = solar_os_gpio_init();
        if (gpio_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "GPIO service unavailable: %s", esp_err_to_name(gpio_err));
        }
    }

    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_STATUS_LED)) {
        const esp_err_t led_err = solar_os_status_led_init();
        if (led_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(led_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_GPIO)) {
        const esp_err_t onewire_err = solar_os_onewire_init();
        if (onewire_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "1-Wire service unavailable: %s", esp_err_to_name(onewire_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_ADC)) {
        const esp_err_t adc_err = solar_os_adc_init();
        if (adc_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "ADC service unavailable: %s", esp_err_to_name(adc_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PWM
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_PWM)) {
        const esp_err_t pwm_err = solar_os_pwm_init();
        if (pwm_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "PWM service unavailable: %s", esp_err_to_name(pwm_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BUTTONS
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_BUTTONS)) {
        const esp_err_t buttons_err = solar_os_buttons_init();
        if (buttons_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Board buttons unavailable: %s", esp_err_to_name(buttons_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_JOYSTICK
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_JOYSTICK)) {
        const esp_err_t joystick_err = solar_os_joystick_init();
        if (joystick_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "Joystick unavailable: %s", esp_err_to_name(joystick_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_ADC_DPAD)) {
        const esp_err_t dpad_err = solar_os_adc_dpad_init();
        if (dpad_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "ADC D-pad unavailable: %s", esp_err_to_name(dpad_err));
        }
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_I2C
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        const esp_err_t i2c_err = solar_os_i2c_init();
        if (i2c_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "I2C unavailable: %s", esp_err_to_name(i2c_err));
        } else {
#if SOLAR_OS_BOARD_HAS_POINTER
            if (solar_os_board_has(SOLAR_OS_BOARD_CAP_POINTER)) {
                const esp_err_t pointer_err = solar_os_pointer_init();
                if (pointer_err != ESP_OK) {
                    SOLAR_OS_LOGW(TAG,
                                  "Pointer unavailable: %s",
                                  esp_err_to_name(pointer_err));
                }
            }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RTC
            if (solar_os_board_has(SOLAR_OS_BOARD_CAP_RTC)) {
                const esp_err_t rtc_err = solar_os_time_init();
                if (rtc_err != ESP_OK) {
                    SOLAR_OS_LOGW(TAG, "RTC unavailable: %s", esp_err_to_name(rtc_err));
                }
            }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
            if (solar_os_board_has(SOLAR_OS_BOARD_CAP_TEMPERATURE) ||
                solar_os_board_has(SOLAR_OS_BOARD_CAP_HUMIDITY)) {
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
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_SPI)) {
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
            SOLAR_OS_LOGW(TAG,
                          "Expansion initialization incomplete: %s",
                          esp_err_to_name(expansion_err));
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
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_BLE)) {
        const esp_err_t ble_err = solar_os_ble_keyboard_init();
        if (ble_err == ESP_ERR_NOT_ALLOWED) {
            SOLAR_OS_LOGI(TAG, "BLE disabled by boot preference");
        } else if (ble_err != ESP_OK) {
            SOLAR_OS_LOGE(TAG, "BLE keyboard init failed: %s", esp_err_to_name(ble_err));
        }
    }
#endif
}
