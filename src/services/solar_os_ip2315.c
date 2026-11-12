#include "solar_os_ip2315.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ip2315.h"
#include "solar_os_buses.h"
#include "solar_os_charger.h"
#include "solar_os_gpio_controller.h"

typedef struct {
    bool active;
    bool led_mode;
    bool gate_unclosed;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int watch_sda;
    int watch_scl;
    solar_os_gpio_line_ref_t gate;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    ip2315_t chip;
} solar_os_ip2315_device_t;

static const char *TAG = "ip2315";
static solar_os_ip2315_device_t charger;
static solar_os_ip2315_vin_present_fn vin_present_fn;

void solar_os_ip2315_set_vin_present_fn(solar_os_ip2315_vin_present_fn fn)
{
    vin_present_fn = fn;
}

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    solar_os_ip2315_device_t *device = ctx;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, data, len);
}

static esp_err_t chip_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    solar_os_ip2315_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, reg, data, len);
}

static bool vin_present(void)
{
    return vin_present_fn != NULL && vin_present_fn();
}

typedef enum {
    IP2315_BUS_QUIET,
    IP2315_BUS_DISTURBED,
    IP2315_BUS_UNOBSERVED,
} ip2315_bus_t;

static ip2315_bus_t watch_bus(const solar_os_ip2315_device_t *device)
{
    if (device->watch_sda < 0 || device->watch_scl < 0) {
        return IP2315_BUS_UNOBSERVED;
    }
    const int64_t deadline = esp_timer_get_time() + 3000;
    bool low = false;
    while (esp_timer_get_time() < deadline) {
        if (gpio_get_level((gpio_num_t)device->watch_sda) == 0 ||
            gpio_get_level((gpio_num_t)device->watch_scl) == 0) {
            low = true;
            break;
        }
    }
    return low ? IP2315_BUS_DISTURBED : IP2315_BUS_QUIET;
}

static esp_err_t gate_write(solar_os_ip2315_device_t *device, bool open)
{
    return solar_os_gpio_line_write(&device->gate, open);
}

static esp_err_t gate_close(solar_os_ip2315_device_t *device)
{
    const esp_err_t ret = gate_write(device, false);
    if (ret == ESP_OK) {
        device->gate_unclosed = false;
    } else {
        device->gate_unclosed = true;
    }
    return ret;
}

static esp_err_t gate_recover(solar_os_ip2315_device_t *device)
{
    return device->gate_unclosed ? gate_close(device) : ESP_OK;
}

typedef enum {
    IP2315_GATE_OPENED,
    IP2315_GATE_LED_MODE,
    IP2315_GATE_ERROR,
} ip2315_gate_t;

static ip2315_gate_t gate_open(solar_os_ip2315_device_t *device, bool input_present)
{
    if (!input_present) {
        device->led_mode = false;
    }
    if (device->led_mode) {
        return IP2315_GATE_LED_MODE;
    }
    if (gate_write(device, true) != ESP_OK) {
        return IP2315_GATE_ERROR;
    }
    const ip2315_bus_t bus = watch_bus(device);
    if (bus == IP2315_BUS_QUIET || bus == IP2315_BUS_UNOBSERVED) {
        /* Unobserved: still try a short probe later; do not assume LED mode. */
        if (bus == IP2315_BUS_QUIET) {
            return IP2315_GATE_OPENED;
        }
        /* Give the charger a moment, then probe. If the bus is not driven as
         * LED pins, treat the open as success and let the I2C transaction decide. */
        vTaskDelay(pdMS_TO_TICKS(2));
        if (solar_os_bus_i2c_probe(device->i2c_bus, device->address) == ESP_OK) {
            return IP2315_GATE_OPENED;
        }
        (void)gate_close(device);
        return IP2315_GATE_ERROR;
    }
    const bool closed = gate_close(device) == ESP_OK;
    if (closed && input_present) {
        device->led_mode = true;
        return IP2315_GATE_LED_MODE;
    }
    return IP2315_GATE_ERROR;
}

static bool wait_ready(solar_os_ip2315_device_t *device)
{
    vTaskDelay(pdMS_TO_TICKS(2));
    for (int i = 0; i < 64; i++) {
        if (solar_os_bus_i2c_probe(device->i2c_bus, device->address) == ESP_OK) {
            return true;
        }
    }
    return false;
}

static esp_err_t with_chip(solar_os_ip2315_device_t *device,
                           esp_err_t (*op)(ip2315_t *chip, void *ctx),
                           void *ctx)
{
    if (gate_recover(device) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool input = vin_present();
    const ip2315_gate_t gate = gate_open(device, input);
    if (gate == IP2315_GATE_LED_MODE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (gate != IP2315_GATE_OPENED) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_FAIL;
    if (wait_ready(device)) {
        ret = op(&device->chip, ctx);
    }
    const esp_err_t closed = gate_close(device);
    if (closed != ESP_OK && ret == ESP_OK) {
        ret = closed;
    }
    return ret;
}

typedef struct {
    bool enabled;
} ip2315_enable_ctx_t;

static esp_err_t op_set_enabled(ip2315_t *chip, void *ctx)
{
    const ip2315_enable_ctx_t *args = ctx;
    return ip2315_set_charge_enabled(chip, args->enabled);
}

static esp_err_t charger_read_status(void *ctx, solar_os_charger_status_t *status)
{
    solar_os_ip2315_device_t *device = ctx;
    if (device == NULL || !device->active || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(status, 0, sizeof(*status));
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (gate_recover(device) != ESP_OK) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const bool input = vin_present();
    if (!input) {
        device->led_mode = false;
        status->state = SOLAR_OS_CHARGER_STATE_NOT_CHARGING;
        status->input_present = false;
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }
    status->input_present = true;
    const ip2315_gate_t gate = gate_open(device, true);
    if (gate == IP2315_GATE_LED_MODE) {
        status->state = SOLAR_OS_CHARGER_STATE_UNKNOWN;
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }
    if (gate != IP2315_GATE_OPENED) {
        xSemaphoreGive(device->mutex);
        return ESP_FAIL;
    }
    uint8_t stat = 0U;
    esp_err_t ret = ESP_FAIL;
    if (wait_ready(device)) {
        ret = ip2315_read_charge_stat(&device->chip, &stat);
    }
    const esp_err_t closed = gate_close(device);
    xSemaphoreGive(device->mutex);
    if (ret != ESP_OK) {
        return ret;
    }
    if (closed != ESP_OK) {
        return closed;
    }
    /* Only bit 7 is documented as charging-in-progress. */
    if ((stat & IP2315_CHG_STAT_CHARGING) != 0U) {
        status->state = SOLAR_OS_CHARGER_STATE_FAST_CHARGE;
        status->enabled = true;
    } else {
        status->state = SOLAR_OS_CHARGER_STATE_NOT_CHARGING;
    }
    return ESP_OK;
}

static esp_err_t charger_set_enabled(void *ctx, bool enabled)
{
    solar_os_ip2315_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    ip2315_enable_ctx_t args = {.enabled = enabled};
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = with_chip(device, op_set_enabled, &args);
    xSemaphoreGive(device->mutex);
    return ret;
}

/* Charge current and voltage are fixed by the board's ICHGSET/VSET
 * resistors, and the IP2315 exposes no documented setters. */
static esp_err_t charger_set_input_current_limit(void *ctx, uint16_t current_ma)
{
    (void)ctx;
    (void)current_ma;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t charger_set_charge_current(void *ctx, uint16_t current_ma)
{
    (void)ctx;
    (void)current_ma;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t charger_set_charge_voltage(void *ctx, uint16_t voltage_mv)
{
    (void)ctx;
    (void)voltage_mv;
    return ESP_ERR_NOT_SUPPORTED;
}

static const solar_os_charger_ops_t charger_ops = {
    .read_status = charger_read_status,
    .set_enabled = charger_set_enabled,
    .set_input_current_limit = charger_set_input_current_limit,
    .set_charge_current = charger_set_charge_current,
    .set_charge_voltage = charger_set_charge_voltage,
};

esp_err_t solar_os_ip2315_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || charger.active) {
        return ESP_ERR_INVALID_ARG;
    }
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    solar_os_gpio_line_ref_t gate = {0};
    int watch_sda = -1;
    int watch_scl = -1;
    bool have_i2c = false;
    bool have_address = false;
    bool have_gate = false;
    for (size_t i = 0; bindings != NULL && i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, sizeof(i2c_bus));
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address) {
            address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE &&
                   strcmp(binding->role, "gate") == 0 && !have_gate) {
            strlcpy(gate.controller, binding->target, sizeof(gate.controller));
            gate.line = (uint8_t)binding->value;
            have_gate = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(binding->role, "watch_sda") == 0) {
            watch_sda = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(binding->role, "watch_scl") == 0) {
            watch_scl = binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    solar_os_expansion_i2c_bus_t bus;
    if (!have_i2c || !have_address || !have_gate ||
        !solar_os_expansion_find_i2c_bus(i2c_bus, &bus, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (watch_sda < 0) {
        watch_sda = bus.sda_pin;
    }
    if (watch_scl < 0) {
        watch_scl = bus.scl_pin;
    }

    memset(&charger, 0, sizeof(charger));
    charger.address = address;
    charger.gate = gate;
    charger.watch_sda = watch_sda;
    charger.watch_scl = watch_scl;
    strlcpy(charger.name, name, sizeof(charger.name));
    strlcpy(charger.i2c_bus, i2c_bus, sizeof(charger.i2c_bus));
    charger.mutex = xSemaphoreCreateMutexStatic(&charger.mutex_storage);
    if (charger.mutex == NULL) {
        memset(&charger, 0, sizeof(charger));
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(solar_os_gpio_line_configure(&charger.gate,
                                                     SOLAR_OS_GPIO_LINE_MODE_OUTPUT,
                                                     SOLAR_OS_GPIO_LINE_PULL_NONE),
                        TAG, "gate configure failed");
    ESP_RETURN_ON_ERROR(solar_os_gpio_line_write(&charger.gate, false),
                        TAG, "gate close failed");
    const ip2315_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = &charger,
    };
    const esp_err_t init_ret = ip2315_init(&charger.chip, &io);
    if (init_ret != ESP_OK) {
        memset(&charger, 0, sizeof(charger));
        return init_ret;
    }
    charger.active = true;
    const solar_os_charger_registration_t registration = {
        .name = name,
        .driver = "ip2315",
        /* The charger service requires valid ranges; the setters above
         * reject every value, so a zero-width range is advertised. */
        .input_current_limit_ma = {0U, 0U, 1U},
        .charge_current_ma = {0U, 0U, 1U},
        .charge_voltage_mv = {0U, 0U, 1U},
        .ops = &charger_ops,
        .ctx = &charger,
    };
    const esp_err_t ret = solar_os_charger_register(&registration);
    if (ret != ESP_OK) {
        (void)ip2315_deinit(&charger.chip);
        memset(&charger, 0, sizeof(charger));
        return ret;
    }
    ESP_LOGI(TAG, "%s attached on %s address 0x%02x (gated I2C)", name, i2c_bus, address);
    return ESP_OK;
}

esp_err_t solar_os_ip2315_detach(const char *name)
{
    if (!charger.active || name == NULL || strcmp(charger.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_charger_unregister(name), TAG, "unregister failed");
    (void)solar_os_gpio_line_write(&charger.gate, false);
    (void)ip2315_deinit(&charger.chip);
    memset(&charger, 0, sizeof(charger));
    return ESP_OK;
}
