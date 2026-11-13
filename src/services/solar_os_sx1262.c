#include "solar_os_sx1262.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sx1262.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_radio.h"

#define SOLAR_OS_SX1262_MAX 1
#define SX1262_DEFAULT_SPEED_HZ 2000000U
#define SX1262_DEFAULT_FREQUENCY_HZ 915000000U /* T-LoRa-Pager SX1262 SKU is 915 MHz */

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int busy_pin;
    int reset_pin;
    int irq_pin;
    bool has_reset_line;
    bool has_power_line;
    bool has_ant_line;
    solar_os_gpio_line_ref_t reset_line;
    solar_os_gpio_line_ref_t power_line;
    solar_os_gpio_line_ref_t ant_line;
    uint32_t frequency_hz;
    bool regulator_ldo;
    sx1262_t radio;
} solar_os_sx1262_device_t;

static const char *TAG = "sx1262";
static solar_os_sx1262_device_t devices[SOLAR_OS_SX1262_MAX];
static const solar_os_radio_ops_t radio_ops;

typedef struct {
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int busy_pin;
    int reset_pin;
    int irq_pin;
    bool has_reset_line;
    bool has_power_line;
    bool has_ant_line;
    solar_os_gpio_line_ref_t reset_line;
    solar_os_gpio_line_ref_t power_line;
    solar_os_gpio_line_ref_t ant_line;
    uint32_t frequency_hz;
    bool regulator_ldo;
} sx1262_binding_config_t;

static void copy_line(solar_os_gpio_line_ref_t *line, const solar_os_expansion_binding_t *binding)
{
    strlcpy(line->controller, binding->target, sizeof(line->controller));
    line->line = (uint8_t)binding->value;
}

static esp_err_t set_antenna_tx(void *ctx, bool tx)
{
    solar_os_sx1262_device_t *device = ctx;
    return solar_os_gpio_line_write(&device->ant_line, tx);
}


static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static solar_os_sx1262_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_SX1262_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_sx1262_device_t *alloc_device(void)
{
    for (size_t i = 0; i < SOLAR_OS_SX1262_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_radio_config_t default_config(void)
{
    return (solar_os_radio_config_t) {
        .frequency_hz = SX1262_DEFAULT_FREQUENCY_HZ,
        .modulation = SOLAR_OS_RADIO_MODULATION_LORA,
        .rx_bandwidth_hz = 125000,
        .spreading_factor = 7,
        .coding_rate_denominator = 5,
        .preamble_len = 8,
        .sync_word_len = 1,
        .sync_word = {0x12},
        .tx_power_dbm = 14,
        .rx_gain = SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING,
        .crc_enabled = true,
        .variable_length = true,
        .payload_length = SX1262_MAX_PACKET_LEN,
    };
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                sx1262_binding_config_t *config)
{
    bool have_spi = false;
    bool have_cs = false;

    if (bindings == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    config->cs_pin = -1;
    config->busy_pin = -1;
    config->reset_pin = -1;
    config->irq_pin = -1;
    config->frequency_hz = SX1262_DEFAULT_FREQUENCY_HZ;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(config->spi_bus, binding->target, sizeof(config->spi_bus));
            have_spi = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            config->cs_pin = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(config->spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(config->spi_bus, binding->target, sizeof(config->spi_bus));
                have_spi = true;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (binding_role_is(binding, "busy")) {
                if (config->busy_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                config->busy_pin = binding->value;
            } else if (binding_role_is(binding, "reset") && !config->has_reset_line) {
                if (config->reset_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                config->reset_pin = binding->value;
            } else if (binding_role_is(binding, "irq")) {
                if (config->irq_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                config->irq_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
            if (binding_role_is(binding, "reset") && !config->has_reset_line &&
                config->reset_pin < 0) {
                copy_line(&config->reset_line, binding);
                config->has_reset_line = true;
            } else if (binding_role_is(binding, "power") && !config->has_power_line) {
                copy_line(&config->power_line, binding);
                config->has_power_line = true;
            } else if (binding_role_is(binding, "ant") && !config->has_ant_line) {
                copy_line(&config->ant_line, binding);
                config->has_ant_line = true;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
            if (binding_role_is(binding, "frequency") &&
                binding->value >= 150000000 && binding->value <= 960000000) {
                config->frequency_hz = (uint32_t)binding->value;
            } else if (binding_role_is(binding, "regulator") &&
                       (binding->value == 0 || binding->value == 1)) {
                config->regulator_ldo = binding->value == 1;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_spi || !have_cs || config->busy_pin < 0 ||
        !solar_os_expansion_find_spi_bus(config->spi_bus, NULL, NULL) ||
        !solar_os_expansion_spi_cs_allowed(config->spi_bus, config->cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void clear_device(solar_os_sx1262_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->radio.mutex != NULL) {
        vSemaphoreDelete(device->radio.mutex);
    }
    memset(device, 0, sizeof(*device));
    device->cs_pin = -1;
    device->busy_pin = -1;
    device->reset_pin = -1;
    device->irq_pin = -1;
}

static esp_err_t op_configure(void *ctx, const solar_os_radio_config_t *config)
{
    return sx1262_configure((sx1262_t *)ctx, config);
}

static esp_err_t op_set_state(void *ctx, solar_os_radio_state_t state)
{
    return sx1262_set_state((sx1262_t *)ctx, state);
}

static esp_err_t op_get_status(void *ctx, solar_os_radio_status_t *status)
{
    return sx1262_get_status((sx1262_t *)ctx, status);
}

static esp_err_t op_send(void *ctx, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return sx1262_send((sx1262_t *)ctx, packet, timeout_ms);
}

static esp_err_t op_send_stream(void *ctx, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    return sx1262_send_stream((sx1262_t *)ctx, data, len, timeout_ms);
}

static esp_err_t op_receive(void *ctx, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return sx1262_receive((sx1262_t *)ctx, packet, timeout_ms);
}

static esp_err_t op_read_rssi(void *ctx, int16_t *dbm)
{
    return sx1262_read_rssi((sx1262_t *)ctx, dbm);
}

static esp_err_t op_tune_rx(void *ctx, uint32_t hz, uint32_t *settle_ms)
{
    return sx1262_tune_rx((sx1262_t *)ctx, hz, settle_ms);
}

static const solar_os_radio_ops_t radio_ops = {
    .tune_rx = op_tune_rx,
    .read_rssi = op_read_rssi,
    .configure = op_configure,
    .set_state = op_set_state,
    .get_status = op_get_status,
    .send = op_send,
    .send_stream = op_send_stream,
    .receive = op_receive,
};

esp_err_t solar_os_sx1262_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    sx1262_binding_config_t binding = {0};

    if (name == NULL || name[0] == '\0' || find_device(name) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &binding),
                        TAG, "invalid bindings");

    solar_os_sx1262_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    clear_device(device);
    device->active = true;
    device->cs_pin = binding.cs_pin;
    device->busy_pin = binding.busy_pin;
    device->reset_pin = binding.has_reset_line ? -1 : binding.reset_pin;
    device->irq_pin = binding.irq_pin;
    device->has_reset_line = binding.has_reset_line;
    device->has_power_line = binding.has_power_line;
    device->has_ant_line = binding.has_ant_line;
    device->reset_line = binding.reset_line;
    device->power_line = binding.power_line;
    device->ant_line = binding.ant_line;
    device->frequency_hz = binding.frequency_hz;
    device->regulator_ldo = binding.regulator_ldo;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, binding.spi_bus, sizeof(device->spi_bus));

    if (device->has_power_line) {
        esp_err_t line_ret = solar_os_gpio_line_configure(&device->power_line,
            SOLAR_OS_GPIO_LINE_MODE_OUTPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
        if (line_ret == ESP_OK) {
            line_ret = solar_os_gpio_line_write(&device->power_line, true);
        }
        if (line_ret != ESP_OK) {
            clear_device(device);
            return line_ret;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (device->has_reset_line) {
        esp_err_t line_ret = solar_os_gpio_line_configure(&device->reset_line,
            SOLAR_OS_GPIO_LINE_MODE_OUTPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
        if (line_ret == ESP_OK) {
            line_ret = solar_os_gpio_line_write(&device->reset_line, false);
        }
        if (line_ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            line_ret = solar_os_gpio_line_write(&device->reset_line, true);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (line_ret != ESP_OK) {
            clear_device(device);
            return line_ret;
        }
    }
    if (device->has_ant_line) {
        esp_err_t line_ret = solar_os_gpio_line_configure(&device->ant_line,
            SOLAR_OS_GPIO_LINE_MODE_OUTPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
        if (line_ret == ESP_OK) {
            line_ret = solar_os_gpio_line_write(&device->ant_line, false);
        }
        if (line_ret != ESP_OK) {
            clear_device(device);
            return line_ret;
        }
        device->radio.set_antenna_tx = set_antenna_tx;
        device->radio.antenna_ctx = device;
    }

    esp_err_t ret = sx1262_init(&device->radio, binding.spi_bus, binding.cs_pin,
                                binding.busy_pin, device->reset_pin, binding.irq_pin,
                                SX1262_DEFAULT_SPEED_HZ);
    if (ret == ESP_OK && device->has_ant_line) {
        /* sx1262_init may memset the radio; restore the antenna hook. */
        device->radio.set_antenna_tx = set_antenna_tx;
        device->radio.antenna_ctx = device;
    }
    if (ret == ESP_OK) {
        device->radio.regulator_ldo = device->regulator_ldo;
    }
        uint8_t chip_mode = 0;
    if (ret == ESP_OK) {
        ret = sx1262_probe(&device->radio, &chip_mode);
    }
    solar_os_radio_config_t config = default_config();
    config.frequency_hz = device->frequency_hz;
    if (ret == ESP_OK) {
        ret = sx1262_configure(&device->radio, &config);
    }
    if (ret == ESP_OK) {
        const solar_os_radio_registration_t registration = {
            .name = name,
            .driver = "sx1262",
            .summary = "Semtech SX1262 LoRa/(G)FSK radio",
            .modulations = SOLAR_OS_RADIO_MODULATION_LORA | SOLAR_OS_RADIO_MODULATION_GFSK,
            .features = SOLAR_OS_RADIO_FEATURE_PACKET |
                SOLAR_OS_RADIO_FEATURE_RSSI |
                SOLAR_OS_RADIO_FEATURE_LIVE_RSSI |
                SOLAR_OS_RADIO_FEATURE_FAST_TUNE |
                SOLAR_OS_RADIO_FEATURE_SNR |
                SOLAR_OS_RADIO_FEATURE_TX_POWER |
                SOLAR_OS_RADIO_FEATURE_RX_GAIN |
                SOLAR_OS_RADIO_FEATURE_CRC |
                SOLAR_OS_RADIO_FEATURE_SYNC_WORD |
                SOLAR_OS_RADIO_FEATURE_PREAMBLE |
                SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH |
                SOLAR_OS_RADIO_FEATURE_CONTINUOUS_RX,
            .max_packet_len = SX1262_MAX_PACKET_LEN,
            .rx_gain_modes = SOLAR_OS_RADIO_RX_GAIN_BOOST_MODES,
            .default_config = config,
            .initial_state = SOLAR_OS_RADIO_STATE_STANDBY,
            .ops = &radio_ops,
            .ctx = &device->radio,
        };
        ret = solar_os_radio_register(&registration);
    }

    if (ret != ESP_OK) {
        clear_device(device);
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "%s probe failed: status 0x%02x", name, chip_mode);
        }
        return ret;
    }

    ESP_LOGI(TAG,
             "%s attached on %s CS GPIO%d BUSY GPIO%d%s%s",
             name,
             device->spi_bus,
             device->cs_pin,
             device->busy_pin,
             device->irq_pin >= 0 ? " irq" : "",
             (device->reset_pin >= 0 || device->has_reset_line) ? " reset" : "");
    return ESP_OK;
}

esp_err_t solar_os_sx1262_detach(const char *name)
{
    solar_os_sx1262_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(solar_os_radio_unregister(name), TAG, "radio is in use");
    (void)sx1262_set_state(&device->radio, SOLAR_OS_RADIO_STATE_SLEEP);
    clear_device(device);
    return ESP_OK;
}
