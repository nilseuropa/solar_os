#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_bus_types.h"
#include "solar_os_radio.h"

#define LR11XX_MAX_PACKET_LEN 255

/* Which member of the family answered the bus, as the type byte GetVersion
 * reports it; see lr11xx_probe(). */
typedef enum {
    LR11XX_PART_UNKNOWN = 0,
    LR11XX_PART_LR1110 = 1,
    LR11XX_PART_LR1120 = 2,
    LR11XX_PART_LR1121 = 3,
} lr11xx_part_t;

typedef struct {
    lr11xx_part_t part;
    uint8_t hardware;      /* silicon revision */
    uint8_t device_code;   /* raw type byte the part reported */
    uint8_t firmware_major;
    uint8_t firmware_minor;
} lr11xx_version_t;

/*
 * Which of the part's switch-capable DIOs to drive high, as the bits of
 * SetDioAsRfSwitch. The datasheet numbers them RFSW0..RFSW4 and maps them
 * to DIO5, DIO6, DIO7, DIO8 and DIO10 in that order; DIO9 is the interrupt
 * line and is not among them.
 */
#define LR11XX_RF_SWITCH_DIO5 0x01U
#define LR11XX_RF_SWITCH_DIO6 0x02U
#define LR11XX_RF_SWITCH_DIO7 0x04U
#define LR11XX_RF_SWITCH_DIO8 0x08U
#define LR11XX_RF_SWITCH_DIO10 0x10U

/*
 * The antenna switch is a property of the board, not of the part: the same
 * LR1110 appears with the receive port on one DIO on one board and on the
 * other DIO on the next. A board therefore states its own table and the
 * driver only relays it, which is why there is no default here - guessing
 * would park the switch on the wrong port and transmit into no antenna.
 *
 * The eight fields are the eight bytes of SetDioAsRfSwitch, in its order:
 * `enable` names every DIO the part may drive at all, and the rest say
 * which of those go high in each operating mode.
 */
typedef struct {
    uint8_t enable;
    uint8_t standby;
    uint8_t rx;
    uint8_t tx;
    uint8_t tx_hp;
    uint8_t tx_hf;
    uint8_t gnss;
    uint8_t wifi;
} lr11xx_rf_switch_t;

typedef struct {
    char spi_bus[SOLAR_OS_BUS_NAME_MAX];
    int cs_pin;
    int busy_pin;
    int reset_pin;
    int irq_pin; /* DIO9 on the LR11xx, wired as the single IRQ line */
    uint32_t speed_hz;
    uint16_t tcxo_mv; /* 0 when the board clocks the part from a crystal */
    lr11xx_rf_switch_t rf_switch;
    bool has_rf_switch;
    lr11xx_part_t part;
    solar_os_radio_config_t config;
    /* Whether `config` is on the part: not before the first configure, and
     * not after a failure the driver could not see the end of. */
    bool configured;
    /* Set by such a failure: the part is brought to standby, awake, before
     * it is next used. */
    bool recover;
    solar_os_radio_state_t state;
    int16_t last_rssi_dbm;
    int16_t last_snr_db;
    bool has_last_packet;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
} lr11xx_t;

esp_err_t lr11xx_init(lr11xx_t *dev,
                      const char *spi_bus,
                      int cs_pin,
                      int busy_pin,
                      int reset_pin,
                      int irq_pin,
                      uint32_t speed_hz,
                      uint16_t tcxo_mv,
                      const lr11xx_rf_switch_t *rf_switch);
esp_err_t lr11xx_probe(lr11xx_t *dev, lr11xx_version_t *version);
/* Settings left at zero take the part's defaults, and ones it cannot do are
 * refused; lr11xx_get_status() then reports the settings in force. */
esp_err_t lr11xx_configure(lr11xx_t *dev, const solar_os_radio_config_t *config);
/*
 * Two errors mean one thing each throughout. ESP_ERR_TIMEOUT is the caller's
 * own timeout running out: nothing heard, or a transmission not finished.
 * ESP_ERR_INVALID_RESPONSE is the part: it did not answer, answered what no
 * part would, or refused a command. After it the state is unknown and the
 * radio unconfigured, since the part may be in any mode on any settings.
 *
 * Sending, receiving and the receive and transmit states need settings, and
 * return ESP_ERR_INVALID_STATE without touching the part while it has none:
 * before the first lr11xx_configure(), and after a failure until the next.
 */
esp_err_t lr11xx_set_state(lr11xx_t *dev, solar_os_radio_state_t state);
esp_err_t lr11xx_get_status(lr11xx_t *dev, solar_os_radio_status_t *status);
/* The part's error flags, as GetErrors reports them: zero after a clean
 * configure. */
esp_err_t lr11xx_get_errors(lr11xx_t *dev, uint16_t *errors);
esp_err_t lr11xx_send(lr11xx_t *dev,
                      const solar_os_radio_packet_t *packet,
                      uint32_t timeout_ms);
esp_err_t lr11xx_send_stream(lr11xx_t *dev,
                             const uint8_t *data,
                             size_t len,
                             uint32_t timeout_ms);
esp_err_t lr11xx_receive(lr11xx_t *dev,
                         solar_os_radio_packet_t *packet,
                         uint32_t timeout_ms);
const char *lr11xx_part_name(lr11xx_part_t part);
