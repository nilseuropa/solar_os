#include "solar_os_native_mqtt.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_MQTT
#include "solar_os_mqtt.h"
#include "solar_os_native_support.h"

static int mqtt_connect(const char *url, const char *username, const char *password)
{ return solar_os_native_result(solar_os_mqtt_connect(url, username, password)); }

static int mqtt_disconnect(void)
{ return solar_os_native_result(solar_os_mqtt_disconnect()); }

static int mqtt_publish(const char *topic, const void *payload, size_t length, int32_t qos,
    bool retain, int32_t *message_id)
{
    int id = 0;
    const esp_err_t error = solar_os_mqtt_publish(topic, payload, length, qos, retain, &id);
    if (message_id) *message_id = id;
    return solar_os_native_result(error);
}

static int mqtt_subscribe(const char *filter, int32_t qos, int32_t *message_id)
{
    int id = 0;
    const esp_err_t error = solar_os_mqtt_subscribe(filter, qos, &id);
    if (message_id) *message_id = id;
    return solar_os_native_result(error);
}

static int mqtt_read(solar_os_mqtt_message_t *message, uint32_t timeout_ms)
{
    if (!message) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *message = (solar_os_mqtt_message_t){0};
    return solar_os_native_result(solar_os_mqtt_read_message(message, timeout_ms));
}

static int mqtt_status(solar_os_mqtt_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *status = (solar_os_mqtt_status_t){0};
    const esp_err_t error = solar_os_mqtt_get_status(status);
    if (!error) status->error = solar_os_native_result(status->error);
    return solar_os_native_result(error);
}

static const solar_os_native_mqtt_api_v1_t mqtt_api = {
    .abi_version = SOLAR_OS_NATIVE_MQTT_ABI, .struct_size = sizeof(mqtt_api),
    .connect = mqtt_connect, .disconnect = mqtt_disconnect, .publish = mqtt_publish,
    .subscribe = mqtt_subscribe, .read = mqtt_read, .status = mqtt_status,
};
#endif

const void *solar_os_native_mqtt_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_mqtt_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
