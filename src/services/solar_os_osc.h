#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_native_osc_abi.h"

typedef solar_os_native_osc_binding_config_v1_t solar_os_osc_binding_config_t;
/* last_error uses esp_err_t codes within the Core registry. */
typedef solar_os_native_osc_binding_info_v1_t solar_os_osc_binding_info_t;
typedef solar_os_native_osc_dispatch_result_v1_t solar_os_osc_dispatch_result_t;

esp_err_t solar_os_osc_bind(const solar_os_osc_binding_config_t *config,
                            uint32_t *binding_id);
esp_err_t solar_os_osc_unbind(const char *name);
esp_err_t solar_os_osc_clear(void);
/* Release receive writers on worker shutdown without deleting bindings.
 * Failed closes remain reachable for a later cleanup retry. */
esp_err_t solar_os_osc_receive_release(void);
size_t solar_os_osc_binding_count(void);
bool solar_os_osc_binding_get(size_t index, solar_os_osc_binding_info_t *info);
bool solar_os_osc_binding_due(const solar_os_osc_binding_info_t *info,
                              uint64_t now_ms);
esp_err_t solar_os_osc_binding_prepare(uint32_t id,
                                       uint64_t now_ms,
                                       float value,
                                       bool *send,
                                       bool *send_integer);
void solar_os_osc_binding_note_error(uint32_t id,
                                     uint64_t now_ms,
                                     esp_err_t error,
                                     bool send_error);
void solar_os_osc_binding_note_sent(uint32_t id, uint64_t now_ms);

esp_err_t solar_os_osc_dispatch_packet(
    const uint8_t *packet,
    size_t packet_len,
    solar_os_osc_dispatch_result_t *result);
esp_err_t solar_os_osc_encode_float(const char *address,
                                    float value,
                                    uint8_t *packet,
                                    size_t packet_capacity,
                                    size_t *packet_len);
esp_err_t solar_os_osc_encode_floats(const char *address,
                                     const float *values, size_t count,
                                     uint8_t *packet, size_t packet_capacity,
                                     size_t *packet_len);
esp_err_t solar_os_osc_encode_int(const char *address,
                                  int32_t value,
                                  uint8_t *packet,
                                  size_t packet_capacity,
                                  size_t *packet_len);

const char *solar_os_osc_source_name(solar_os_osc_source_t source);
const char *solar_os_osc_value_name(solar_os_osc_value_t value);
const char *solar_os_osc_edge_name(solar_os_osc_edge_t edge);
