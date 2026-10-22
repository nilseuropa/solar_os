#pragma once
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
typedef void *twai_handle_t;
typedef struct { int bitrate; } twai_timing_config_t;
#define TWAI_TIMING_CONFIG_25KBITS() {.bitrate=25}
#define TWAI_TIMING_CONFIG_50KBITS() {.bitrate=50}
#define TWAI_TIMING_CONFIG_100KBITS() {.bitrate=100}
#define TWAI_TIMING_CONFIG_125KBITS() {.bitrate=125}
#define TWAI_TIMING_CONFIG_250KBITS() {.bitrate=250}
#define TWAI_TIMING_CONFIG_500KBITS() {.bitrate=500}
#define TWAI_TIMING_CONFIG_800KBITS() {.bitrate=800}
#define TWAI_TIMING_CONFIG_1MBITS() {.bitrate=1000}
#define TWAI_MODE_LISTEN_ONLY 1
#define TWAI_MODE_NORMAL 0
typedef struct { int tx_io,rx_io,mode; } twai_general_config_t;
#define TWAI_GENERAL_CONFIG_DEFAULT(tx,rx,m) {.tx_io=(tx),.rx_io=(rx),.mode=(m)}
typedef struct { int unused; } twai_filter_config_t;
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() {0}
#define TWAI_MSG_FLAG_EXTD 1
#define TWAI_MSG_FLAG_RTR 2
typedef struct { uint32_t identifier,flags; uint8_t data_length_code,data[8]; } twai_message_t;
typedef enum { TWAI_STATE_STOPPED,TWAI_STATE_RUNNING,TWAI_STATE_BUS_OFF,TWAI_STATE_RECOVERING } twai_state_t;
typedef struct {
twai_state_t state;
uint32_t msgs_to_tx,msgs_to_rx,tx_error_counter,rx_error_counter,tx_failed_count,rx_missed_count,rx_overrun_count,bus_error_count;
} twai_status_info_t;
esp_err_t twai_driver_install_v2(const twai_general_config_t *,const twai_timing_config_t *,const twai_filter_config_t *,twai_handle_t *);
esp_err_t twai_driver_uninstall_v2(twai_handle_t);
esp_err_t twai_start_v2(twai_handle_t);
esp_err_t twai_stop_v2(twai_handle_t);
esp_err_t twai_get_status_info_v2(twai_handle_t,twai_status_info_t *);
esp_err_t twai_transmit_v2(twai_handle_t,const twai_message_t *,TickType_t);
esp_err_t twai_receive_v2(twai_handle_t,twai_message_t *,TickType_t);
esp_err_t twai_initiate_recovery_v2(twai_handle_t);
