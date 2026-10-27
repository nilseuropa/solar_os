#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2s_chan_handle_t;
typedef struct { int id, role; unsigned dma_desc_num, dma_frame_num; } i2s_chan_config_t;
typedef struct {
    struct { unsigned sample_rate_hz; } clk_cfg;
    struct { int data_bit_width, slot_mode, slot_mask; } slot_cfg;
    struct { int clk, din; struct { bool clk_inv; } invert_flags; } gpio_cfg;
} i2s_pdm_rx_config_t;
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 1
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define I2S_PDM_SLOT_LEFT 1
#define I2S_PDM_SLOT_RIGHT 2
#define I2S_CHANNEL_DEFAULT_CONFIG(port_, role_) { .id=(port_), .role=(role_) }
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate_) { .sample_rate_hz=(rate_) }
#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(bits_, mode_) { .data_bit_width=(bits_), .slot_mode=(mode_) }
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t, const i2s_pdm_rx_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t);
