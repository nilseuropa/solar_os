#include "solar_os_stc8h_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pwm_port.h"
#include "solar_os_buses.h"
#include "solar_os_cardkb_codec.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_task.h"

/*
 * STC8H companion-MCU keypad as wired on the Elecrow ThinkNode M9: a 37-key
 * QWERTY keypad scanned by a dedicated STC8H MCU that acts as an I2C slave.
 * The scancode-to-character mapping, the alt/sym layer, and long-press
 * detection all live inside the STC8H firmware; the host receives finished
 * bytes. Printable keys arrive as ASCII 0x20..0x7f and the remaining keys as
 * the fixed special codes below. The interrupt line idles low and is driven
 * high while a key is active; the pending key code is served from the key
 * register and cleared by the read.
 */

#define STC8H_REG_KEY 0x01U
#define STC8H_REG_LONG_PRESS_MS 0x03U
#define STC8H_LONG_PRESS_DEFAULT_MS 700U

/* Keycap codes measured on ThinkNode M9 revision 1.0 hardware. The alt and
 * triangle keys intermittently emit 0x88 and 0x90; both are ignored. */
#define STC8H_KEY_MESSAGES 0x81U
#define STC8H_KEY_HOME 0x82U
#define STC8H_KEY_MESSAGE 0x83U
#define STC8H_KEY_PIN 0x84U
#define STC8H_KEY_MAPS 0x85U
#define STC8H_KEY_BACK 0x86U
#define STC8H_KEY_PIN_LONG 0x87U
#define STC8H_KEY_INVALID 0x88U
#define STC8H_KEY_DELETE_LONG 0x89U

#define STC8H_COMPOSE_TIMEOUT_US 2000000LL

#define STC8H_POLL_MS 15U
#define STC8H_BACKLIGHT_PWM_HZ 5000U
#define STC8H_BACKLIGHT_DEFAULT_PERCENT 50U
#define STC8H_TASK_STACK 3072U
#define STC8H_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int backlight_pin;
    bool backlight_active;
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    bool compose_armed;
    int64_t compose_deadline_us;
    uint32_t keys;
    uint32_t unsupported;
    uint32_t dropped;
    uint32_t bus_errors;
} solar_os_stc8h_device_t;

static const char *TAG = "stc8h-keyboard";
static solar_os_stc8h_device_t stc8h_device;

static bool binding_role_is(const solar_os_expansion_binding_t *binding,
                            const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address,
                                uint8_t *alternate_address,
                                int *backlight_pin)
{
    bool have_i2c = false;
    bool have_address = false;
    bool have_alternate = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL ||
        alternate_address == NULL || backlight_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0U;
    *alternate_address = 0U;
    *backlight_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (binding->value != SOLAR_OS_STC8H_KEYBOARD_ADDRESS &&
                binding->value != SOLAR_OS_STC8H_KEYBOARD_ALT_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            if (binding_role_is(binding, "alt_addr")) {
                if (have_alternate) {
                    return ESP_ERR_INVALID_ARG;
                }
                *alternate_address = (uint8_t)binding->value;
                have_alternate = true;
            } else {
                if (have_address) {
                    return ESP_ERR_INVALID_ARG;
                }
                *address = (uint8_t)binding->value;
                have_address = true;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_PWM:
            if (binding_role_is(binding, "backlight") && *backlight_pin < 0) {
                *backlight_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static bool stc8h_decode(uint8_t value, uint8_t *key)
{
    switch (value) {
    case STC8H_KEY_HOME:
        /* The keypad's Home key leaves the current application. */
        *key = SOLAR_OS_KEY_APP_EXIT;
        return true;
    case STC8H_KEY_BACK:
        *key = SOLAR_OS_KEY_ESCAPE;
        return true;
    case STC8H_KEY_MESSAGES:
        *key = SOLAR_OS_KEY_F1;
        return true;
    case STC8H_KEY_MAPS:
        *key = SOLAR_OS_KEY_F2;
        return true;
    case STC8H_KEY_PIN:
        *key = SOLAR_OS_KEY_F3;
        return true;
    case STC8H_KEY_PIN_LONG:
        *key = SOLAR_OS_KEY_F4;
        return true;
    case STC8H_KEY_DELETE_LONG:
        *key = SOLAR_OS_KEY_DELETE;
        return true;
    case STC8H_KEY_INVALID:
        return false;
    default:
        break;
    }
    /* Arrow, enter, backspace, and printable codes match the CardKB map. */
    return solar_os_cardkb_decode(value, key);
}

/* The STC8H scanner cannot emit tab, pipe, backslash, brackets, or several
 * other programmer characters: its sym layer is fixed in the keypad MCU and
 * the modifier keys are invisible to the host. The Message key therefore
 * arms a one-shot compose layer for the next key. The pairings follow the
 * character's US-keyboard shift position or its mnemonic. */
static bool stc8h_compose(uint8_t key, uint8_t *composed)
{
    switch (key) {
    case ' ':
    case 't':
        *composed = '\t';
        return true;
    case 'p':
        *composed = '|';
        return true;
    case 'b':
        *composed = '\\';
        return true;
    case 'e':
        *composed = '=';
        return true;
    case '5':
        *composed = '%';
        return true;
    case '6':
        *composed = '^';
        return true;
    case '9':
        *composed = '[';
        return true;
    case '0':
        *composed = ']';
        return true;
    case '(':
        *composed = '{';
        return true;
    case ')':
        *composed = '}';
        return true;
    case ',':
        *composed = '<';
        return true;
    case '.':
        *composed = '>';
        return true;
    case 'g':
        *composed = '`';
        return true;
    case 'n':
        *composed = '~';
        return true;
    default:
        return false;
    }
}

static void process_pending_key(solar_os_stc8h_device_t *device,
                                bool *bus_error_reported)
{
    uint8_t value = 0U;
    const esp_err_t read_err = solar_os_bus_i2c_read_reg(device->i2c_bus,
                                                         device->address,
                                                         STC8H_REG_KEY,
                                                         &value,
                                                         1);
    if (read_err != ESP_OK) {
        device->bus_errors++;
        if (!*bus_error_reported) {
            ESP_LOGW(TAG,
                     "%s read failed on %s: %s",
                     device->name,
                     device->i2c_bus,
                     esp_err_to_name(read_err));
            *bus_error_reported = true;
        }
        return;
    }
    *bus_error_reported = false;
    if (value == 0U || value == 0xffU) {
        return;
    }
    if (value == STC8H_KEY_MESSAGE) {
        /* The Message key arms the one-shot compose layer; a second press
         * before the timeout disarms it. An expired layer counts as
         * disarmed, so the press re-arms it. */
        const int64_t now_us = esp_timer_get_time();
        device->compose_armed = !device->compose_armed ||
            now_us >= device->compose_deadline_us;
        device->compose_deadline_us = now_us + STC8H_COMPOSE_TIMEOUT_US;
        return;
    }
    uint8_t key = 0U;
    if (!stc8h_decode(value, &key)) {
        device->unsupported++;
        return;
    }
    if (device->compose_armed) {
        if (esp_timer_get_time() >= device->compose_deadline_us) {
            device->compose_armed = false;
        } else if (key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_DOWN ||
                   key == SOLAR_OS_KEY_RIGHT) {
            /* Composed up and down page through the scrollback and
             * composed right sends tab. The layer stays armed so repeated
             * presses keep working. */
            switch (key) {
            case SOLAR_OS_KEY_UP:
                key = SOLAR_OS_KEY_PAGE_UP;
                break;
            case SOLAR_OS_KEY_DOWN:
                key = SOLAR_OS_KEY_PAGE_DOWN;
                break;
            default:
                key = (uint8_t)'\t';
                break;
            }
            device->compose_deadline_us =
                esp_timer_get_time() + STC8H_COMPOSE_TIMEOUT_US;
        } else {
            device->compose_armed = false;
            uint8_t composed = 0U;
            if (stc8h_compose(key, &composed)) {
                key = composed;
            }
        }
    }
    if (solar_os_input_write_char(device->input_source,
                                  (char)key) != ESP_OK) {
        device->dropped++;
    } else {
        device->keys++;
    }
}

static void stc8h_worker(void *arg)
{
    solar_os_stc8h_device_t *device = arg;
    bool bus_error_reported = false;

    while (!device->stop_requested) {
        /* The key register clears on read and reads back zero with no key
         * pending, so polling it every tick is idempotent. The interrupt
         * line is not a reliable read gate: its pulse can be shorter than
         * the poll period. */
        process_pending_key(device, &bus_error_reported);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STC8H_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t configure_long_press(solar_os_stc8h_device_t *device)
{
    const uint8_t value[2] = {
        (uint8_t)(STC8H_LONG_PRESS_DEFAULT_MS >> 8),
        (uint8_t)(STC8H_LONG_PRESS_DEFAULT_MS & 0xffU),
    };
    return solar_os_bus_i2c_write_reg(device->i2c_bus,
                                      device->address,
                                      STC8H_REG_LONG_PRESS_MS,
                                      value,
                                      sizeof(value));
}

static void clear_device(solar_os_stc8h_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    if (device->backlight_active) {
        const esp_err_t err = pwm_port_stop((gpio_num_t)device->backlight_pin);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "%s backlight stop failed: %s",
                     device->name,
                     esp_err_to_name(err));
        }
    }
    memset(device, 0, sizeof(*device));
    device->backlight_pin = -1;
}

esp_err_t solar_os_stc8h_keyboard_attach(const char *name,
                                         const solar_os_expansion_binding_t *bindings,
                                         size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    uint8_t alternate_address = 0U;
    int backlight_pin = -1;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (stc8h_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address,
                                       &alternate_address,
                                       &backlight_pin),
                        TAG,
                        "invalid bindings");

    /* The keypad address identifies the board revision: 0x6c on revision
     * 1.0 boards and 0x6d on revision 1.1. Fall back to the alternate when
     * the configured address does not answer. */
    esp_err_t err = solar_os_bus_i2c_probe(i2c_bus, address);
    if (err != ESP_OK && alternate_address != 0U &&
        alternate_address != address) {
        err = solar_os_bus_i2c_probe(i2c_bus, alternate_address);
        if (err == ESP_OK) {
            address = alternate_address;
        }
    }
    ESP_RETURN_ON_ERROR(err, TAG, "STC8H keypad not found");

    clear_device(&stc8h_device);
    stc8h_device.address = address;
    stc8h_device.backlight_pin = backlight_pin;
    strlcpy(stc8h_device.name, name, sizeof(stc8h_device.name));
    strlcpy(stc8h_device.i2c_bus, i2c_bus, sizeof(stc8h_device.i2c_bus));

    err = configure_long_press(&stc8h_device);
    if (err != ESP_OK) {
        clear_device(&stc8h_device);
        return err;
    }

    if (backlight_pin >= 0) {
        err = pwm_port_set((gpio_num_t)backlight_pin,
                           STC8H_BACKLIGHT_PWM_HZ,
                           STC8H_BACKLIGHT_DEFAULT_PERCENT);
        if (err != ESP_OK) {
            clear_device(&stc8h_device);
            return err;
        }
        stc8h_device.backlight_active = true;
    }

    err = solar_os_input_keyboard_source_open(stc8h_device.name,
                                              true,
                                              &stc8h_device.input_source);
    if (err != ESP_OK) {
        clear_device(&stc8h_device);
        return err;
    }
    if (solar_os_task_create_pinned_internal(stc8h_worker,
                                             stc8h_device.name,
                                             STC8H_TASK_STACK,
                                             &stc8h_device,
                                             STC8H_TASK_PRIORITY,
                                             &stc8h_device.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(&stc8h_device);
        return ESP_ERR_NO_MEM;
    }
    stc8h_device.active = true;

    ESP_LOGI(TAG,
             "%s attached on %s address 0x%02x (board revision %s)",
             name,
             i2c_bus,
             address,
             address == SOLAR_OS_STC8H_KEYBOARD_ADDRESS ? "1.0" : "1.1");
    return ESP_OK;
}

esp_err_t solar_os_stc8h_keyboard_detach(const char *name)
{
    if (!stc8h_device.active || name == NULL ||
        strcmp(stc8h_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    stc8h_device.stop_requested = true;
    if (stc8h_device.worker_task != NULL) {
        (void)xTaskNotifyGive(stc8h_device.worker_task);
    }
    if (!solar_os_task_wait_done(stc8h_device.worker_task,
                                 &stc8h_device.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "%s detached: %lu keys, %lu unsupported, %lu dropped, %lu bus errors",
             name,
             (unsigned long)stc8h_device.keys,
             (unsigned long)stc8h_device.unsupported,
             (unsigned long)stc8h_device.dropped,
             (unsigned long)stc8h_device.bus_errors);
    clear_device(&stc8h_device);
    return ESP_OK;
}
