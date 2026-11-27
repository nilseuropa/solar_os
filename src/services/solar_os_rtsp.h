#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_media_protocol_abi.h"

size_t solar_os_rtsp_header_length(const uint8_t *data, size_t length);
esp_err_t solar_os_rtsp_parse_request(const uint8_t *data,
                                      size_t length,
                                      solar_os_rtsp_request_t *request);
const char *solar_os_rtsp_method_name(solar_os_rtsp_method_t method);
