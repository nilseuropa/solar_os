#include "solar_os_st25r3916.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
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

static esp_err_t scan(void *ctx,
                      uint32_t timeout_ms,
                      solar_os_nfc_tag_t *tag)
{
    st25r3916_device_t *device = ctx;
    if (device == NULL || !device->active || tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    st25r3916_nfca_tag_t raw_tag;
    ESP_RETURN_ON_ERROR(st25r3916_nfca_scan(&device->chip,
                                            timeout_ms,
                                            &raw_tag),
                        TAG,
                        "NFC-A scan failed");
    memset(tag, 0, sizeof(*tag));
    tag->technology = SOLAR_OS_NFC_TECHNOLOGY_NFCA;
    tag->uid_len = raw_tag.uid_len;
    memcpy(tag->uid, raw_tag.uid, raw_tag.uid_len);
    memcpy(tag->atqa, raw_tag.atqa, sizeof(tag->atqa));
    tag->sak = raw_tag.sak;
    return ESP_OK;
}

static const solar_os_nfc_ops_t nfc_ops = {
    .scan = scan,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin,
                                int *irq_pin)
{
    bool have_spi = false;
    bool have_cs = false;
    bool have_irq = false;
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
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count,
                                       spi_bus, sizeof(spi_bus),
                                       &cs_pin, &irq_pin),
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
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));
    const st25r3916_io_t io = {
        .transfer = chip_transfer,
        .delay_ms = chip_delay_ms,
        .time_ms = chip_time_ms,
        .irq_asserted = chip_irq_asserted,
        .ctx = device,
    };
    esp_err_t ret = st25r3916_init(&device->chip, &io);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    const solar_os_nfc_registration_t registration = {
        .name = name,
        .driver = "st25r3916",
        .ops = &nfc_ops,
        .ctx = device,
    };
    ret = solar_os_nfc_register(&registration);
    if (ret != ESP_OK) {
        (void)st25r3916_deinit(&device->chip);
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ESP_LOGI(TAG, "%s attached on %s cs=%d irq=%d identity=0x%02x",
             name, spi_bus, cs_pin, irq_pin, device->chip.identity);
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
            const esp_err_t ret = st25r3916_deinit(&devices[i].chip);
            memset(&devices[i], 0, sizeof(devices[i]));
            return ret;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
