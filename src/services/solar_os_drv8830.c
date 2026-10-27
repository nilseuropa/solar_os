#include "solar_os_drv8830.h"

#include <string.h>
#include <math.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"
#include "solar_os_motor.h"

typedef struct device {
    struct device *next;
    solar_os_drv8830_info_t info;
    solar_os_drv8830_chip_t chip;
    uint8_t max_code;
} device_t;

static esp_err_t motor_status(void *ctx, solar_os_motor_status_t *status);
static esp_err_t motor_drive(void *ctx, float value);
static esp_err_t motor_coast(void *ctx);
static esp_err_t motor_brake(void *ctx);
static esp_err_t motor_clear(void *ctx);
static esp_err_t motor_shutdown(void *ctx);
static const solar_os_motor_ops_t motor_ops = {
    .status = motor_status, .drive = motor_drive, .coast = motor_coast,
    .brake = motor_brake, .clear = motor_clear, .shutdown = motor_shutdown,
};

static device_t *devices;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

/* Serialize transfers with detach so no callback can use released state. */
static void lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static device_t *find(const char *name)
{
    for (device_t *d = devices; d != NULL; d = d->next) {
        if (name != NULL && strcmp(name, d->info.name) == 0) return d;
    }
    return NULL;
}

static bool speed_supported(const device_t *d)
{
    solar_os_bus_info_t info;
    return solar_os_bus_find(d->info.bus, SOLAR_OS_BUS_PROTOCOL_I2C, &info) &&
        info.config.i2c.speed_hz > 0U && info.config.i2c.speed_hz <= 400000U;
}

static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    const device_t *d = ctx;
    if (!speed_supported(d)) return ESP_ERR_INVALID_STATE;
    return solar_os_bus_i2c_read_reg(d->info.bus, d->info.address, reg, data, len);
}

static esp_err_t write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    const device_t *d = ctx;
    if (!speed_supported(d)) return ESP_ERR_INVALID_STATE;
    return solar_os_bus_i2c_write_reg(d->info.bus, d->info.address, reg, data, len);
}

static esp_err_t parse(device_t *d, const solar_os_expansion_binding_t *bindings, size_t count)
{
    bool bus = false, address = false;
    bool have_max = false;
    const char *address_bus = NULL;
    d->info.interrupt_gpio = -1;
    d->max_code = 63U;
    if (bindings == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !bus && b->target[0] != '\0') {
            if (strlcpy(d->info.bus, b->target, sizeof(d->info.bus)) >= sizeof(d->info.bus)) return ESP_ERR_INVALID_ARG;
            bus = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS && !address && b->value >= 0x60 && b->value <= 0x68) {
            d->info.address = (uint8_t)b->value;
            address_bus = b->target;
            address = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && strcmp(b->role, "int") == 0 &&
                   d->info.interrupt_gpio < 0 && GPIO_IS_VALID_GPIO(b->value)) {
            d->info.interrupt_gpio = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && strcmp(b->role, "max_mv") == 0 &&
                   !have_max && b->value >= 482 && b->value <= 5060) {
            d->max_code = (uint8_t)((unsigned)b->value * 64U / 5140U);
            have_max = true;
        } else return ESP_ERR_INVALID_ARG;
    }
    return bus && address && (address_bus[0] == '\0' || strcmp(address_bus, d->info.bus) == 0) &&
        speed_supported(d) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_drv8830_attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (name == NULL || name[0] == '\0' || strnlen(name, SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t ret = find(name) == NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    device_t *d = NULL;
    if (ret == ESP_OK) {
        d = solar_os_memory_calloc(1U, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "drv8830.device");
        if (d == NULL) ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK) ret = parse(d, bindings, count);
    bool configured = false;
    if (ret == ESP_OK && d->info.interrupt_gpio >= 0) {
        const solar_os_gpio_line_ref_t line = {.line = (uint8_t)d->info.interrupt_gpio};
        configured = true;
        ret = solar_os_gpio_line_configure(&line, SOLAR_OS_GPIO_LINE_MODE_INPUT, SOLAR_OS_GPIO_LINE_PULL_UP);
    }
    if (ret == ESP_OK) ret = solar_os_bus_i2c_probe(d->info.bus, d->info.address);
    if (ret == ESP_OK) {
        const solar_os_drv8830_io_t io = {.read = read_reg, .write = write_reg, .ctx = d};
        ret = solar_os_drv8830_chip_init(&d->chip, &io);
    }
    if (ret == ESP_OK) {
        strlcpy(d->info.name, name, sizeof(d->info.name));
        const solar_os_motor_registration_t registration = {
            .name = name, .driver = "drv8830", .ops = &motor_ops, .ctx = d,
        };
        ret = solar_os_motor_register(&registration);
    }
    if (ret == ESP_OK) {
        d->next = devices;
        devices = d;
    } else if (d != NULL) {
        if (d->chip.initialized) (void)solar_os_drv8830_chip_stop(&d->chip, false);
        if (configured) (void)gpio_reset_pin((gpio_num_t)d->info.interrupt_gpio);
        solar_os_memory_free(d);
    }
    xSemaphoreGive(mutex);
    return ret;
}

esp_err_t solar_os_drv8830_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (device_t **link = &devices; *link != NULL; link = &(*link)->next) {
        device_t *d = *link;
        if (strcmp(d->info.name, name) != 0) continue;
        ret = solar_os_motor_unregister(name);
        if (ret == ESP_OK) { *link = d->next; solar_os_memory_free(d); }
        break;
    }
    xSemaphoreGive(mutex);
    return ret;
}

bool solar_os_drv8830_get(size_t index, solar_os_drv8830_info_t *info)
{
    if (info == NULL) return false;
    lock();
    device_t *d = devices;
    while (d != NULL && index > 0U) { d = d->next; index--; }
    if (d != NULL) *info = d->info;
    const bool found = d != NULL;
    xSemaphoreGive(mutex);
    return found;
}

static esp_err_t fault_line(device_t *d, bool *asserted)
{
    *asserted = false;
    if (d->info.interrupt_gpio < 0) return ESP_OK;
    bool level;
    const solar_os_gpio_line_ref_t line = {.line = (uint8_t)d->info.interrupt_gpio};
    const esp_err_t ret = solar_os_gpio_line_read(&line, &level);
    if (ret == ESP_OK) *asserted = !level;
    return ret;
}

static uint32_t motor_faults(uint8_t fault)
{
    uint32_t result = 0;
    if (fault & SOLAR_OS_DRV8830_OCP) result |= SOLAR_OS_MOTOR_FAULT_OVERCURRENT;
    if (fault & SOLAR_OS_DRV8830_UVLO) result |= SOLAR_OS_MOTOR_FAULT_UNDERVOLTAGE;
    if (fault & SOLAR_OS_DRV8830_OTS) result |= SOLAR_OS_MOTOR_FAULT_OVERTEMPERATURE;
    if (fault & SOLAR_OS_DRV8830_ILIMIT) result |= SOLAR_OS_MOTOR_FAULT_CURRENT_LIMIT;
    if ((fault & SOLAR_OS_DRV8830_FAULT) && result == 0U) result = SOLAR_OS_MOTOR_FAULT_UNKNOWN;
    return result;
}

static esp_err_t motor_status(void *ctx, solar_os_motor_status_t *status)
{
    device_t *d = ctx;
    solar_os_drv8830_chip_status_t chip;
    esp_err_t ret = solar_os_drv8830_chip_status(&d->chip, &chip);
    bool asserted = false;
    if (ret == ESP_OK) ret = fault_line(d, &asserted);
    if (ret == ESP_OK) {
        *status = (solar_os_motor_status_t){
            .voltage_valid = true, .voltage_mv = chip.voltage_mv,
            .faults = motor_faults(chip.fault),
            .fault_input_present = d->info.interrupt_gpio >= 0, .fault_input_asserted = asserted,
            .raw_valid = true, .raw_control = chip.control, .raw_fault = chip.fault,
            .drive_valid = true,
        };
        switch (chip.mode) {
        case SOLAR_OS_DRV8830_COAST: status->mode = SOLAR_OS_MOTOR_MODE_COAST; break;
        case SOLAR_OS_DRV8830_FORWARD: status->mode = SOLAR_OS_MOTOR_MODE_FORWARD; break;
        case SOLAR_OS_DRV8830_REVERSE: status->mode = SOLAR_OS_MOTOR_MODE_REVERSE; break;
        case SOLAR_OS_DRV8830_BRAKE: status->mode = SOLAR_OS_MOTOR_MODE_BRAKE; break;
        }
        if (chip.mode == SOLAR_OS_DRV8830_FORWARD || chip.mode == SOLAR_OS_DRV8830_REVERSE) {
            status->drive = (float)(chip.control >> 2U) / d->max_code;
            if (chip.mode == SOLAR_OS_DRV8830_REVERSE) status->drive = -status->drive;
        }
        status->blocking_faults = motor_faults(chip.fault & (SOLAR_OS_DRV8830_OCP | SOLAR_OS_DRV8830_ILIMIT));
        if ((chip.fault & SOLAR_OS_DRV8830_FAULT) || asserted) {
            if (status->faults == 0U) status->faults = SOLAR_OS_MOTOR_FAULT_UNKNOWN;
            status->blocking_faults |= status->faults;
        }
    }
    return ret;
}

static esp_err_t motor_drive(void *ctx, float value)
{
    device_t *d = ctx;
    const unsigned code = (unsigned)(fabsf(value) * d->max_code + 0.5f);
    if (code < 6U) return solar_os_drv8830_chip_stop(&d->chip, false);
    bool asserted = false;
    esp_err_t ret = fault_line(d, &asserted);
    if (ret == ESP_OK && asserted) ret = ESP_ERR_INVALID_STATE;
    const int voltage_mv = (int)((code * 5140U + 32U) / 64U);
    if (ret == ESP_OK) ret = solar_os_drv8830_chip_drive(&d->chip, value < 0.0f ? -voltage_mv : voltage_mv);
    return ret;
}

static esp_err_t motor_coast(void *ctx) { return solar_os_drv8830_chip_stop(&((device_t *)ctx)->chip, false); }
static esp_err_t motor_brake(void *ctx) { return solar_os_drv8830_chip_stop(&((device_t *)ctx)->chip, true); }
static esp_err_t motor_clear(void *ctx) { return solar_os_drv8830_chip_clear(&((device_t *)ctx)->chip); }
static esp_err_t motor_shutdown(void *ctx)
{
    device_t *d = ctx;
    esp_err_t ret = motor_coast(d);
    if (ret == ESP_OK && d->info.interrupt_gpio >= 0) ret = gpio_reset_pin((gpio_num_t)d->info.interrupt_gpio);
    return ret;
}

/* Compatibility helpers for driver diagnostics. Shared callers use motor. */
esp_err_t solar_os_drv8830_status(const char *name, solar_os_drv8830_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    solar_os_motor_status_t value;
    const esp_err_t ret = solar_os_motor_status(name, &value);
    if (ret == ESP_OK) *status = (solar_os_drv8830_status_t){
        .chip = {.control = (uint8_t)value.raw_control, .fault = (uint8_t)value.raw_fault,
            .mode = (solar_os_drv8830_mode_t)(value.raw_control & 3U), .voltage_mv = (uint16_t)value.voltage_mv},
        .interrupt_present = value.fault_input_present, .fault_asserted = value.fault_input_asserted,
    };
    return ret;
}

esp_err_t solar_os_drv8830_drive(const char *name, int voltage_mv)
{
    if (voltage_mv == 0) return solar_os_motor_coast(name);
    if (voltage_mv < -5060 || voltage_mv > 5060 || (voltage_mv > -480 && voltage_mv < 480)) return ESP_ERR_INVALID_ARG;
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    device_t *d = find(name);
    const unsigned maximum = d != NULL ? d->max_code : 0U;
    xSemaphoreGive(mutex);
    if (maximum == 0U) return ESP_ERR_NOT_FOUND;
    const unsigned magnitude = (unsigned)(voltage_mv < 0 ? -voltage_mv : voltage_mv);
    const unsigned code = (magnitude * 64U + 2570U) / 5140U;
    return solar_os_motor_drive(name, (voltage_mv < 0 ? -1.0f : 1.0f) * (float)code / maximum);
}

esp_err_t solar_os_drv8830_stop(const char *name, bool brake)
{ return brake ? solar_os_motor_brake(name) : solar_os_motor_coast(name); }
esp_err_t solar_os_drv8830_clear(const char *name) { return solar_os_motor_clear(name); }
