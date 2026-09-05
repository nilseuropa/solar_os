#include "solar_os_tca8418.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_task.h"

/*
 * TCA8418 matrix keyboard scanner as wired on the LilyGO T-LoRa-Pager: a
 * fixed 4x10 matrix behind a QWERTY keycap set. The register map is the
 * generic TI TCA8418 map; the matrix size, keymap, and modifier-key raw
 * values below are specific to this board and are ported directly from
 * LilyGO's own LilyGoKeyboard.cpp reference implementation (raw special-key
 * values 0x1E/0x1C/0x14/0x1D/0x19, and the "blank matrix cell means space"
 * quirk) so key behavior matches stock firmware.
 */

#define TCA8418_REG_CFG 0x01U
#define TCA8418_REG_INT_STAT 0x02U
#define TCA8418_REG_KEY_LCK_EC 0x03U
#define TCA8418_REG_KEY_EVENT_A 0x04U
#define TCA8418_REG_KP_GPIO_1 0x1DU
#define TCA8418_REG_KP_GPIO_2 0x1EU
#define TCA8418_REG_KP_GPIO_3 0x1FU

#define TCA8418_CFG_KE_IEN 0x01U
#define TCA8418_INT_STAT_K_INT 0x01U
#define TCA8418_KEY_LCK_EC_COUNT_MASK 0x0FU
#define TCA8418_EVENT_PRESSED_BIT 0x80U
#define TCA8418_EVENT_FIFO_MAX 10U
#define TCA8418_EVENT_DRAIN_MAX 32U

#define TCA8418_KB_ROWS 4U
#define TCA8418_KB_COLS 10U
#define TCA8418_SYMBOL_KEY 0x1EU
#define TCA8418_CAPS_KEY 0x1CU
#define TCA8418_ALT_KEY 0x14U
#define TCA8418_BACKSPACE_KEY 0x1DU
#define TCA8418_ALT_BRIGHTNESS_KEY 0x19U

#define TCA8418_POLL_MS 15U
#define TCA8418_TASK_STACK 3072U
#define TCA8418_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

/* 4x10 character map, ported verbatim from LilyGoLib's LilyGo_LoRa_Pager.cpp. */
static const char keymap[TCA8418_KB_ROWS][TCA8418_KB_COLS] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\n'},
    {'\0', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '\0', '\0'},
    {' ', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};
static const char symbol_map[TCA8418_KB_ROWS][TCA8418_KB_COLS] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'*', '/', '+', '-', '=', ':', '\'', '"', '@', '\0'},
    {'\0', '_', '$', ';', '?', '!', ',', '.', '\0', '\0'},
    {' ', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    bool symbol_pressed;
    bool caps_pressed;
    bool alt_pressed;
    char last_key_val;
    uint32_t keys;
    uint32_t dropped;
    uint32_t bus_errors;
} solar_os_tca8418_device_t;

static const char *TAG = "tca8418";
static solar_os_tca8418_device_t tca_device;

static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address,
                                int *irq_pin)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL || irq_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0U;
    *irq_pin = -1;

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
            if (have_address || binding->value != SOLAR_OS_TCA8418_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (!binding_role_is(binding, "irq") || *irq_pin >= 0) {
                return ESP_ERR_INVALID_ARG;
            }
            *irq_pin = binding->value;
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

static esp_err_t read_reg(solar_os_tca8418_device_t *device, uint8_t reg, uint8_t *value)
{
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, value, 1);
}

static esp_err_t write_reg(solar_os_tca8418_device_t *device, uint8_t reg, uint8_t value)
{
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, reg, &value, 1);
}

static esp_err_t configure_matrix(solar_os_tca8418_device_t *device)
{
    /* Enable ROW0-3 and COL0-9 for keypad matrix scanning. */
    ESP_RETURN_ON_ERROR(write_reg(device, TCA8418_REG_KP_GPIO_1, 0x0FU), TAG, "KP_GPIO_1 failed");
    ESP_RETURN_ON_ERROR(write_reg(device, TCA8418_REG_KP_GPIO_2, 0xFFU), TAG, "KP_GPIO_2 failed");
    ESP_RETURN_ON_ERROR(write_reg(device, TCA8418_REG_KP_GPIO_3, 0x03U), TAG, "KP_GPIO_3 failed");
    ESP_RETURN_ON_ERROR(write_reg(device, TCA8418_REG_CFG, TCA8418_CFG_KE_IEN), TAG, "CFG failed");

    /* Drain any stale events left in the FIFO from before power-up. */
    for (uint32_t i = 0; i < TCA8418_EVENT_FIFO_MAX; i++) {
        uint8_t count = 0U;
        if (read_reg(device, TCA8418_REG_KEY_LCK_EC, &count) != ESP_OK) {
            break;
        }
        if ((count & TCA8418_KEY_LCK_EC_COUNT_MASK) == 0U) {
            break;
        }
        uint8_t discard = 0U;
        (void)read_reg(device, TCA8418_REG_KEY_EVENT_A, &discard);
    }
    return write_reg(device, TCA8418_REG_INT_STAT, 0xFFU);
}

static char key_char(solar_os_tca8418_device_t *device, uint8_t row, uint8_t col)
{
    if (row >= TCA8418_KB_ROWS || col >= TCA8418_KB_COLS) {
        return '\0';
    }
    char value = device->symbol_pressed ? symbol_map[row][col] : keymap[row][col];
    if (!device->symbol_pressed && device->caps_pressed && value != '\0') {
        value = (char)toupper((unsigned char)value);
    }
    return value;
}

/* Ported from LilyGoKeyboard::handleSpaceAndNullChar (has_symbol_key == false
 * branch, which is what the T-LoRa-Pager config uses). A blank matrix cell
 * with no symbol layer active is reported as a space; this also forces
 * `pressed` true, matching upstream's behavior exactly. */
static char apply_space_quirk(solar_os_tca8418_device_t *device, char value, bool *pressed)
{
    if (device->symbol_pressed && value == ' ') {
        return '\0';
    }
    if (!device->symbol_pressed && device->last_key_val == '\0') {
        *pressed = true;
        return ' ';
    }
    return value;
}

/* Returns true if the raw code was a modifier/backspace key and has already
 * been fully handled (no further character lookup needed).
 *
 * Symbol/Caps/Alt are momentary (hold-to-activate) keys on stock firmware:
 * the flag is flipped on *every* event, press and release alike, which nets
 * out to "active only while the key is held down". This is intentional and
 * matches LilyGoKeyboard::handleSpecialKeys exactly - do not gate these on
 * `pressed`. */
static bool handle_special_key(solar_os_tca8418_device_t *device,
                               uint8_t k,
                               bool pressed)
{
    if (k == TCA8418_SYMBOL_KEY) {
        device->symbol_pressed = !device->symbol_pressed;
        return true;
    }
    if (k == TCA8418_CAPS_KEY) {
        device->caps_pressed = !device->caps_pressed;
        return true;
    }
    if (k == TCA8418_ALT_KEY) {
        device->alt_pressed = !device->alt_pressed;
        return true;
    }
    if (k == TCA8418_BACKSPACE_KEY) {
        if (pressed) {
            if (solar_os_input_write_char(device->input_source, '\b') != ESP_OK) {
                device->dropped++;
            } else {
                device->keys++;
            }
        }
        return true;
    }
    if (device->alt_pressed && k == TCA8418_ALT_BRIGHTNESS_KEY) {
        /* Alt+B toggles the backlight on stock firmware; brightness control
         * is not wired up yet on this port, so just swallow the key. */
        return true;
    }
    return false;
}

static void process_event(solar_os_tca8418_device_t *device, uint8_t raw)
{
    if (raw == 0U) {
        return;
    }
    bool pressed = (raw & TCA8418_EVENT_PRESSED_BIT) != 0U;
    uint8_t k = (uint8_t)(raw & 0x7FU);
    if (k == 0U || k > 96U) {
        return;
    }
    k--;
    const uint8_t row = (uint8_t)(k / TCA8418_KB_COLS);
    if (row >= TCA8418_KB_ROWS) {
        return;
    }
    if (handle_special_key(device, k, pressed)) {
        return;
    }

    const uint8_t col = (uint8_t)(k % TCA8418_KB_COLS);
    char value = key_char(device, row, col);
    value = apply_space_quirk(device, value, &pressed);
    device->last_key_val = value;

    if (!pressed || value == '\0') {
        return;
    }
    if (solar_os_input_write_char(device->input_source, value) != ESP_OK) {
        device->dropped++;
    } else {
        device->keys++;
    }
}

static esp_err_t poll_once(solar_os_tca8418_device_t *device)
{
    uint8_t int_stat = 0U;
    ESP_RETURN_ON_ERROR(read_reg(device, TCA8418_REG_INT_STAT, &int_stat), TAG, "INT_STAT read failed");
    if ((int_stat & TCA8418_INT_STAT_K_INT) == 0U) {
        return ESP_OK;
    }

    uint32_t drained = 0U;
    while (drained < TCA8418_EVENT_DRAIN_MAX) {
        uint8_t count = 0U;
        ESP_RETURN_ON_ERROR(read_reg(device, TCA8418_REG_KEY_LCK_EC, &count), TAG, "KEY_LCK_EC read failed");
        if ((count & TCA8418_KEY_LCK_EC_COUNT_MASK) == 0U) {
            break;
        }
        uint8_t raw = 0U;
        ESP_RETURN_ON_ERROR(read_reg(device, TCA8418_REG_KEY_EVENT_A, &raw), TAG, "KEY_EVENT_A read failed");
        process_event(device, raw);
        drained++;
    }
    return write_reg(device, TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);
}

static void tca8418_worker(void *arg)
{
    solar_os_tca8418_device_t *device = arg;
    bool bus_error_reported = false;

    while (!device->stop_requested) {
        const esp_err_t err = poll_once(device);
        if (err == ESP_OK) {
            bus_error_reported = false;
        } else {
            device->bus_errors++;
            if (!bus_error_reported) {
                ESP_LOGW(TAG, "%s poll failed on %s: %s", device->name, device->i2c_bus, esp_err_to_name(err));
                bus_error_reported = true;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TCA8418_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(solar_os_tca8418_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
    device->last_key_val = '\0';
}

esp_err_t solar_os_tca8418_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    int irq_pin = -1;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (tca_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, i2c_bus, sizeof(i2c_bus), &address, &irq_pin),
                        TAG, "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address), TAG, "TCA8418 not found");

    if (irq_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "irq gpio config failed");
    }

    clear_device(&tca_device);
    tca_device.active = true;
    tca_device.address = address;
    tca_device.last_key_val = '\0';
    strlcpy(tca_device.name, name, sizeof(tca_device.name));
    strlcpy(tca_device.i2c_bus, i2c_bus, sizeof(tca_device.i2c_bus));

    ESP_RETURN_ON_ERROR(configure_matrix(&tca_device), TAG, "matrix configuration failed");

    esp_err_t err = solar_os_input_keyboard_source_open(tca_device.name, true, &tca_device.input_source);
    if (err != ESP_OK) {
        clear_device(&tca_device);
        return err;
    }
    if (solar_os_task_create_pinned_internal(tca8418_worker,
                                             tca_device.name,
                                             TCA8418_TASK_STACK,
                                             &tca_device,
                                             TCA8418_TASK_PRIORITY,
                                             &tca_device.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(&tca_device);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "%s attached on %s address 0x%02x", name, i2c_bus, address);
    return ESP_OK;
}

esp_err_t solar_os_tca8418_detach(const char *name)
{
    if (!tca_device.active || name == NULL || strcmp(tca_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    tca_device.stop_requested = true;
    if (tca_device.worker_task != NULL) {
        (void)xTaskNotifyGive(tca_device.worker_task);
    }
    if (!solar_os_task_wait_done(tca_device.worker_task, &tca_device.worker_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "%s detached: %lu keys, %lu dropped, %lu bus errors",
             name,
             (unsigned long)tca_device.keys,
             (unsigned long)tca_device.dropped,
             (unsigned long)tca_device.bus_errors);
    clear_device(&tca_device);
    return ESP_OK;
}
