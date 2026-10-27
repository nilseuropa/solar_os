#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_bus_types.h"
#include "solar_os_radio.h"

#define RFM95_VERSION 0x12
#define RFM95_MAX_PACKET_LEN 255
#define RFM95_FSK_MAX_PACKET_LEN 64

typedef struct {
    char spi_bus[SOLAR_OS_BUS_NAME_MAX];
    int cs_pin;
    uint32_t speed_hz;
    solar_os_radio_config_t config;
    solar_os_radio_state_t state;
    int16_t last_rssi_dbm;
    int16_t last_snr_db;
    bool has_last_packet;
    SemaphoreHandle_t mutex;
} rfm95_t;

esp_err_t rfm95_init(rfm95_t *dev,
                     const char *spi_bus,
                     int cs_pin,
                     uint32_t speed_hz);
esp_err_t rfm95_probe(rfm95_t *dev, uint8_t *version);
esp_err_t rfm95_configure(rfm95_t *dev, const solar_os_radio_config_t *config);
esp_err_t rfm95_set_state(rfm95_t *dev, solar_os_radio_state_t state);
esp_err_t rfm95_get_status(rfm95_t *dev, solar_os_radio_status_t *status);
esp_err_t rfm95_tune_rx(rfm95_t *dev, uint32_t frequency_hz, uint32_t *settle_ms);
esp_err_t rfm95_send(rfm95_t *dev,
                     const solar_os_radio_packet_t *packet,
                     uint32_t timeout_ms);
esp_err_t rfm95_send_stream(rfm95_t *dev,
                            const uint8_t *data,
                            size_t len,
                            uint32_t timeout_ms);
esp_err_t rfm95_receive(rfm95_t *dev,
                        solar_os_radio_packet_t *packet,
                        uint32_t timeout_ms);
