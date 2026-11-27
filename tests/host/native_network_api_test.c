#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_network.h"
#include "solar_os_native_handles.h"
#include "solar_os_net_session.h"
#include "solar_os_memory.h"

struct solar_os_net_session {
    solar_os_net_session_cancel_fn cancel;
    void *user;
    char owner[24];
    uint32_t channels;
};
static const solar_os_native_net_api_v1_t *api;
static unsigned allocations,events,visits;
static bool waiting,release_wait,block_receive;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static solar_os_net_session_cancel_fn saved_cancel;
static void *saved_user;

void *solar_os_memory_calloc(size_t count,size_t size,solar_os_memory_class_t cls,const char *tag)
{
    assert(cls==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    void *value=calloc(count,size);if(value)++allocations;return value;
}
void solar_os_memory_free(void *value) { if(value)--allocations;free(value); }
esp_err_t solar_os_net_session_create(const char *owner,solar_os_net_session_cancel_fn cancel,void *user,solar_os_net_session_t **out)
{
    *out=calloc(1,sizeof(**out));assert(*out);
    (*out)->cancel=cancel;(*out)->user=user;strcpy((*out)->owner,owner);
    saved_cancel=cancel;saved_user=user;return ESP_OK;
}
void solar_os_net_session_destroy(solar_os_net_session_t *state) { assert(state);free(state); }
esp_err_t solar_os_net_session_tcp_connect(solar_os_net_session_t *state,const char *host,uint16_t port,uint32_t timeout,uint32_t *out)
{
    assert(!strcmp(host,"peer") && port==443 && timeout==37);
    if(state->cancel(state->user))return ESP_ERR_TIMEOUT;
    state->channels|=1;*out=1;return ESP_OK;
}
esp_err_t solar_os_net_session_tls_connect(solar_os_net_session_t *state, const char *host,
    uint16_t port, uint32_t timeout, uint32_t *out)
{
    assert(!strcmp(host, "peer") && port == 443 && timeout == 37);
    if (state->cancel(state->user)) return ESP_ERR_INVALID_STATE;
    state->channels |= 8U;
    *out = 4;
    return ESP_OK;
}
esp_err_t solar_os_net_session_udp_open(solar_os_net_session_t *state,uint16_t port,uint32_t *out)
{ assert(port==55);state->channels|=2;*out=2;return ESP_OK; }
esp_err_t solar_os_net_session_websocket_connect(solar_os_net_session_t *state,const char *url,const char *subprotocol,uint32_t timeout,uint32_t *out)
{ assert(!strcmp(url,"wss://peer/ws") && !strcmp(subprotocol,"proto") && timeout==37);state->channels|=4;*out=3;return ESP_OK; }
esp_err_t solar_os_net_session_tcp_send(solar_os_net_session_t *state,uint32_t id,const void *bytes,size_t size,uint32_t timeout)
{ assert(state && id==1 && size==2 && !memcmp(bytes,"hi",2) && timeout==37);return ESP_OK; }
esp_err_t solar_os_net_session_udp_send(solar_os_net_session_t *state,uint32_t id,const char *host,uint16_t port,const void *bytes,size_t size,uint32_t timeout)
{ assert(id==2 && !strcmp(host,"peer") && port==9);return solar_os_net_session_tcp_send(state,1,bytes,size,timeout); }
esp_err_t solar_os_net_session_websocket_send(solar_os_net_session_t *state,uint32_t id,const void *bytes,size_t size,bool text,uint32_t timeout)
{ assert(id==3 && text);return solar_os_net_session_tcp_send(state,1,bytes,size,timeout); }
esp_err_t solar_os_net_session_tls_send(solar_os_net_session_t *state, uint32_t id,
    const void *data, size_t size, uint32_t timeout)
{
    assert(id == 4 && (state->channels & 8U));
    return solar_os_net_session_tcp_send(state, 1, data, size, timeout);
}
static esp_err_t receive(solar_os_net_session_t *state,uint32_t id,void *bytes,size_t capacity,uint32_t timeout,solar_os_net_receive_result_t *out)
{
    assert(id>=1 && id<=3 && capacity>=4 && timeout==37);
    pthread_mutex_lock(&lock);waiting=true;pthread_cond_broadcast(&condition);
    while(block_receive && !release_wait)pthread_cond_wait(&condition,&lock);
    pthread_mutex_unlock(&lock);
    if(state->cancel(state->user))return ESP_ERR_TIMEOUT;
    memcpy(bytes,"data",4);
    *out=(solar_os_net_receive_result_t){.data_len=4,.message_len=9999,.truncated=true,
        .final=true,.opcode=SOLAR_OS_NET_WS_BINARY,.port=65535};
    strcpy(out->address,"2001:db8::1");return ESP_OK;
}
esp_err_t solar_os_net_session_tcp_receive(solar_os_net_session_t *state,uint32_t id,void *data,size_t size,uint32_t timeout,solar_os_net_receive_result_t *out)
{ return receive(state,id,data,size,timeout,out); }
esp_err_t solar_os_net_session_udp_receive(solar_os_net_session_t *state,uint32_t id,void *data,size_t size,uint32_t timeout,solar_os_net_receive_result_t *out)
{ return receive(state,id,data,size,timeout,out); }
esp_err_t solar_os_net_session_websocket_receive(solar_os_net_session_t *state,uint32_t id,void *data,size_t size,uint32_t timeout,solar_os_net_receive_result_t *out)
{ return receive(state,id,data,size,timeout,out); }
esp_err_t solar_os_net_session_tls_receive(solar_os_net_session_t *state, uint32_t id,
    void *data, size_t size, uint32_t timeout, solar_os_net_receive_result_t *out)
{
    assert(id == 4 && (state->channels & 8U) && size >= 4 && timeout == 37);
    memcpy(data, "data", 4);
    *out = (solar_os_net_receive_result_t){.data_len = 4, .message_len = 4};
    return ESP_OK;
}
esp_err_t solar_os_net_session_close(solar_os_net_session_t *state,uint32_t id)
{ assert(id>=1 && id<=4);state->channels&=~(1U<<(id-1));return ESP_OK; }
void solar_os_net_session_close_all(solar_os_net_session_t *state) { state->channels=0; }
void solar_os_net_session_get_status(const solar_os_net_session_t *state,solar_os_net_session_status_t *out)
{
    memset(out,0,sizeof(*out));strcpy(out->owner,state->owner);
    for(unsigned i=0;i<4;++i)out->open_channels+=(state->channels>>i)&1;
    out->session_limit=4;out->global_open_channels=out->open_channels;out->global_limit=8;
}
const char *solar_os_net_ws_opcode_name(solar_os_net_ws_opcode_t opcode)
{ return opcode==SOLAR_OS_NET_WS_BINARY?"binary":"other"; }
esp_err_t solar_os_net_resolve_host(const char *host,char *out,size_t capacity)
{ assert(!strcmp(host,"peer"));snprintf(out,capacity,"2001:db8::1");return ESP_OK; }
esp_err_t solar_os_net_hosts_visit(solar_os_net_host_visitor_t visitor,void *user)
{ (void)visitor("2001:db8::1","peer",user);return ESP_OK; }
esp_err_t solar_os_net_ping(const char *host,const solar_os_net_ping_options_t *options,
    solar_os_net_ping_event_fn_t event,void *event_user,solar_os_net_ping_stop_fn_t stop,
    void *stop_user,solar_os_net_ping_result_t *out)
{
    assert(!strcmp(host,"peer") && options->count==3 && options->timeout_ms==37 && options->interval_ms==10 && options->data_size==123);
    solar_os_net_ping_event_t value={.type=SOLAR_OS_NET_PING_REPLY,.seqno=65535,.bytes=123,.elapsed_ms=8,.ttl=254};
    strcpy(value.from,"2001:db8::1");event(&value,event_user);
    *out=(solar_os_net_ping_result_t){.interrupted=stop(stop_user),.transmitted=3,.received=1,.loss_percent=66,
        .total_time_ms=30,.min_time_ms=8,.avg_time_ms=8,.max_time_ms=8};
    strcpy(out->resolved_ip,value.from);return ESP_OK;
}
static bool visit(const char *address,const char *alias,void *user)
{ assert(user==&visits && !strcmp(address,"2001:db8::1") && !strcmp(alias,"peer"));++visits;return false; }
static void ping_event(const solar_os_native_ping_event_v1_t *event,void *user)
{
    uint32_t context=*(uint32_t *)user;
    assert(event->type==SOLAR_OS_NATIVE_NET_PING_REPLY && event->sequence==65535 && event->ttl==254);
    assert(!strcmp(event->from,"2001:db8::1"));++events;
    assert(!api->cancel(context,true));
}
static void *read_worker(void *user)
{
    uint32_t context=*(uint32_t *)user;char data[8];solar_os_native_net_receive_v1_t result;
    assert(api->receive(context,1,SOLAR_OS_NATIVE_NET_TCP,data,sizeof(data),37,&result)==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(result.bytes==0);return NULL;
}
int main(void)
{
    api=solar_os_native_network_get_service("net",1,sizeof(*api));assert(api);
    assert(api->capabilities & SOLAR_OS_NATIVE_NET_CHANNELS);
    assert(api->capabilities & SOLAR_OS_NATIVE_NET_TLS_CHANNELS);
    uint32_t context=0,channel=0;assert(!api->create("fixture",&context) && context);
    char address[46];assert(!api->resolve("peer",address,sizeof(address)) && !strcmp(address,"2001:db8::1"));
    assert(!api->hosts(visit,&visits) && visits==1);
    solar_os_native_ping_options_v1_t options={3,37,10,123};solar_os_native_ping_result_v1_t ping;
    assert(!api->ping(context,"peer",&options,ping_event,&context,&ping));
    assert(events==1 && ping.interrupted && ping.received==1 && ping.loss_percent==66);
    assert(!api->cancel(context,false));
    assert(!api->tcp_connect(context,"peer",443,37,&channel) && channel==1);
    assert(!api->udp_open(context,55,&channel) && channel==2);
    assert(!api->websocket_connect(context,"wss://peer/ws","proto",37,&channel) && channel==3);
    for(unsigned kind=1;kind<=3;++kind) {
        assert(!api->send(context,kind,kind,"peer",9,"hi",2,true,37));
        char data[8];solar_os_native_net_receive_v1_t value;
        assert(!api->receive(context,kind,kind,data,sizeof(data),37,&value));
        assert(value.bytes==4 && value.message_bytes==9999 && value.port==65535 && value.opcode==2);
        assert(value.flags==(SOLAR_OS_NATIVE_NET_TRUNCATED|SOLAR_OS_NATIVE_NET_FINAL));
    }
    solar_os_native_net_status_v1_t status;assert(!api->status(context,&status) && status.channels==3 && status.global_limit==8);
    assert(!strcmp(api->websocket_opcode_name(2),"binary"));
    char tls_data[8];
    solar_os_native_net_receive_v1_t tls;
    assert(!api->cancel(context, true));
    assert(api->tls_connect(context, "peer", 443, 37, &channel) == SOLAR_OS_NATIVE_ERROR_CANCELLED && !channel);
    assert(!api->cancel(context, false));
    assert(!api->tls_connect(context, "peer", 443, 37, &channel) && channel == 4);
    assert(!api->send(context, channel, SOLAR_OS_NATIVE_NET_TLS, NULL, 0, "hi", 2, false, 37));
    assert(!api->receive(context, channel, SOLAR_OS_NATIVE_NET_TLS, tls_data, sizeof(tls_data), 37, &tls));
    assert(tls.bytes == 4 && !tls.flags && !memcmp(tls_data, "data", 4));
    assert(!api->status(context, &status) && status.channels == 4);
    assert(!api->channel_close(context, channel));

    waiting=false;block_receive=true;
    pthread_t thread;assert(!pthread_create(&thread,NULL,read_worker,&context));
    pthread_mutex_lock(&lock);while(!waiting)pthread_cond_wait(&condition,&lock);pthread_mutex_unlock(&lock);
    assert(api->close(context)==SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!api->cancel(context,true));
    pthread_mutex_lock(&lock);release_wait=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&lock);
    assert(!pthread_join(thread,NULL));
    assert(!api->cancel(context,false));
    assert(!api->channel_close(context,1) && !api->close_all(context));
    assert(!api->status(context,&status) && !status.channels);
    assert(!api->close(context) && !allocations && !solar_os_native_handle_count());
    assert(api->close(context)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    /* A delayed host cancellation poll contains a token, never freed state. */
    assert(saved_cancel(saved_user));
    puts("native network metadata, channel ownership and cancellation passed");return 0;
}
