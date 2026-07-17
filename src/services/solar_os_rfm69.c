#include "solar_os_rfm69.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rfm69.h"
#include "solar_os_radio.h"
#include "solar_os_spi.h"

#define SOLAR_OS_RFM69_MAX 2
#define RFM69_DEFAULT_SPEED_HZ 1000000U
#define RFM69_DEFAULT_FREQUENCY_HZ 433000000U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int irq_pin;
    int reset_pin;
    rfm69_t radio;
} solar_os_rfm69_device_t;

static const char *TAG = "rfm69";
static solar_os_rfm69_device_t devices[SOLAR_OS_RFM69_MAX];

static const solar_os_radio_ops_t radio_ops;

static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static solar_os_rfm69_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_RFM69_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_rfm69_device_t *alloc_device(void)
{
    for (size_t i = 0; i < SOLAR_OS_RFM69_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_radio_config_t default_config(void)
{
    solar_os_radio_config_t config = {
        .frequency_hz = RFM69_DEFAULT_FREQUENCY_HZ,
        .modulation = SOLAR_OS_RADIO_MODULATION_GFSK,
        .bitrate_bps = 4800,
        .deviation_hz = 5000,
        .rx_bandwidth_hz = 10500,
        .preamble_len = 3,
        .sync_word_len = 2,
        .sync_word = {0x2D, 0xD4},
        .tx_power_dbm = 13,
        .crc_enabled = true,
        .variable_length = true,
        .payload_length = RFM69_MAX_PACKET_LEN,
    };
    return config;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin,
                                int *irq_pin,
                                int *reset_pin)
{
    bool have_spi = false;
    bool have_cs = false;

    if (bindings == NULL || spi_bus == NULL || cs_pin == NULL || irq_pin == NULL || reset_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_bus[0] = '\0';
    *cs_pin = -1;
    *irq_pin = -1;
    *reset_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(spi_bus, binding->target, spi_bus_len);
            have_spi = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            *cs_pin = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(spi_bus, binding->target, spi_bus_len);
                have_spi = true;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (binding_role_is(binding, "irq")) {
                if (*irq_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                *irq_pin = binding->value;
            } else if (binding_role_is(binding, "reset")) {
                if (*reset_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                *reset_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_spi || !have_cs || !solar_os_expansion_find_spi_bus(spi_bus, NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_expansion_spi_cs_allowed(spi_bus, *cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t configure_optional_gpio(int irq_pin, int reset_pin)
{
    if (irq_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "irq gpio config failed");
    }

    if (reset_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (uint32_t)reset_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "reset gpio config failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(reset_pin, 0), TAG, "reset low failed");
        vTaskDelay(pdMS_TO_TICKS(1));
        ESP_RETURN_ON_ERROR(gpio_set_level(reset_pin, 1), TAG, "reset high failed");
        vTaskDelay(pdMS_TO_TICKS(2));
        ESP_RETURN_ON_ERROR(gpio_set_level(reset_pin, 0), TAG, "reset release failed");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

static void clear_device(solar_os_rfm69_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->radio.mutex != NULL) {
        vSemaphoreDelete(device->radio.mutex);
    }
    memset(device, 0, sizeof(*device));
    device->cs_pin = -1;
    device->irq_pin = -1;
    device->reset_pin = -1;
}

static esp_err_t op_configure(void *ctx, const solar_os_radio_config_t *config)
{
    return rfm69_configure((rfm69_t *)ctx, config);
}

static esp_err_t op_set_state(void *ctx, solar_os_radio_state_t state)
{
    return rfm69_set_state((rfm69_t *)ctx, state);
}

static esp_err_t op_get_status(void *ctx, solar_os_radio_status_t *status)
{
    return rfm69_get_status((rfm69_t *)ctx, status);
}

static esp_err_t op_send(void *ctx, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return rfm69_send((rfm69_t *)ctx, packet, timeout_ms);
}

static esp_err_t op_receive(void *ctx, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return rfm69_receive((rfm69_t *)ctx, packet, timeout_ms);
}

static const solar_os_radio_ops_t radio_ops = {
    .configure = op_configure,
    .set_state = op_set_state,
    .get_status = op_get_status,
    .send = op_send,
    .receive = op_receive,
};

esp_err_t solar_os_rfm69_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    int cs_pin = -1;
    int irq_pin = -1;
    int reset_pin = -1;

    if (name == NULL || name[0] == '\0' || find_device(name) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       spi_bus,
                                       sizeof(spi_bus),
                                       &cs_pin,
                                       &irq_pin,
                                       &reset_pin),
                        TAG,
                        "invalid bindings");

    solar_os_rfm69_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    clear_device(device);
    device->active = true;
    device->cs_pin = cs_pin;
    device->irq_pin = irq_pin;
    device->reset_pin = reset_pin;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));

    esp_err_t ret = configure_optional_gpio(irq_pin, reset_pin);
    if (ret == ESP_OK) {
        ret = rfm69_init(&device->radio, cs_pin, RFM69_DEFAULT_SPEED_HZ);
    }
    uint8_t version = 0;
    if (ret == ESP_OK) {
        ret = rfm69_probe(&device->radio, &version);
    }
    solar_os_radio_config_t config = default_config();
    if (ret == ESP_OK) {
        ret = rfm69_configure(&device->radio, &config);
    }
    if (ret == ESP_OK) {
        const solar_os_radio_registration_t registration = {
            .name = name,
            .driver = "rfm69",
            .summary = "HopeRF RFM69 433 MHz SPI radio",
            .modulations = SOLAR_OS_RADIO_MODULATION_FSK |
                SOLAR_OS_RADIO_MODULATION_GFSK |
                SOLAR_OS_RADIO_MODULATION_MSK |
                SOLAR_OS_RADIO_MODULATION_GMSK |
                SOLAR_OS_RADIO_MODULATION_OOK,
            .features = SOLAR_OS_RADIO_FEATURE_PACKET |
                SOLAR_OS_RADIO_FEATURE_RSSI |
                SOLAR_OS_RADIO_FEATURE_TX_POWER |
                SOLAR_OS_RADIO_FEATURE_CRC |
                SOLAR_OS_RADIO_FEATURE_SYNC_WORD |
                SOLAR_OS_RADIO_FEATURE_PREAMBLE |
                SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH |
                SOLAR_OS_RADIO_FEATURE_ADDRESSING |
                SOLAR_OS_RADIO_FEATURE_CONTINUOUS_RX,
            .max_packet_len = RFM69_MAX_PACKET_LEN,
            .default_config = config,
            .initial_state = SOLAR_OS_RADIO_STATE_STANDBY,
            .ops = &radio_ops,
            .ctx = &device->radio,
        };
        ret = solar_os_radio_register(&registration);
    }

    if (ret != ESP_OK) {
        clear_device(device);
        if (ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGW(TAG, "%s probe failed: version 0x%02x", name, version);
        }
        return ret;
    }

    ESP_LOGI(TAG,
             "%s attached on %s CS GPIO%d%s%s",
             name,
             spi_bus,
             cs_pin,
             irq_pin >= 0 ? " irq" : "",
             reset_pin >= 0 ? " reset" : "");
    return ESP_OK;
}

esp_err_t solar_os_rfm69_detach(const char *name)
{
    solar_os_rfm69_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    (void)solar_os_radio_unregister(name);
    (void)rfm69_set_state(&device->radio, SOLAR_OS_RADIO_STATE_SLEEP);
    clear_device(device);
    return ESP_OK;
}
