#include "solar_os_st25r3916.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_nfc.h"
#include "st25r3916.h"

#define ST25R3916_DEVICE_MAX 2U
#define ST25R3916_SPI_MODE 1U
#define ST25R3916_SPI_SPEED_HZ 4000000U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int irq_pin;
    bool power_control;
    bool powered;
    bool chip_initialized;
    solar_os_gpio_line_ref_t power_line;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    st25r3916_t chip;
} st25r3916_device_t;

static const char *TAG = "st25r3916";
static st25r3916_device_t devices[ST25R3916_DEVICE_MAX];

static esp_err_t chip_transfer(void *ctx,
                               const uint8_t *tx,
                               uint8_t *rx,
                               size_t len)
{
    st25r3916_device_t *device = ctx;
    return solar_os_bus_spi_transfer(device->spi_bus,
                                     device->cs_pin,
                                     ST25R3916_SPI_MODE,
                                     ST25R3916_SPI_SPEED_HZ,
                                     tx,
                                     rx,
                                     len);
}
static void chip_delay_ms(void *ctx, uint32_t delay_ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static int64_t chip_time_ms(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time() / 1000LL;
}

static bool chip_irq_asserted(void *ctx)
{
    const st25r3916_device_t *device = ctx;
    return gpio_get_level((gpio_num_t)device->irq_pin) != 0;
}

static esp_err_t init_chip(st25r3916_device_t *device)
{
    const st25r3916_io_t io = {
        .transfer = chip_transfer,
        .delay_ms = chip_delay_ms,
        .time_ms = chip_time_ms,
        .irq_asserted = chip_irq_asserted,
        .ctx = device,
    };
    const esp_err_t ret = st25r3916_init(&device->chip, &io);
    device->chip_initialized = ret == ESP_OK;
    return ret;
}

static esp_err_t scan(void *ctx,
                      uint32_t timeout_ms,
                      solar_os_nfc_tag_t *tag)
{
    st25r3916_device_t *device = ctx;
    if (device == NULL || !device->active || tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (!device->powered || !device->chip_initialized) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    st25r3916_nfca_tag_t raw_tag;
    const esp_err_t ret = st25r3916_nfca_scan(&device->chip,
                                              timeout_ms,
                                              &raw_tag);
    if (ret != ESP_OK) {
        xSemaphoreGive(device->mutex);
        ESP_LOGE(TAG, "NFC-A scan failed: %s", esp_err_to_name(ret));
        return ret;
    }
    memset(tag, 0, sizeof(*tag));
    tag->technology = SOLAR_OS_NFC_TECHNOLOGY_NFCA;
    tag->uid_len = raw_tag.uid_len;
    memcpy(tag->uid, raw_tag.uid, raw_tag.uid_len);
    memcpy(tag->atqa, raw_tag.atqa, sizeof(tag->atqa));
    tag->sak = raw_tag.sak;
    xSemaphoreGive(device->mutex);
    return ESP_OK;
}

static esp_err_t set_power(void *ctx, bool enabled)
{
    st25r3916_device_t *device = ctx;
    if (device == NULL || !device->active || !device->power_control) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (device->powered == enabled) {
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (enabled) {
        ret = solar_os_gpio_line_write(&device->power_line, true);
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10U));
            ret = init_chip(device);
        }
        if (ret != ESP_OK) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
            device->chip_initialized = false;
        }
    } else {
        if (device->chip_initialized) {
            const esp_err_t deinit_ret = st25r3916_deinit(&device->chip);
            if (deinit_ret != ESP_OK) {
                ESP_LOGW(TAG, "chip shutdown failed: %s", esp_err_to_name(deinit_ret));
            }
        }
        ret = solar_os_gpio_line_write(&device->power_line, false);
        if (ret == ESP_OK) {
            device->chip_initialized = false;
        }
    }
    if (ret == ESP_OK) {
        device->powered = enabled;
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_nfc_ops_t nfc_ops = {
    .scan = scan,
};

static const solar_os_nfc_ops_t powered_nfc_ops = {
    .scan = scan,
    .set_power = set_power,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin,
                                int *irq_pin,
                                bool *power_control,
                                solar_os_gpio_line_ref_t *power_line)
{
    bool have_spi = false;
    bool have_cs = false;
    bool have_irq = false;
    *power_control = false;
    if (bindings == NULL || spi_bus == NULL || cs_pin == NULL || irq_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS && !have_spi) {
            strlcpy(spi_bus, binding->target, spi_bus_len);
            have_spi = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS && !have_cs) {
            *cs_pin = binding->value;
            have_cs = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(binding->role, "irq") == 0 && !have_irq) {
            *irq_pin = binding->value;
            have_irq = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE &&
                   strcmp(binding->role, "power") == 0 && !*power_control) {
            strlcpy(power_line->controller,
                    binding->target,
                    sizeof(power_line->controller));
            power_line->line = (uint8_t)binding->value;
            *power_control = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_spi && have_cs && have_irq &&
        solar_os_expansion_find_spi_bus(spi_bus, NULL, NULL) &&
        solar_os_expansion_spi_cs_allowed(spi_bus, *cs_pin)
        ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_st25r3916_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    st25r3916_device_t *device = NULL;
    for (size_t i = 0; i < ST25R3916_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) {
            device = &devices[i];
        }
    }
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    int cs_pin = -1;
    int irq_pin = -1;
    bool power_control = false;
    solar_os_gpio_line_ref_t power_line = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count,
                                       spi_bus, sizeof(spi_bus),
                                       &cs_pin, &irq_pin,
                                       &power_control, &power_line),
                        TAG,
                        "invalid bindings");
    const gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), TAG, "IRQ setup failed");

    memset(device, 0, sizeof(*device));
    device->active = true;
    device->cs_pin = cs_pin;
    device->irq_pin = irq_pin;
    device->power_control = power_control;
    device->power_line = power_line;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    if (power_control) {
        ret = solar_os_gpio_line_write(&device->power_line, false);
    } else {
        ret = init_chip(device);
        device->powered = ret == ESP_OK;
    }
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    const solar_os_nfc_registration_t registration = {
        .name = name,
        .driver = "st25r3916",
        .ops = power_control ? &powered_nfc_ops : &nfc_ops,
        .ctx = device,
        .powered = device->powered,
    };
    ret = solar_os_nfc_register(&registration);
    if (ret != ESP_OK) {
        (void)st25r3916_deinit(&device->chip);
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ESP_LOGI(TAG,
             "%s attached on %s cs=%d irq=%d power=%s identity=0x%02x",
             name,
             spi_bus,
             cs_pin,
             irq_pin,
             power_control ? "off" : "always-on",
             device->chip.identity);
    return ESP_OK;
}

esp_err_t solar_os_st25r3916_detach(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < ST25R3916_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            ESP_RETURN_ON_ERROR(solar_os_nfc_unregister(name), TAG, "unregister failed");
            esp_err_t ret = ESP_OK;
            if (devices[i].chip_initialized) {
                ret = st25r3916_deinit(&devices[i].chip);
            }
            if (devices[i].power_control) {
                const esp_err_t power_ret =
                    solar_os_gpio_line_write(&devices[i].power_line, false);
                if (ret == ESP_OK) {
                    ret = power_ret;
                }
            }
            memset(&devices[i], 0, sizeof(devices[i]));
            return ret;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
