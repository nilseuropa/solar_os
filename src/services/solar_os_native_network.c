#include "solar_os_native_network.h"
#include "solar_os_native_net_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include <stdatomic.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_NET
#include "solar_os_net.h"
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET
#include "solar_os_net_session.h"
#endif
typedef struct {
    atomic_bool cancelled;
    char owner[24];
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET
    solar_os_net_session_t *channels;
#endif
} native_net_context_t;
static bool net_cancelled(void *user)
{
    uint32_t id=(uint32_t)(uintptr_t)user;
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return true;
    bool result=atomic_load(&state->cancelled);
    solar_os_native_handle_release(id);return result;
}
static int net_result(native_net_context_t *state,esp_err_t error)
{
    return error && atomic_load(&state->cancelled)?SOLAR_OS_NATIVE_ERROR_CANCELLED:solar_os_native_result(error);
}
static int net_destroy(void *value)
{
    native_net_context_t *state=value;atomic_store(&state->cancelled,true);
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET
    if (state->channels) solar_os_net_session_destroy(state->channels);
#endif
    solar_os_memory_free(state);return SOLAR_OS_NATIVE_OK;
}
static int net_create(const char *owner,solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    if (!owner || !*owner || strlen(owner)>=24) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_net_context_t *state=solar_os_memory_calloc(1,sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.net");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    atomic_init(&state->cancelled,false);memcpy(state->owner,owner,strlen(owner)+1);
    int result=solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_NET,state,net_destroy,out);
    if (result) { net_destroy(state);return result; }
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET
    result=solar_os_native_result(solar_os_net_session_create(owner,net_cancelled,
        (void *)(uintptr_t)*out,&state->channels));
    if (result && !solar_os_native_handle_close_kind(*out,SOLAR_OS_NATIVE_HANDLE_NET)) *out=0;
#endif
    return result;
}
static int net_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id,SOLAR_OS_NATIVE_HANDLE_NET); }
static int net_cancel(solar_os_native_handle_t id,bool requested)
{
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    atomic_store(&state->cancelled,requested);solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}
static int net_resolve(const char *host,char *address,size_t capacity)
{ return solar_os_native_result(solar_os_net_resolve_host(host,address,capacity)); }
static int net_hosts(solar_os_native_host_visit_fn visitor,void *user)
{
    if (!visitor) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_net_hosts_visit(visitor,user));
}
typedef struct { solar_os_native_ping_event_fn event;void *user; } native_ping_bridge_t;
static void ping_event(const solar_os_net_ping_event_t *in,void *user)
{
    native_ping_bridge_t *bridge=user;
    if (!bridge->event) return;
    solar_os_native_ping_event_v1_t out={.type=in->type==SOLAR_OS_NET_PING_REPLY?
        SOLAR_OS_NATIVE_NET_PING_REPLY:SOLAR_OS_NATIVE_NET_PING_TIMEOUT,.sequence=in->seqno,
        .bytes=in->bytes,.elapsed_ms=in->elapsed_ms,.ttl=in->ttl};
    memcpy(out.from,in->from,sizeof(out.from));bridge->event(&out,bridge->user);
}
static int net_ping(solar_os_native_handle_t id,const char *host,const solar_os_native_ping_options_v1_t *options,
    solar_os_native_ping_event_fn event,void *user,solar_os_native_ping_result_v1_t *out)
{
    if (!out || !host) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_net_ping_options_t value={0};
    if (options) value=(solar_os_net_ping_options_t){options->count,options->timeout_ms,options->interval_ms,options->data_size};
    solar_os_net_ping_result_t result={0};native_ping_bridge_t bridge={event,user};
    int error=net_result(state,solar_os_net_ping(host,options?&value:NULL,ping_event,&bridge,
        net_cancelled,(void *)(uintptr_t)id,&result));
    memcpy(out->address,result.resolved_ip,sizeof(out->address));
    out->interrupted=result.interrupted;out->transmitted=result.transmitted;out->received=result.received;
    out->loss_percent=result.loss_percent;out->total_ms=result.total_time_ms;
    out->minimum_ms=result.min_time_ms;out->average_ms=result.avg_time_ms;out->maximum_ms=result.max_time_ms;
    solar_os_native_handle_release(id);return error;
}
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET
static int net_tcp(solar_os_native_handle_t id,const char *host,uint16_t port,uint32_t timeout,uint32_t *channel)
{
    if (!channel) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *channel=0;
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=net_result(state,solar_os_net_session_tcp_connect(state->channels,host,port,timeout,channel));
    solar_os_native_handle_release(id);return result;
}
static int net_tls(solar_os_native_handle_t id, const char *host, uint16_t port,
    uint32_t timeout, uint32_t *channel)
{
    if (!channel) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *channel = 0;
    native_net_context_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = net_result(state, solar_os_net_session_tls_connect(state->channels, host, port, timeout, channel));
    solar_os_native_handle_release(id);
    return result;
}
static int net_udp(solar_os_native_handle_t id,uint16_t port,uint32_t *channel)
{
    if (!channel) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *channel=0;
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=net_result(state,solar_os_net_session_udp_open(state->channels,port,channel));
    solar_os_native_handle_release(id);return result;
}
static int net_websocket(solar_os_native_handle_t id,const char *url,const char *subprotocol,uint32_t timeout,uint32_t *channel)
{
    if (!channel) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *channel=0;
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=net_result(state,solar_os_net_session_websocket_connect(state->channels,url,subprotocol,timeout,channel));
    solar_os_native_handle_release(id);return result;
}
static int net_send(solar_os_native_handle_t id,uint32_t channel,uint32_t type,const char *host,uint16_t port,
    const void *bytes,size_t size,bool text,uint32_t timeout)
{
    if ((!bytes && size) || size>SOLAR_OS_NET_MAX_TRANSFER_BYTES) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    esp_err_t result=ESP_ERR_INVALID_ARG;
    if (type==SOLAR_OS_NATIVE_NET_TCP) result=solar_os_net_session_tcp_send(state->channels,channel,bytes,size,timeout);
    if (type==SOLAR_OS_NATIVE_NET_TLS) result=solar_os_net_session_tls_send(state->channels,channel,bytes,size,timeout);
    if (type==SOLAR_OS_NATIVE_NET_UDP) result=solar_os_net_session_udp_send(state->channels,channel,host,port,bytes,size,timeout);
    if (type==SOLAR_OS_NATIVE_NET_WEBSOCKET) result=solar_os_net_session_websocket_send(state->channels,channel,bytes,size,text,timeout);
    int error=net_result(state,result);solar_os_native_handle_release(id);return error;
}
static int net_receive(solar_os_native_handle_t id,uint32_t channel,uint32_t type,void *bytes,
    size_t capacity,uint32_t timeout,solar_os_native_net_receive_v1_t *out)
{
    if (!out || (!bytes && capacity) || capacity>SOLAR_OS_NET_MAX_TRANSFER_BYTES)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_net_receive_result_t value={0};esp_err_t result=ESP_ERR_INVALID_ARG;
    if (type==SOLAR_OS_NATIVE_NET_TCP) result=solar_os_net_session_tcp_receive(state->channels,channel,bytes,capacity,timeout,&value);
    if (type==SOLAR_OS_NATIVE_NET_TLS) result=solar_os_net_session_tls_receive(state->channels,channel,bytes,capacity,timeout,&value);
    if (type==SOLAR_OS_NATIVE_NET_UDP) result=solar_os_net_session_udp_receive(state->channels,channel,bytes,capacity,timeout,&value);
    if (type==SOLAR_OS_NATIVE_NET_WEBSOCKET) result=solar_os_net_session_websocket_receive(state->channels,channel,bytes,capacity,timeout,&value);
    if (value.data_len>capacity) result=ESP_ERR_INVALID_SIZE;
    if (!result) {
        out->message_bytes=value.message_len;out->bytes=value.data_len;out->opcode=value.opcode;out->port=value.port;
        memcpy(out->address,value.address,sizeof(out->address));
        out->flags=(value.timed_out?SOLAR_OS_NATIVE_NET_TIMED_OUT:0)|(value.closed?SOLAR_OS_NATIVE_NET_CLOSED:0)|
            (value.truncated?SOLAR_OS_NATIVE_NET_TRUNCATED:0)|(value.final?SOLAR_OS_NATIVE_NET_FINAL:0);
    }
    int error=net_result(state,result);solar_os_native_handle_release(id);return error;
}
static int net_channel_close(solar_os_native_handle_t id,uint32_t channel)
{
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_net_session_close(state->channels,channel));
    solar_os_native_handle_release(id);return result;
}
static int net_close_all(solar_os_native_handle_t id)
{
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_net_session_close_all(state->channels);solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}
static int net_status(solar_os_native_handle_t id,solar_os_native_net_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    native_net_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_NET);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_net_session_status_t value={0};solar_os_net_session_get_status(state->channels,&value);
    memcpy(out->owner,value.owner,sizeof(out->owner));out->channels=value.open_channels;
    out->session_limit=value.session_limit;out->global_channels=value.global_open_channels;
    out->global_limit=value.global_limit;solar_os_native_handle_release(id);return SOLAR_OS_NATIVE_OK;
}
static const char *net_opcode_name(uint32_t opcode)
{
    switch(opcode) {
    case 0: return solar_os_net_ws_opcode_name(SOLAR_OS_NET_WS_CONTINUATION);
    case 1: return solar_os_net_ws_opcode_name(SOLAR_OS_NET_WS_TEXT);
    case 2: return solar_os_net_ws_opcode_name(SOLAR_OS_NET_WS_BINARY);
    case 8: return solar_os_net_ws_opcode_name(SOLAR_OS_NET_WS_CLOSE);
    case 9: return solar_os_net_ws_opcode_name(SOLAR_OS_NET_WS_PING);
    case 10: return solar_os_net_ws_opcode_name(SOLAR_OS_NET_WS_PONG);
    default: return "unknown";
    }
}
#endif
static const solar_os_native_net_api_v1_t net_api={
    .abi_version=SOLAR_OS_NATIVE_NET_ABI,.struct_size=sizeof(net_api),
    .capabilities=SOLAR_OS_NATIVE_NET_DNS|SOLAR_OS_NATIVE_NET_PING|SOLAR_OS_NATIVE_NET_HOSTS
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET
        |SOLAR_OS_NATIVE_NET_CHANNELS|SOLAR_OS_NATIVE_NET_TLS_CHANNELS
#endif
        ,
    .create=net_create,.close=net_close,.cancel=net_cancel,.resolve=net_resolve,.hosts=net_hosts,.ping=net_ping,
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_NET
    .tcp_connect=net_tcp,.udp_open=net_udp,.websocket_connect=net_websocket,
    .send=net_send,.receive=net_receive,.channel_close=net_channel_close,
    .close_all=net_close_all,.status=net_status,.websocket_opcode_name=net_opcode_name, .tls_connect=net_tls,
#endif
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_OSC
#include "solar_os_osc.h"

static int osc_bind(const solar_os_native_osc_binding_config_v1_t *config, uint32_t *id)
{ return solar_os_native_result(solar_os_osc_bind(config, id)); }

static int osc_unbind(const char *name)
{ return solar_os_native_result(solar_os_osc_unbind(name)); }

static int osc_clear(void)
{ return solar_os_native_result(solar_os_osc_clear()); }

static int osc_info(size_t index, solar_os_native_osc_binding_info_v1_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!solar_os_osc_binding_get(index, info)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    info->last_error = solar_os_native_result(info->last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int osc_dispatch(const uint8_t *packet, size_t bytes, solar_os_native_osc_dispatch_result_v1_t *result)
{ return solar_os_native_result(solar_os_osc_dispatch_packet(packet, bytes, result)); }

static int osc_encode_floats(const char *address, const float *values, size_t count,
    uint8_t *packet, size_t capacity, size_t *bytes)
{ return solar_os_native_result(solar_os_osc_encode_floats(address, values, count, packet, capacity, bytes)); }

static int osc_encode_integer(const char *address, int32_t value, uint8_t *packet, size_t capacity, size_t *bytes)
{ return solar_os_native_result(solar_os_osc_encode_int(address, value, packet, capacity, bytes)); }

static int osc_prepare(uint32_t id, uint64_t now, float value, bool *send, bool *integer)
{ return solar_os_native_result(solar_os_osc_binding_prepare(id, now, value, send, integer)); }

static int osc_report(uint32_t id, uint64_t now, uint32_t outcome, int error)
{
    if (!id) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    switch (outcome) {
    case SOLAR_OS_NATIVE_OSC_SENT:
        solar_os_osc_binding_note_sent(id, now);
        break;
    case SOLAR_OS_NATIVE_OSC_SOURCE_ERROR:
    case SOLAR_OS_NATIVE_OSC_SEND_ERROR:
        solar_os_osc_binding_note_error(id, now, solar_os_native_callback_result(error),
            outcome == SOLAR_OS_NATIVE_OSC_SEND_ERROR);
        break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return SOLAR_OS_NATIVE_OK;
}

static int osc_release_receivers(void)
{ return solar_os_native_result(solar_os_osc_receive_release()); }

static const solar_os_native_osc_api_v1_t osc_api = {
    .abi_version = SOLAR_OS_NATIVE_OSC_ABI, .struct_size = sizeof(osc_api),
    .bind = osc_bind, .unbind = osc_unbind, .clear = osc_clear,
    .count = solar_os_osc_binding_count, .info = osc_info, .dispatch = osc_dispatch,
    .encode_floats = osc_encode_floats, .encode_integer = osc_encode_integer,
    .source_name = solar_os_osc_source_name, .value_name = solar_os_osc_value_name, .edge_name = solar_os_osc_edge_name,
    .due = solar_os_osc_binding_due, .prepare = osc_prepare, .report = osc_report,
    .release_receivers = osc_release_receivers,
};
#endif
const void *solar_os_native_network_get_service(const char *name,uint32_t abi,uint32_t minimum)
{
#include "generated/native_network_dispatch.inc"
    (void)name;(void)abi;(void)minimum;return NULL;
}
