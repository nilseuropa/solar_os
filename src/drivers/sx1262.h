#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_bus_types.h"
#include "solar_os_radio.h"

#define SX1262_MAX_PACKET_LEN 255

typedef struct {
    char spi_bus[SOLAR_OS_BUS_NAME_MAX];
    int cs_pin;
    int busy_pin;
    int reset_pin;
    int irq_pin; /* DIO1 */
    uint32_t speed_hz;
    solar_os_radio_config_t config;
    solar_os_radio_state_t state;
    int16_t last_rssi_dbm;
    int16_t last_snr_db;
    bool has_last_packet;
    SemaphoreHandle_t mutex;
} sx1262_t;

esp_err_t sx1262_init(sx1262_t *dev,
                      const char *spi_bus,
                      int cs_pin,
                      int busy_pin,
                      int reset_pin,
                      int irq_pin,
                      uint32_t speed_hz);
esp_err_t sx1262_probe(sx1262_t *dev, uint8_t *chip_mode);
esp_err_t sx1262_configure(sx1262_t *dev, const solar_os_radio_config_t *config);
esp_err_t sx1262_set_state(sx1262_t *dev, solar_os_radio_state_t state);
esp_err_t sx1262_get_status(sx1262_t *dev, solar_os_radio_status_t *status);
esp_err_t sx1262_send(sx1262_t *dev,
                      const solar_os_radio_packet_t *packet,
                      uint32_t timeout_ms);
esp_err_t sx1262_send_stream(sx1262_t *dev,
                             const uint8_t *data,
                             size_t len,
                             uint32_t timeout_ms);
esp_err_t sx1262_receive(sx1262_t *dev,
                         solar_os_radio_packet_t *packet,
                         uint32_t timeout_ms);
