/* Exercise endpoint ownership through the real port and stream registries.
 * The separate CoC harness tests the native host queue and buffer contract. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_ble_stream.h"
#include "solar_os_ble_backend.h"
#include "solar_os_port.h"
#include "solar_os_stream.h"

static uint32_t sessions, created, closed, channel, next_channel;
static bool fail_connect, fail_read, fail_channel, bad_descriptor, peripheral_busy;
static uint16_t seen_conn;
static solar_os_ble_coc_info_t channel_info;
uint64_t solar_os_time_uptime_ms(void) { return 0; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *value) { *value=0; return ESP_OK; }
esp_err_t solar_os_ble_session_create(const char *owner, solar_os_ble_session_t *out)
{ assert(!strncmp(owner,"stream:",7)); *out=++created; sessions++; return ESP_OK; }
esp_err_t solar_os_ble_session_close(solar_os_ble_session_t owner)
{ assert(owner && sessions); sessions--; closed++; return ESP_OK; }
esp_err_t solar_os_ble_peer_connect(solar_os_ble_session_t owner, const uint8_t bda[6],
    uint8_t type, uint32_t timeout, solar_os_ble_peer_t *peer)
{ assert(owner && bda[0]==0xa0 && type==1 && timeout); *peer=99; return fail_connect ? ESP_FAIL : ESP_OK; }
esp_err_t solar_os_ble_peer_services(solar_os_ble_session_t owner, solar_os_ble_peer_t peer,
    solar_os_ble_gatt_service_t *services, size_t max, size_t *count)
{ assert(owner && peer==99 && max); *count=1; strcpy(services[0].uuid,SOLAR_OS_BLE_STREAM_SERVICE_UUID); return ESP_OK; }
esp_err_t solar_os_ble_peer_characteristics(solar_os_ble_session_t owner, solar_os_ble_peer_t peer,
    size_t index, solar_os_ble_gatt_characteristic_t *chars, size_t max, size_t *count)
{ assert(owner && peer==99 && !index && max); *count=1; chars[0].handle=12;
  strcpy(chars[0].uuid,SOLAR_OS_BLE_STREAM_INFO_UUID); return ESP_OK; }
esp_err_t solar_os_ble_peer_read(solar_os_ble_session_t owner, solar_os_ble_peer_t peer,
    uint16_t handle, uint8_t *value, size_t max, size_t *len, uint32_t timeout)
{ assert(owner && peer==99 && handle==12 && max==5 && timeout);
  memcpy(value,(uint8_t[]){bad_descriptor ? 2 : 1,0x80,0,246,0},5); *len=5; return fail_read ? ESP_FAIL : ESP_OK; }
esp_err_t solar_os_ble_peer_get_info(solar_os_ble_session_t owner, solar_os_ble_peer_t peer,
    solar_os_ble_session_info_t *info)
{ assert(owner && peer==99); *info=(solar_os_ble_session_info_t){.gatt={.connected=true,.conn_id=7}}; return ESP_OK; }
esp_err_t solar_os_ble_server_request(solar_os_ble_session_t owner, solar_os_ble_server_request_t *r)
{ assert(owner); if (r->op==SOLAR_OS_BLE_SERVER_CREATE && peripheral_busy) return ESP_ERR_INVALID_STATE;
  if (r->op==SOLAR_OS_BLE_SERVER_SERVICE) r->id=3;
  if (r->op==SOLAR_OS_BLE_SERVER_CHARACTERISTIC) assert(r->parent==3 && r->value_len==5 && r->value[1]==0x80);
  return ESP_OK; }
uint16_t solar_os_ble_backend_coc_mtu(void) { return 246; }
esp_err_t solar_os_ble_backend_coc_open(solar_os_ble_session_t owner, uint16_t conn,
    bool listener, uint32_t *token)
{ assert(owner); seen_conn=conn; assert(conn==(listener ? 0xffff : 7));
  if (fail_channel) return ESP_ERR_NO_MEM;
  assert(!channel); *token=channel=++next_channel; channel_info=(solar_os_ble_coc_info_t){.connected=true,.rx_mtu=246,.tx_mtu=246}; return ESP_OK; }
void solar_os_ble_backend_coc_close(uint32_t token) { if (token) { assert(token==channel); channel=0; } }
esp_err_t solar_os_ble_backend_coc_status(uint32_t token, solar_os_ble_coc_info_t *info)
{ assert(token==channel && token); *info=channel_info; return ESP_OK; }
esp_err_t solar_os_ble_backend_coc_read(uint32_t token, uint8_t *data, size_t len, size_t *read)
{ assert(token && token==channel); *read=0; (void)data; (void)len; return channel_info.failed ? ESP_FAIL : ESP_ERR_TIMEOUT; }
esp_err_t solar_os_ble_backend_coc_write(uint32_t token, const uint8_t *data, size_t len, size_t *written)
{ assert(token && token==channel && data); *written=channel_info.failed ? 0 : len;
  return channel_info.failed ? ESP_FAIL : ESP_OK; }

int main(void)
{
    assert(!solar_os_port_init()); assert(!solar_os_stream_init());
    uint8_t address[6]={0xa0,1,2,3,4,5}, data=0;
    assert(solar_os_ble_stream_connect("ble0",NULL,0)==ESP_ERR_INVALID_ARG);
    assert(solar_os_ble_stream_connect("ble0",address,4)==ESP_ERR_INVALID_ARG);
    assert(!solar_os_ble_stream_connect("ble0",address,1));
    assert(!created && !channel);
    solar_os_ble_stream_info_t status;
    assert(solar_os_ble_stream_status("",&status)==ESP_ERR_INVALID_ARG);
    assert(!solar_os_ble_stream_status("ble0",&status) && !status.claimed && !status.listening);
    assert(solar_os_ble_stream_listen("ble0")==ESP_ERR_INVALID_STATE);
    solar_os_port_handle_t h=SOLAR_OS_PORT_HANDLE_INIT;
    /* Each failure must roll back both BLE resources and the common claim. */
    for (unsigned failure=0;failure<4;failure++) {
        fail_connect=failure==0; fail_read=failure==1; bad_descriptor=failure==2; fail_channel=failure==3;
        assert(solar_os_port_claim("ble0","test",&h)!=ESP_OK);
        assert(!sessions && created==closed && !channel && !solar_os_port_handle_valid(&h));
    }
    fail_connect=fail_read=bad_descriptor=fail_channel=false;
    assert(!solar_os_port_claim("ble0","test",&h) && sessions==1 && seen_conn==7);
    uint32_t old=channel;
    assert(solar_os_ble_stream_remove("ble0")==ESP_ERR_INVALID_STATE);
    assert(!solar_os_ble_stream_status("ble0",&status) && status.claimed && status.connected);
    size_t n;
    assert(!solar_os_port_write(&h,&data,1,&n) && n==1);
    channel_info.failed=true;
    assert(solar_os_port_read(&h,&data,1,0,&n)==ESP_FAIL);
    assert(solar_os_port_write(&h,&data,1,&n)==ESP_FAIL);
    assert(!solar_os_port_release(&h) && !sessions && !channel);
    assert(!solar_os_port_claim("ble0","next",&h) && channel!=old);
    assert(!solar_os_port_release(&h)); assert(!solar_os_ble_stream_remove("ble0"));
    assert(!solar_os_ble_stream_listen("ble0")); peripheral_busy=true;
    assert(solar_os_port_claim("ble0","server",&h)==ESP_ERR_INVALID_STATE && !sessions);
    peripheral_busy=false;
    assert(!solar_os_port_claim("ble0","server",&h) && seen_conn==0xffff);
    assert(!solar_os_port_release(&h)); assert(!solar_os_ble_stream_remove("ble0"));
    assert(!sessions && !channel && created==closed);
    for (unsigned i=0;i<4;i++) { char name[8]; snprintf(name,sizeof(name),"ble%u",i); assert(!solar_os_ble_stream_listen(name)); }
    assert(solar_os_ble_stream_listen("ble4")==ESP_ERR_NO_MEM);
    for (unsigned i=0;i<4;i++) { char name[8]; snprintf(name,sizeof(name),"ble%u",i); assert(!solar_os_ble_stream_remove(name)); }
    puts("BLE stream registry and ownership tests passed");
}
