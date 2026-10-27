#include "solar_os_lr11xx.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lr11xx.h"
#include "solar_os_board.h"
#include "solar_os_log.h"
#include "solar_os_radio.h"

/*
 * A board with an LR11xx states its antenna-switch table here, because the
 * table belongs to the board's RF layout and not to the part. Two boards
 * carrying the same LR1110 have been seen to use opposite DIO polarities for
 * receive, so there is deliberately no default: a board that says nothing
 * leaves the switch DIOs in high impedance, exactly as the part ships.
 */
#ifdef SOLAR_OS_BOARD_LR11XX_RF_SWITCH
static const lr11xx_rf_switch_t board_rf_switch = SOLAR_OS_BOARD_LR11XX_RF_SWITCH;
static const lr11xx_rf_switch_t *const rf_switch = &board_rf_switch;
#else
static const lr11xx_rf_switch_t *const rf_switch = NULL;
#endif

#define SOLAR_OS_LR11XX_MAX 1
#define LR11XX_DEFAULT_SPEED_HZ 2000000U
/* Only used when a board declares no `freq` binding. Both boards that carry an
 * LR11xx today ship a 915 MHz SKU, but a board is expected to say so itself. */
#define LR11XX_FALLBACK_FREQUENCY_HZ 915000000U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int busy_pin;
    int reset_pin;
    int irq_pin;
    char summary[SOLAR_OS_RADIO_SUMMARY_MAX];
    solar_os_radio_config_t default_config;
    lr11xx_t radio;
} solar_os_lr11xx_device_t;

static const char *TAG = "lr11xx";
static solar_os_lr11xx_device_t devices[SOLAR_OS_LR11XX_MAX];
static const solar_os_radio_ops_t radio_ops;

static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static solar_os_lr11xx_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_LR11XX_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_lr11xx_device_t *alloc_device(void)
{
    for (size_t i = 0; i < SOLAR_OS_LR11XX_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_radio_config_t default_config(uint32_t frequency_hz)
{
    return (solar_os_radio_config_t) {
        .frequency_hz = frequency_hz,
        .modulation = SOLAR_OS_RADIO_MODULATION_LORA,
        .rx_bandwidth_hz = 125000,
        .spreading_factor = 7,
        .coding_rate_denominator = 5,
        .preamble_len = 8,
        .sync_word_len = 1,
        .sync_word = {0x12},
        .tx_power_dbm = 14,
        .crc_enabled = true,
        .variable_length = true,
        .payload_length = LR11XX_MAX_PACKET_LEN,
    };
}

typedef struct {
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int busy_pin;
    int reset_pin;
    int irq_pin;
    uint32_t frequency_hz;
    uint16_t tcxo_mv;
} lr11xx_wiring_t;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                               size_t binding_count,
                               lr11xx_wiring_t *wiring)
{
    bool have_spi = false;
    bool have_cs = false;

    if (bindings == NULL || wiring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(wiring, 0, sizeof(*wiring));
    wiring->cs_pin = -1;
    wiring->busy_pin = -1;
    wiring->reset_pin = -1;
    wiring->irq_pin = -1;
    wiring->frequency_hz = LR11XX_FALLBACK_FREQUENCY_HZ;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(wiring->spi_bus, binding->target, sizeof(wiring->spi_bus));
            have_spi = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            wiring->cs_pin = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(wiring->spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(wiring->spi_bus, binding->target, sizeof(wiring->spi_bus));
                have_spi = true;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (binding_role_is(binding, "busy")) {
                if (wiring->busy_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                wiring->busy_pin = binding->value;
            } else if (binding_role_is(binding, "reset")) {
                if (wiring->reset_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                wiring->reset_pin = binding->value;
            } else if (binding_role_is(binding, "irq")) {
                if (wiring->irq_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                wiring->irq_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
            if (binding_role_is(binding, "freq")) {
                wiring->frequency_hz = (uint32_t)binding->value * 1000000U;
            } else if (binding_role_is(binding, "tcxo")) {
                /* The binding's range admits 0..3300; 1..1599 is no supply. */
                if (binding->value != 0 && binding->value < 1600) {
                    return ESP_ERR_INVALID_ARG;
                }
                wiring->tcxo_mv = (uint16_t)binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_spi || !have_cs || wiring->busy_pin < 0 ||
        !solar_os_expansion_find_spi_bus(wiring->spi_bus, NULL, NULL) ||
        !solar_os_expansion_spi_cs_allowed(wiring->spi_bus, wiring->cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void clear_device(solar_os_lr11xx_device_t *device)
{
    if (device == NULL) {
        return;
    }
    memset(device, 0, sizeof(*device));
    device->cs_pin = -1;
    device->busy_pin = -1;
    device->reset_pin = -1;
    device->irq_pin = -1;
}

static esp_err_t op_configure(void *ctx, const solar_os_radio_config_t *config)
{
    return lr11xx_configure((lr11xx_t *)ctx, config);
}

static esp_err_t op_set_state(void *ctx, solar_os_radio_state_t state)
{
    return lr11xx_set_state((lr11xx_t *)ctx, state);
}

static esp_err_t op_get_status(void *ctx, solar_os_radio_status_t *status)
{
    return lr11xx_get_status((lr11xx_t *)ctx, status);
}

static esp_err_t op_send(void *ctx, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return lr11xx_send((lr11xx_t *)ctx, packet, timeout_ms);
}

static esp_err_t op_send_stream(void *ctx, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    return lr11xx_send_stream((lr11xx_t *)ctx, data, len, timeout_ms);
}

static esp_err_t op_receive(void *ctx, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    return lr11xx_receive((lr11xx_t *)ctx, packet, timeout_ms);
}

static const solar_os_radio_ops_t radio_ops = {
    .configure = op_configure,
    .set_state = op_set_state,
    .get_status = op_get_status,
    .send = op_send,
    .send_stream = op_send_stream,
    .receive = op_receive,
};

static esp_err_t register_radio(solar_os_lr11xx_device_t *device)
{
    const solar_os_radio_registration_t registration = {
        .name = device->name,
        .driver = "lr11xx",
        .summary = device->summary,
        .modulations = SOLAR_OS_RADIO_MODULATION_LORA,
        .features = SOLAR_OS_RADIO_FEATURE_PACKET |
            SOLAR_OS_RADIO_FEATURE_RSSI |
            SOLAR_OS_RADIO_FEATURE_SNR |
            SOLAR_OS_RADIO_FEATURE_TX_POWER |
            SOLAR_OS_RADIO_FEATURE_CRC |
            SOLAR_OS_RADIO_FEATURE_SYNC_WORD |
            SOLAR_OS_RADIO_FEATURE_PREAMBLE |
            SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH |
            SOLAR_OS_RADIO_FEATURE_CONTINUOUS_RX,
        .max_packet_len = LR11XX_MAX_PACKET_LEN,
        .default_config = device->default_config,
        .initial_state = device->radio.state,
        .ops = &radio_ops,
        .ctx = &device->radio,
    };
    return solar_os_radio_register(&registration);
}

esp_err_t solar_os_lr11xx_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    lr11xx_wiring_t wiring = {0};

    if (name == NULL || name[0] == '\0' || find_device(name) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &wiring), TAG, "invalid bindings");

    solar_os_lr11xx_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    clear_device(device);
    device->active = true;
    device->cs_pin = wiring.cs_pin;
    device->busy_pin = wiring.busy_pin;
    device->reset_pin = wiring.reset_pin;
    device->irq_pin = wiring.irq_pin;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, wiring.spi_bus, sizeof(device->spi_bus));

    const char *step = "init";
    esp_err_t ret = lr11xx_init(&device->radio, wiring.spi_bus, wiring.cs_pin, wiring.busy_pin,
                                wiring.reset_pin, wiring.irq_pin, LR11XX_DEFAULT_SPEED_HZ,
                                wiring.tcxo_mv, rf_switch);
    lr11xx_version_t version = {0};
    if (ret == ESP_OK) {
        /* The part identifies itself, so nothing about which LR11xx this is
         * has to be compiled in or declared by the board. */
        step = "probe";
        ret = lr11xx_probe(&device->radio, &version);
    }
    device->default_config = default_config(wiring.frequency_hz);
    bool reached = false;
    if (ret == ESP_OK) {
        reached = true;
        step = "configure";
        ret = lr11xx_configure(&device->radio, &device->default_config);
    }
    /* Calibration and clock-start failures are reported here and nowhere
     * else, and the part carries on regardless, so the flags go in the
     * attach line rather than refusing the radio. Zero is a clean part. */
    uint16_t errors = 0U;
    if (ret == ESP_OK) {
        (void)lr11xx_get_errors(&device->radio, &errors);
    }
    if (ret == ESP_OK) {
        snprintf(device->summary, sizeof(device->summary), "Semtech %s LoRa radio",
                 lr11xx_part_name(version.part));
        step = "register";
        ret = register_radio(device);
    }
    if (ret != ESP_OK && reached) {
        /* A part that answered and was then not attached is put to sleep
         * rather than left awake with nothing able to reach it. */
        (void)lr11xx_set_state(&device->radio, SOLAR_OS_RADIO_STATE_SLEEP);
    }
    if (ret != ESP_OK) {
        clear_device(device);
        if (ret == ESP_ERR_NOT_FOUND) {
            SOLAR_OS_LOGW(TAG, "%s probe found no LR11xx (type byte 0x%02x)", name,
                          (unsigned)version.device_code);
        } else {
            /* The BUSY level is the one fact a stuck part gives away. */
            SOLAR_OS_LOGW(TAG, "%s %s failed: %s (busy=%d)", name, step,
                          esp_err_to_name(ret), gpio_get_level(wiring.busy_pin));
        }
        return ret;
    }

    SOLAR_OS_LOGI(TAG,
                  "%s attached as %s fw %u.%u errors 0x%04x on %s CS GPIO%d BUSY GPIO%d%s%s",
                  name,
                  lr11xx_part_name(version.part),
                  (unsigned)version.firmware_major,
                  (unsigned)version.firmware_minor,
                  (unsigned)errors,
                  wiring.spi_bus,
                  wiring.cs_pin,
                  wiring.busy_pin,
                  wiring.irq_pin >= 0 ? " irq" : "",
                  wiring.reset_pin >= 0 ? " reset" : "");
    return ESP_OK;
}

esp_err_t solar_os_lr11xx_detach(const char *name)
{
    solar_os_lr11xx_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(solar_os_radio_unregister(name), TAG, "radio is in use");
    /* Asleep before it is let go, since nothing can reach it afterwards. A
     * part that will not sleep is let go all the same: one that is dead,
     * removed or held in reset never will, and has to be detachable. */
    const esp_err_t ret = lr11xx_set_state(&device->radio, SOLAR_OS_RADIO_STATE_SLEEP);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "%s would not sleep, detached awake: %s", name,
                      esp_err_to_name(ret));
    }
    clear_device(device);
    return ESP_OK;
}
