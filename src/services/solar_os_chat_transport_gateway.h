#pragma once

#include "solar_os_chat_transport.h"

extern const solar_os_chat_transport_t solar_os_chat_gateway_transport;
/* Operate on the Gateway instance bound by the manager or class callback. */
esp_err_t solar_os_chat_gateway_transport_prepare_remove(void);
void solar_os_chat_gateway_transport_destroy(void);
