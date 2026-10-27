#include "solar_os_cl32_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "solar_os_backlight.h"
#include "solar_os_cl32_keyboard.h"
#include "solar_os_input.h"
#include "solar_os_task.h"
#include "solar_os_power.h"

#define CL32_CORE_POLL_MS 10U
#define CL32_CORE_TASK_STACK 3072U
#define CL32_CORE_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define CL32_CORE_EVENT_FIFO_SIZE 10U
#define CL32_CORE_EVENT_DRAIN_MAX 32U
#define CL32_CORE_REG_STATUS 0x01U
#define CL32_CORE_REG_INTERRUPT 0x02U
#define CL32_CORE_REG_EVENT_COUNT 0x03U
#define CL32_CORE_REG_EVENT 0x04U
#define CL32_CORE_REG_BACKLIGHT 0x0FU
#define CL32_CORE_REG_BATTERY_VOLTAGE 0x14U
#define CL32_CORE_STATUS_USB (1U << 1)
#define CL32_CORE_STATUS_CHARGING (1U << 2)
#define CL32_CORE_INTERRUPT_KEYBOARD (1U << 0)
#define CL32_CORE_INTERRUPT_POWER ((1U << 1) | (1U << 2) | (1U << 3))
#define CL32_CORE_KEYBOARD_NAME "keyboard0"
#define CL32_CORE_BATTERY_NAME "battery0"
#define CL32_CORE_BATTERY_MV_PER_COUNT 25U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    solar_os_input_source_t input_source;
    solar_os_cl32_keyboard_t keyboard;
    TaskHandle_t worker_task;
    uint32_t transitions;
    uint32_t state_events;
    uint32_t unsupported;
    uint32_t dropped;
    uint32_t bus_errors;
    uint32_t invalid_counts;
} solar_os_cl32_core_device_t;

static const char *TAG = "cl32-core";
static solar_os_cl32_core_device_t cl32_core;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0U;

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
            if (have_address || binding->value != SOLAR_OS_CL32_CORE_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
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

static esp_err_t read_register(solar_os_cl32_core_device_t *device,
                               uint8_t reg,
                               uint8_t *value)
{
    return solar_os_bus_i2c_read_reg(device->i2c_bus,
                                     device->address,
                                     reg,
                                     value,
                                     sizeof(*value));
}

static esp_err_t power_off(void *user)
{
    solar_os_cl32_core_device_t *device = user;
    if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE;
    const uint8_t status = 0xffU;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                     CL32_CORE_REG_STATUS, &status, sizeof(status));
}

static esp_err_t battery_read(void *user, solar_os_battery_sample_t *sample)
{
    solar_os_cl32_core_device_t *device = user;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0U;
    esp_err_t err = read_register(device, CL32_CORE_REG_STATUS, &status);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t raw_voltage = 0U;
    err = read_register(device,
                        CL32_CORE_REG_BATTERY_VOLTAGE,
                        &raw_voltage);
    if (err != ESP_OK) {
        return err;
    }
    *sample = (solar_os_battery_sample_t) {
        .battery_mv = (uint16_t)raw_voltage * CL32_CORE_BATTERY_MV_PER_COUNT,
        .calibrated = true,
        .external_power_valid = true,
        .external_power = (status & CL32_CORE_STATUS_USB) != 0U,
        .charging_valid = true,
        .charging = (status & CL32_CORE_STATUS_CHARGING) != 0U,
    };
    return ESP_OK;
}

static esp_err_t acknowledge_core_interrupts(
    solar_os_cl32_core_device_t *device)
{
    uint8_t interrupts = 0U;
    esp_err_t err = read_register(device,
                                  CL32_CORE_REG_INTERRUPT,
                                  &interrupts);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t handled = CL32_CORE_INTERRUPT_KEYBOARD | CL32_CORE_INTERRUPT_POWER;
    if (interrupts != 0U && (interrupts & handled) == 0U) {
        return ESP_OK;
    }

    /* Power state is read directly by battery_read(). Its notification bits
     * must also be acknowledged to release the shared active-low wake line.
     * Write zero even when the register already reads zero: the AVR's IRQ
     * output can remain asserted after its event flags have cleared. */
    interrupts &= (uint8_t)~handled;
    return solar_os_bus_i2c_write_reg(device->i2c_bus,
                                      device->address,
                                      CL32_CORE_REG_INTERRUPT,
                                      &interrupts,
                                      sizeof(interrupts));
}

static void publish_wire_event(solar_os_cl32_core_device_t *device,
                               uint8_t wire_event)
{
    solar_os_cl32_key_transition_t transition;
    const solar_os_cl32_key_result_t result = solar_os_cl32_keyboard_decode(
        &device->keyboard,
        wire_event,
        &transition);
    if (result == SOLAR_OS_CL32_KEY_NONE) {
        device->state_events++;
        return;
    }
    if (result != SOLAR_OS_CL32_KEY_TRANSITION && result != SOLAR_OS_CL32_KEY_TAP) {
        device->unsupported++;
        return;
    }

    uint8_t key = transition.key;
    if (transition.pressed && key == 0U && transition.usage != 0U) {
        key = solar_os_input_translate_hid_usage(transition.usage,
                                                  transition.modifiers,
                                                  false);
    }
    const esp_err_t error = result == SOLAR_OS_CL32_KEY_TAP ?
        solar_os_input_write_key_tap(device->input_source,
                                     transition.physical_key,
                                     transition.usage,
                                     key,
                                     transition.modifiers) :
        solar_os_input_write_key(
            device->input_source,
            transition.physical_key,
            transition.usage,
            key,
            transition.modifiers,
            transition.pressed ? SOLAR_OS_INPUT_KEY_PRESS :
                                 SOLAR_OS_INPUT_KEY_RELEASE);
    if (error == ESP_OK) {
        device->transitions++;
    } else {
        device->dropped++;
    }
}

static esp_err_t poll_keyboard(solar_os_cl32_core_device_t *device)
{
    uint8_t event_count = 0U;
    esp_err_t err = read_register(device,
                                  CL32_CORE_REG_EVENT_COUNT,
                                  &event_count);
    if (err != ESP_OK) {
        return err;
    }
    if (event_count > CL32_CORE_EVENT_FIFO_SIZE) {
        device->invalid_counts++;
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t drained = 0U;
    while (event_count > 0U && drained < CL32_CORE_EVENT_DRAIN_MAX) {
        uint8_t wire_event = 0U;
        err = read_register(device, CL32_CORE_REG_EVENT, &wire_event);
        if (err != ESP_OK) {
            return err;
        }
        publish_wire_event(device, wire_event);
        drained++;

        err = read_register(device,
                            CL32_CORE_REG_EVENT_COUNT,
                            &event_count);
        if (err != ESP_OK) {
            return err;
        }
        if (event_count > CL32_CORE_EVENT_FIFO_SIZE) {
            device->invalid_counts++;
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (event_count == 0U) {
        err = acknowledge_core_interrupts(device);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void cl32_core_worker(void *arg)
{
    solar_os_cl32_core_device_t *device = arg;
    bool bus_error_reported = false;

    while (!device->stop_requested) {
        const esp_err_t err = poll_keyboard(device);
        if (err == ESP_OK) {
            bus_error_reported = false;
        } else {
            device->bus_errors++;
            if (!bus_error_reported) {
                ESP_LOGW(TAG,
                         "%s poll failed on %s: %s",
                         device->name,
                         device->i2c_bus,
                         esp_err_to_name(err));
                bus_error_reported = true;
            }
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CL32_CORE_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

/* CL-32/CL-32 Software/CL-32/lib/CL32/include/regMap.h: CL32_BL_BRIGHT.
 * The AVR retains ownership of its keyboard-activity timeout/fade. */
static esp_err_t set_backlight(void *user, uint8_t percent)
{
    const solar_os_cl32_core_device_t *device = user;
    const uint8_t value = (uint8_t)((percent * 255U + 50U) / 100U);
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                      CL32_CORE_REG_BACKLIGHT, &value, 1U);
}

static void clear_device(solar_os_cl32_core_device_t *device)
{
    solar_os_power_unregister_off_provider(device);
    if (device == NULL) {
        return;
    }
    solar_os_backlight_unregister(device);
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_cl32_core_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (cl32_core.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG,
                        "CL-32 core not found");

    memset(&cl32_core, 0, sizeof(cl32_core));
    cl32_core.active = true;
    cl32_core.address = address;
    strlcpy(cl32_core.name, name, sizeof(cl32_core.name));
    strlcpy(cl32_core.i2c_bus, i2c_bus, sizeof(cl32_core.i2c_bus));
    solar_os_cl32_keyboard_reset(&cl32_core.keyboard);

    esp_err_t err = solar_os_input_keyboard_source_open(
        CL32_CORE_KEYBOARD_NAME,
        true,
        &cl32_core.input_source);
    if (err != ESP_OK) {
        clear_device(&cl32_core);
        return err;
    }
    err = solar_os_backlight_register(set_backlight, &cl32_core);
    if (err != ESP_OK) {
        clear_device(&cl32_core);
        return err;
    }
    const solar_os_battery_provider_t battery_provider = {
        .read = battery_read,
        .user = &cl32_core,
    };
    err = solar_os_battery_register_provider(CL32_CORE_BATTERY_NAME,
                                              &battery_provider);
    if (err != ESP_OK) {
        clear_device(&cl32_core);
        return err;
    }
    if (solar_os_task_create_pinned_internal(cl32_core_worker,
                                             cl32_core.name,
                                             CL32_CORE_TASK_STACK,
                                             &cl32_core,
                                             CL32_CORE_TASK_PRIORITY,
                                             &cl32_core.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        (void)solar_os_battery_unregister_provider(CL32_CORE_BATTERY_NAME);
        clear_device(&cl32_core);
        return ESP_ERR_NO_MEM;
    }

    (void)solar_os_power_register_off_provider(name, power_off, &cl32_core);
    ESP_LOGI(TAG,
             "%s attached on %s address 0x%02x as %s and %s",
             name,
             i2c_bus,
             address,
             CL32_CORE_KEYBOARD_NAME,
             CL32_CORE_BATTERY_NAME);
    return ESP_OK;
}

esp_err_t solar_os_cl32_core_detach(const char *name)
{
    if (!cl32_core.active || name == NULL ||
        strcmp(cl32_core.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    cl32_core.stop_requested = true;
    if (cl32_core.worker_task != NULL) {
        (void)xTaskNotifyGive(cl32_core.worker_task);
    }
    if (!solar_os_task_wait_done(cl32_core.worker_task,
                                 &cl32_core.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_RETURN_ON_ERROR(
        solar_os_battery_unregister_provider(CL32_CORE_BATTERY_NAME),
        TAG,
        "battery provider unregister failed");

    ESP_LOGI(TAG,
             "%s detached: %lu transitions, %lu state, %lu unsupported, "
             "%lu dropped, %lu bus errors, %lu invalid counts",
             name,
             (unsigned long)cl32_core.transitions,
             (unsigned long)cl32_core.state_events,
             (unsigned long)cl32_core.unsupported,
             (unsigned long)cl32_core.dropped,
             (unsigned long)cl32_core.bus_errors,
             (unsigned long)cl32_core.invalid_counts);
    clear_device(&cl32_core);
    return ESP_OK;
}
