#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_native_mqtt_abi.h"

esp_err_t solar_os_mqtt_init(void);
esp_err_t solar_os_mqtt_connect(const char *url, const char *username, const char *password);
esp_err_t solar_os_mqtt_disconnect(void);
esp_err_t solar_os_mqtt_publish(const char *topic,
                                const void *payload,
                                size_t payload_len,
                                int qos,
                                bool retain,
                                int *msg_id);
esp_err_t solar_os_mqtt_subscribe(const char *topic, int qos, int *msg_id);
esp_err_t solar_os_mqtt_read_message(solar_os_mqtt_message_t *message, uint32_t timeout_ms);
esp_err_t solar_os_mqtt_get_status(solar_os_mqtt_status_t *status);
