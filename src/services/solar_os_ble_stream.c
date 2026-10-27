#include "solar_os_ble_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_ble.h"
#include "solar_os_ble_backend.h"
#include "solar_os_port.h"

#define BLE_STREAM_MAX 4

typedef struct {
    char name[SOLAR_OS_PORT_NAME_MAX];
    bool listener;
    uint8_t address[6], address_type;
    solar_os_ble_session_t session;
    solar_os_ble_peer_t peer;
    uint32_t channel;
} ble_stream_t;
static ble_stream_t endpoints[BLE_STREAM_MAX];
static SemaphoreHandle_t stream_mutex;
static StaticSemaphore_t stream_mutex_storage;
static portMUX_TYPE stream_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void stream_lock(void)
{
    portENTER_CRITICAL(&stream_init_lock);
    if (!stream_mutex) stream_mutex = xSemaphoreCreateMutexStatic(&stream_mutex_storage);
    portEXIT_CRITICAL(&stream_init_lock);
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
}
static void stream_unlock(void) { xSemaphoreGive(stream_mutex); }

static void release_connection(ble_stream_t *s)
{
    solar_os_ble_backend_coc_close(s->channel);
    s->channel = 0;
    if (s->session) (void)solar_os_ble_session_close(s->session);
    s->session = 0; s->peer = 0;
}
static esp_err_t listen_connection(ble_stream_t *s)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_CREATE, .capacity=4};
    snprintf(r.text, sizeof(r.text), "SolarOS %s", s->name);
    esp_err_t err = solar_os_ble_server_request(s->session, &r);
    if (err) return err;
    r = (solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SERVICE};
    strlcpy(r.text, SOLAR_OS_BLE_STREAM_SERVICE_UUID, sizeof(r.text));
    err = solar_os_ble_server_request(s->session, &r);
    if (err) return err;
    uint16_t mtu = solar_os_ble_backend_coc_mtu();
    r = (solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CHARACTERISTIC,
        .parent=r.id, .properties=0x02, .value_len=5,
        .value={1, SOLAR_OS_BLE_STREAM_PSM, 0, mtu & 0xff, mtu >> 8}};
    strlcpy(r.text, SOLAR_OS_BLE_STREAM_INFO_UUID, sizeof(r.text));
    err = solar_os_ble_server_request(s->session, &r);
    if (err) return err;
    /* Queue the PSM before advertising. The shared host queue preserves this
     * order, so a central never sees an offer without a channel listener. */
    err = solar_os_ble_backend_coc_open(s->session, SOLAR_OS_BLE_CONNECTION_INVALID,
        true, &s->channel);
    if (err) return err;
    r = (solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_START};
    return solar_os_ble_server_request(s->session, &r);
}
static esp_err_t connect_connection(ble_stream_t *s)
{
    esp_err_t err = solar_os_ble_peer_connect(s->session, s->address,
        s->address_type, 8000, &s->peer);
    if (err) return err;
    /* Refuse unrelated peripherals and incompatible protocol descriptors.
     * Discovery uses the existing owner-scoped GATT service, not stack handles. */
    solar_os_ble_gatt_service_t *services = calloc(SOLAR_OS_BLE_GATT_MAX_SERVICES, sizeof(*services));
    if (!services) return ESP_ERR_NO_MEM;
    size_t count = 0;
    err = solar_os_ble_peer_services(s->session, s->peer, services,
        SOLAR_OS_BLE_GATT_MAX_SERVICES, &count);
    size_t index = 0;
    if (!err) while (index < count && strcmp(services[index].uuid, SOLAR_OS_BLE_STREAM_SERVICE_UUID)) index++;
    free(services);
    if (err) return err;
    if (index == count) return ESP_ERR_NOT_SUPPORTED;
    /* These discovery arrays are too large for a foreground presenter stack. */
    solar_os_ble_gatt_characteristic_t *chars = calloc(SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, sizeof(*chars));
    if (!chars) return ESP_ERR_NO_MEM;
    err = solar_os_ble_peer_characteristics(s->session, s->peer, index, chars,
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, &count);
    uint16_t handle = 0;
    if (!err) for (size_t i = 0; i < count; i++)
        if (!strcmp(chars[i].uuid, SOLAR_OS_BLE_STREAM_INFO_UUID)) handle = chars[i].handle;
    free(chars);
    if (err) return err;
    if (!handle) return ESP_ERR_NOT_SUPPORTED;
    uint8_t value[5]; size_t length = 0;
    err = solar_os_ble_peer_read(s->session, s->peer, handle, value, sizeof(value), &length, 3000);
    if (err) return err;
    if (length != 5 || value[0] != 1 || value[1] != SOLAR_OS_BLE_STREAM_PSM ||
        value[2] || !(value[3] | value[4])) return ESP_ERR_NOT_SUPPORTED;
    solar_os_ble_session_info_t info;
    err = solar_os_ble_peer_get_info(s->session, s->peer, &info);
    if (err || !info.gatt.connected) return err ? err : ESP_FAIL;
    return solar_os_ble_backend_coc_open(s->session, info.gatt.conn_id, false, &s->channel);
}
static esp_err_t open_stream(void *user)
{
    ble_stream_t *s = user;
    stream_lock();
    char owner[SOLAR_OS_BLE_OWNER_MAX];
    snprintf(owner, sizeof(owner), "stream:%s", s->name);
    esp_err_t err = solar_os_ble_session_create(owner, &s->session);
    if (!err) err = s->listener ? listen_connection(s) : connect_connection(s);
    if (err) release_connection(s);
    stream_unlock();
    return err;
}
static esp_err_t close_stream(void *user)
{
    stream_lock();
    release_connection(user);
    stream_unlock();
    return ESP_OK;
}
static esp_err_t read_stream(void *user, uint8_t *data, size_t len,
    uint32_t timeout_ms, size_t *received)
{
    (void)timeout_ms;
    stream_lock();
    ble_stream_t *s = user;
    esp_err_t err = solar_os_ble_backend_coc_read(s->channel, data, len, received);
    stream_unlock(); return err;
}
static esp_err_t write_stream(void *user, const uint8_t *data, size_t len, size_t *written)
{
    stream_lock();
    ble_stream_t *s = user;
    esp_err_t err = solar_os_ble_backend_coc_write(s->channel, data, len, written);
    stream_unlock(); return err;
}
static esp_err_t configure(const char *name, const uint8_t *address, uint8_t type)
{
    if (!name || !*name || strlen(name) >= SOLAR_OS_PORT_NAME_MAX || type > 3)
        return ESP_ERR_INVALID_ARG;
    stream_lock();
    ble_stream_t *s = NULL;
    for (size_t i = 0; i < BLE_STREAM_MAX; i++) {
        if (!strcmp(endpoints[i].name, name)) { stream_unlock(); return ESP_ERR_INVALID_STATE; }
        if (!s && !endpoints[i].name[0]) s = &endpoints[i];
    }
    if (!s) { stream_unlock(); return ESP_ERR_NO_MEM; }
    *s = (ble_stream_t){.listener=address == NULL, .address_type=type};
    strlcpy(s->name, name, sizeof(s->name));
    if (address) memcpy(s->address, address, sizeof(s->address));
    solar_os_port_driver_t driver = {.name=s->name,
        .label=address ? "BLE CoC central" : "BLE CoC listener",
        .capabilities=SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE,
        .open=open_stream, .close=close_stream, .read=read_stream, .write=write_stream, .user=s};
    esp_err_t err = solar_os_port_register(&driver);
    if (err) memset(s, 0, sizeof(*s));
    stream_unlock(); return err;
}
esp_err_t solar_os_ble_stream_listen(const char *name) { return configure(name, NULL, 0); }
esp_err_t solar_os_ble_stream_connect(const char *name, const uint8_t address[6], uint8_t type)
{
    return address ? configure(name, address, type) : ESP_ERR_INVALID_ARG;
}
esp_err_t solar_os_ble_stream_remove(const char *name)
{
    if (!name || !*name) return ESP_ERR_INVALID_ARG;
    stream_lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < BLE_STREAM_MAX; i++) if (!strcmp(endpoints[i].name, name)) {
        err = solar_os_port_unregister(name);
        if (!err) { release_connection(&endpoints[i]); memset(&endpoints[i], 0, sizeof(endpoints[i])); }
        break;
    }
    stream_unlock(); return err;
}
esp_err_t solar_os_ble_stream_status(const char *name, solar_os_ble_stream_info_t *info)
{
    if (!name || !*name || !info) return ESP_ERR_INVALID_ARG;
    stream_lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < BLE_STREAM_MAX; i++) if (!strcmp(endpoints[i].name, name)) {
        ble_stream_t *s = &endpoints[i];
        *info = (solar_os_ble_stream_info_t){.claimed=s->session != 0, .listening=s->listener};
        if (s->channel) {
            solar_os_ble_coc_info_t channel = {0};
            err = solar_os_ble_backend_coc_status(s->channel, &channel);
            info->connected = !err && channel.connected;
            info->failed = err || channel.failed;
            info->rx_mtu = channel.rx_mtu; info->tx_mtu = channel.tx_mtu;
            info->connection_interval_us = channel.connection_interval_us;
            info->rx_pending = channel.rx_pending; info->tx_pending = channel.tx_pending;
        }
        err = ESP_OK; break;
    }
    stream_unlock(); return err;
}
