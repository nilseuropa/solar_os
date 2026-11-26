#include "solar_os_native_http.h"
#include "solar_os_native_http_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#include <stdatomic.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT
#include "solar_os_http_client.h"
#include "solar_os_http_stream.h"
#include "solar_os_http_policy.h"
#if SOLAR_OS_PACKAGE_SERVICE_DOWNLOAD
#include "solar_os_download.h"
#endif

typedef struct {
    atomic_bool cancelled,closing;
    solar_os_http_session_context_t *connections;
    solar_os_http_stream_session_t *streams;
    uint32_t connection_ids[SOLAR_OS_HTTP_SESSION_MAX_HANDLES];
    uint32_t stream_ids[SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES];
} native_http_context_t;
typedef struct {
    solar_os_http_buffered_response_t value;
    int error;
} native_http_response_t;
static bool http_cancelled(void *user)
{
    uint32_t id=(uint32_t)(uintptr_t)user;
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return true;
    bool result=atomic_load(&state->cancelled);
    solar_os_native_handle_release(id);return result;
}
static int http_result(native_http_context_t *state,esp_err_t error)
{ return error && atomic_load(&state->cancelled)?SOLAR_OS_NATIVE_ERROR_CANCELLED:solar_os_native_result(error); }
static int http_drain(native_http_context_t *state,uint32_t timeout)
{
    bool was_cancelled=atomic_exchange(&state->cancelled,true);
    for (size_t i=0;i<SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES;++i) {
        if (!state->stream_ids[i]) continue;
        esp_err_t error=solar_os_http_stream_close_wait(state->streams,state->stream_ids[i],timeout);
        if (error!=ESP_OK && error!=ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        state->stream_ids[i]=0;
    }
    for (size_t i=0;i<SOLAR_OS_HTTP_SESSION_MAX_HANDLES;++i) {
        if (!state->connection_ids[i]) continue;
        esp_err_t error=solar_os_http_session_close(state->connections,state->connection_ids[i]);
        if (error!=ESP_OK && error!=ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        state->connection_ids[i]=0;
    }
    if (!atomic_load(&state->closing)) atomic_store(&state->cancelled,was_cancelled);
    return SOLAR_OS_NATIVE_OK;
}
static int http_destroy(void *value)
{
    native_http_context_t *state=value;
    atomic_store(&state->closing,true);atomic_store(&state->cancelled,true);
    int result=http_drain(state,2000);
    if (result) return result;
    if (state->streams) solar_os_http_stream_session_destroy(state->streams);
    if (state->connections) solar_os_http_session_context_destroy(state->connections);
    solar_os_memory_free(state);return SOLAR_OS_NATIVE_OK;
}
static int http_create(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    native_http_context_t *state=solar_os_memory_calloc(1,sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.http");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    atomic_init(&state->cancelled,false);atomic_init(&state->closing,false);
    int result=solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_HTTP,state,http_destroy,out);
    if (result) { http_destroy(state);return result; }
    result=solar_os_native_result(solar_os_http_session_context_create(http_cancelled,
        (void *)(uintptr_t)*out,&state->connections));
    if (!result) result=solar_os_native_result(solar_os_http_stream_session_create(http_cancelled,
        (void *)(uintptr_t)*out,&state->streams));
    if (result && !solar_os_native_handle_close_kind(*out,SOLAR_OS_NATIVE_HANDLE_HTTP)) *out=0;
    return result;
}
static int http_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id,SOLAR_OS_NATIVE_HANDLE_HTTP); }
static int http_cancel(solar_os_native_handle_t id,bool requested)
{
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=SOLAR_OS_NATIVE_OK;
    if (!requested && atomic_load(&state->closing)) result=SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    else atomic_store(&state->cancelled,requested);
    solar_os_native_handle_release(id);return result;
}
static int http_limits(solar_os_native_http_limits_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=(solar_os_native_http_limits_v1_t){SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES,
        SOLAR_OS_HTTP_STREAM_GLOBAL_MAX_HANDLES,SOLAR_OS_HTTP_SESSION_MAX_HANDLES,
        SOLAR_OS_HTTP_SESSION_GLOBAL_MAX_HANDLES,SOLAR_OS_HTTP_BUFFERED_MAX_BODY,
        SOLAR_OS_HTTP_BUFFERED_MAX_HEADERS,SOLAR_OS_HTTP_BUFFERED_MAX_HEADER_BYTES,
        SOLAR_OS_HTTP_STREAM_MAX_TIMEOUT_MS};
    return SOLAR_OS_NATIVE_OK;
}
static int method_in(uint32_t value,solar_os_http_method_t *out)
{
    switch(value) {
    case SOLAR_OS_NATIVE_HTTP_GET: *out=SOLAR_OS_HTTP_METHOD_GET;break;
    case SOLAR_OS_NATIVE_HTTP_POST: *out=SOLAR_OS_HTTP_METHOD_POST;break;
    case SOLAR_OS_NATIVE_HTTP_PUT: *out=SOLAR_OS_HTTP_METHOD_PUT;break;
    case SOLAR_OS_NATIVE_HTTP_PATCH: *out=SOLAR_OS_HTTP_METHOD_PATCH;break;
    case SOLAR_OS_NATIVE_HTTP_DELETE: *out=SOLAR_OS_HTTP_METHOD_DELETE;break;
    case SOLAR_OS_NATIVE_HTTP_HEAD: *out=SOLAR_OS_HTTP_METHOD_HEAD;break;
    default: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    }
    return SOLAR_OS_NATIVE_OK;
}
static int http_method_parse(const char *text,uint32_t *out)
{
    if (!text || !out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;solar_os_http_method_t method;
    if (!solar_os_http_method_parse(text,&method)) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    for (uint32_t i=0;i<=SOLAR_OS_NATIVE_HTTP_HEAD;++i) {
        solar_os_http_method_t value;
        if (!method_in(i,&value) && value==method) { *out=i;return SOLAR_OS_NATIVE_OK; }
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
}
static int options_in(const solar_os_native_http_options_v1_t *in,solar_os_http_request_options_t *out,
    solar_os_http_header_t **headers)
{
    *headers=NULL;memset(out,0,sizeof(*out));
    if (!in || !in->url || (in->header_count && !in->headers) || (in->body_size && !in->body) ||
        in->max_redirects>UINT8_MAX || (in->flags & ~SOLAR_OS_NATIVE_HTTP_FOLLOW_REDIRECTS))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int result=method_in(in->method,&out->method);
    if (result) return result;
    if (in->header_count) {
        size_t count=in->header_count;
        if (count>SIZE_MAX/sizeof(**headers)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        *headers=solar_os_memory_calloc(in->header_count,sizeof(**headers),
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.http.headers");
        if (!*headers) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
        for (uint32_t i=0;i<in->header_count;++i) {
            if (!in->headers[i].name || !in->headers[i].value) {
                solar_os_memory_free(*headers);*headers=NULL;return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            }
            (*headers)[i]=(solar_os_http_header_t){in->headers[i].name,in->headers[i].value};
        }
    }
    out->url=in->url;out->headers=*headers;out->header_count=in->header_count;
    out->body=in->body;out->body_len=in->body_size;out->user_agent=in->user_agent;
    out->follow_redirects=(in->flags & SOLAR_OS_NATIVE_HTTP_FOLLOW_REDIRECTS)!=0;
    out->max_redirects=in->max_redirects;out->timeout_ms=in->timeout_ms;out->read_poll_ms=in->read_poll_ms;
    out->deadline_ms=in->deadline_ms;out->receive_buffer_size=in->receive_buffer_size;
    out->transmit_buffer_size=in->transmit_buffer_size;return SOLAR_OS_NATIVE_OK;
}
static void result_out(const solar_os_http_response_t *in,int error,solar_os_native_http_result_v1_t *out)
{
    memset(out,0,sizeof(*out));out->content_length=in->content_length;out->bytes_received=in->bytes_received;
    out->status=in->status_code;out->error=error;out->duration_ms=in->duration_ms;
    out->flags=((in->cancelled || error==SOLAR_OS_NATIVE_ERROR_CANCELLED)?SOLAR_OS_NATIVE_HTTP_CANCELLED:0)|
        (in->deadline_exceeded?SOLAR_OS_NATIVE_HTTP_DEADLINE:0);
}
typedef struct { solar_os_native_http_event_fn event;void *user;int error; } native_http_bridge_t;
static esp_err_t raw_event(const solar_os_http_event_t *in,void *user)
{
    native_http_bridge_t *bridge=user;
    if (!bridge->event) return ESP_OK;
    solar_os_native_http_borrowed_event_v1_t out={
        .header_name=in->header_name,.header_value=in->header_value,.data=in->data,
        .data_size=in->data_len,.content_length=in->content_length,.status=in->status_code};
    switch(in->type) {
    case SOLAR_OS_HTTP_EVENT_HEADER: out.type=SOLAR_OS_NATIVE_HTTP_HEADER;break;
    case SOLAR_OS_HTTP_EVENT_DATA: out.type=SOLAR_OS_NATIVE_HTTP_DATA;break;
    case SOLAR_OS_HTTP_EVENT_RESPONSE: out.type=SOLAR_OS_NATIVE_HTTP_RESPONSE;break;
    default: return ESP_ERR_INVALID_ARG;
    }
    int result=bridge->event(&out,bridge->user);
    if (result && !bridge->error) bridge->error=result;
    return result?ESP_FAIL:ESP_OK;
}
static int http_perform(solar_os_native_handle_t id,const solar_os_native_http_options_v1_t *in,
    solar_os_native_http_event_fn event,void *user,solar_os_native_http_result_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));out->status=-1;out->content_length=-1;
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_http_request_options_t options;solar_os_http_header_t *headers=NULL;
    int result=atomic_load(&state->closing)?SOLAR_OS_NATIVE_ERROR_INVALID_STATE:options_in(in,&options,&headers);
    solar_os_http_request_t *request=NULL;solar_os_http_response_t response={.status_code=-1,.content_length=-1};
    native_http_bridge_t bridge={event,user,0};
    if (!result) {
        options.should_cancel=http_cancelled;options.cancel_user_data=(void *)(uintptr_t)id;
        options.event_handler=raw_event;options.user_data=&bridge;
        result=http_result(state,solar_os_http_request_create(&options,&request));
        if (!result) result=http_result(state,solar_os_http_request_perform(request,&response));
        if (request) {
            int closed=solar_os_native_result(solar_os_http_request_destroy(request));
            if (!result) result=closed;
        }
    }
    if (bridge.error) result=bridge.error;
    result_out(&response,result,out);
    solar_os_memory_free(headers);solar_os_native_handle_release(id);return result;
}
static int response_destroy(void *value)
{
    native_http_response_t *response=value;
    solar_os_http_buffered_response_clear(&response->value);solar_os_memory_free(response);
    return SOLAR_OS_NATIVE_OK;
}
static int http_request(solar_os_native_handle_t id,uint32_t connection,
    const solar_os_native_http_options_v1_t *in,size_t max_body,solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_http_request_options_t options;solar_os_http_header_t *headers=NULL;
    int result=atomic_load(&state->closing)?SOLAR_OS_NATIVE_ERROR_INVALID_STATE:options_in(in,&options,&headers);
    native_http_response_t *response=NULL;
    if (!result) {
        response=solar_os_memory_calloc(1,sizeof(*response),SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.http.response");
        if (!response) result=SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
        else {
            result=solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_HTTP_RESPONSE,response,response_destroy,out);
            if (result) { response_destroy(response);response=NULL; }
        }
    }
    if (!result) {
        options.should_cancel=http_cancelled;options.cancel_user_data=(void *)(uintptr_t)id;
        esp_err_t error=connection?solar_os_http_session_request(state->connections,connection,&options,max_body,&response->value):
            solar_os_http_perform_buffered(&options,max_body,&response->value);
        result=http_result(state,error);response->error=result;
    }
    solar_os_memory_free(headers);solar_os_native_handle_release(id);return result;
}
static int response_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id,SOLAR_OS_NATIVE_HANDLE_HTTP_RESPONSE); }
static int response_info(solar_os_native_handle_t id,solar_os_native_http_result_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    native_http_response_t *response=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP_RESPONSE);
    if (!response) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    result_out(&response->value.response,response->error,out);
    out->body_size=response->value.body_len;out->header_count=response->value.header_count;
    out->flags|=(response->value.body_truncated?SOLAR_OS_NATIVE_HTTP_BODY_TRUNCATED:0)|
        (response->value.headers_truncated?SOLAR_OS_NATIVE_HTTP_HEADERS_TRUNCATED:0);
    solar_os_native_handle_release(id);return SOLAR_OS_NATIVE_OK;
}
static int response_read(solar_os_native_handle_t id,uint64_t offset,void *bytes,size_t capacity,size_t *size)
{
    if (!size || (!bytes && capacity) || capacity>65536) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size=0;
    native_http_response_t *response=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP_RESPONSE);
    if (!response) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (offset<response->value.body_len) {
        size_t available=response->value.body_len-(size_t)offset;
        *size=available<capacity?available:capacity;
        if (*size) memcpy(bytes,response->value.body+(size_t)offset,*size);
    }
    solar_os_native_handle_release(id);return SOLAR_OS_NATIVE_OK;
}
static int response_header(solar_os_native_handle_t id,uint32_t index,char *name,size_t name_capacity,
    char *value,size_t value_capacity,uint32_t *name_size,uint32_t *value_size)
{
    if (!name_size || !value_size || (!name && name_capacity) || (!value && value_capacity))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *name_size=0;*value_size=0;if (name_capacity) *name=0;if (value_capacity) *value=0;
    native_http_response_t *response=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP_RESPONSE);
    if (!response) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (index<response->value.header_count) {
        solar_os_http_response_header_t *header=&response->value.headers[index];
        *name_size=strlen(header->name)+1;*value_size=strlen(header->value)+1;
        result=SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        if (name_capacity>=*name_size && value_capacity>=*value_size) {
            memcpy(name,header->name,*name_size);memcpy(value,header->value,*value_size);result=SOLAR_OS_NATIVE_OK;
        }
    }
    solar_os_native_handle_release(id);return result;
}
static int connection_open(solar_os_native_handle_t id,const char *origin,const char *agent,uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=atomic_load(&state->closing)?SOLAR_OS_NATIVE_ERROR_INVALID_STATE:SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    if (!atomic_load(&state->closing)) for (size_t i=0;i<SOLAR_OS_HTTP_SESSION_MAX_HANDLES;++i) {
        if (state->connection_ids[i]) continue;
        result=http_result(state,solar_os_http_session_open(state->connections,origin,agent,out));
        if (*out) state->connection_ids[i]=*out;
        break;
    }
    solar_os_native_handle_release(id);return result;
}
static int connection_close(solar_os_native_handle_t id,uint32_t connection)
{
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_http_session_close(state->connections,connection));
    if (!result || result==SOLAR_OS_NATIVE_ERROR_NOT_FOUND)
        for (size_t i=0;i<SOLAR_OS_HTTP_SESSION_MAX_HANDLES;++i)
            if (state->connection_ids[i]==connection) state->connection_ids[i]=0;
    solar_os_native_handle_release(id);return result;
}
static int stream_open(solar_os_native_handle_t id,const solar_os_native_http_options_v1_t *in,uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_http_request_options_t options;solar_os_http_header_t *headers=NULL;
    int result=atomic_load(&state->closing)?SOLAR_OS_NATIVE_ERROR_INVALID_STATE:options_in(in,&options,&headers);
    if (!result) {
        result=SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
        for (size_t i=0;i<SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES;++i) {
            if (state->stream_ids[i]) continue;
            result=http_result(state,solar_os_http_stream_open(state->streams,&options,out));
            if (*out) state->stream_ids[i]=*out;
            break;
        }
    }
    solar_os_memory_free(headers);solar_os_native_handle_release(id);return result;
}
static int stream_read(solar_os_native_handle_t id,uint32_t stream,uint32_t timeout,solar_os_native_http_event_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_http_stream_event_t *value=solar_os_memory_alloc(sizeof(*value),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.http.event");
    int result=SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    if (value) {
        memset(value,0,sizeof(*value));
        result=http_result(state,solar_os_http_stream_read(state->streams,stream,timeout,value));
        if (!result) {
            switch(value->type) {
            case SOLAR_OS_HTTP_STREAM_EVENT_RESPONSE: out->type=SOLAR_OS_NATIVE_HTTP_RESPONSE;break;
            case SOLAR_OS_HTTP_STREAM_EVENT_HEADER: out->type=SOLAR_OS_NATIVE_HTTP_HEADER;break;
            case SOLAR_OS_HTTP_STREAM_EVENT_DATA: out->type=SOLAR_OS_NATIVE_HTTP_DATA;break;
            case SOLAR_OS_HTTP_STREAM_EVENT_COMPLETE: out->type=SOLAR_OS_NATIVE_HTTP_COMPLETE;break;
            case SOLAR_OS_HTTP_STREAM_EVENT_ERROR: out->type=SOLAR_OS_NATIVE_HTTP_ERROR;break;
            default: result=SOLAR_OS_NATIVE_ERROR_INVALID_STATE;break;
            }
            out->status=value->status_code;out->content_length=value->content_length;
            out->bytes_received=value->bytes_received;out->duration_ms=value->duration_ms;
            out->error=solar_os_native_result(value->error);
            out->flags=(value->cancelled?SOLAR_OS_NATIVE_HTTP_CANCELLED:0)|
                (value->deadline_exceeded?SOLAR_OS_NATIVE_HTTP_DEADLINE:0)|
                (value->truncated?(out->type==SOLAR_OS_NATIVE_HTTP_HEADER?
                    SOLAR_OS_NATIVE_HTTP_HEADERS_TRUNCATED:SOLAR_OS_NATIVE_HTTP_BODY_TRUNCATED):0);
            if (value->data_len>sizeof(out->data)) result=SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            else {
                out->data_size=value->data_len;memcpy(out->data,value->data,value->data_len);
                memcpy(out->header_name,value->header_name,sizeof(out->header_name));
                memcpy(out->header_value,value->header_value,sizeof(out->header_value));
            }
        }
        solar_os_memory_free(value);
    }
    solar_os_native_handle_release(id);return result;
}
static int stream_close(solar_os_native_handle_t id,uint32_t stream,uint32_t timeout)
{
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_http_stream_close_wait(state->streams,stream,timeout));
    if (!result || result==SOLAR_OS_NATIVE_ERROR_NOT_FOUND)
        for (size_t i=0;i<SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES;++i)
            if (state->stream_ids[i]==stream) state->stream_ids[i]=0;
    solar_os_native_handle_release(id);return result;
}
static int http_close_all(solar_os_native_handle_t id,uint32_t timeout)
{
    native_http_context_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=http_drain(state,timeout);solar_os_native_handle_release(id);return result;
}
static int origin_in(const solar_os_native_http_origin_v1_t *in,solar_os_http_origin_t *out)
{
    if (!in || !memchr(in->host,0,sizeof(in->host)) || in->port>UINT16_MAX || in->tls>1)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memcpy(out->host,in->host,sizeof(out->host));out->port=in->port;out->tls=in->tls!=0;
    return SOLAR_OS_NATIVE_OK;
}
static int origin_parse(const char *url,solar_os_native_http_origin_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));solar_os_http_origin_t value={0};
    if (!solar_os_http_origin_parse(url,&value)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memcpy(out->host,value.host,sizeof(out->host));out->port=value.port;out->tls=value.tls;return SOLAR_OS_NATIVE_OK;
}
static bool same_origin(const solar_os_native_http_origin_v1_t *a,const solar_os_native_http_origin_v1_t *b)
{
    solar_os_http_origin_t first,second;
    return !origin_in(a,&first) && !origin_in(b,&second) && solar_os_http_same_origin(&first,&second);
}
static int redirect_url(const char *base,const char *location,char *out,size_t capacity)
{
    if (!base || !location || !out || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;return solar_os_http_redirect_url(base,location,out,capacity)?SOLAR_OS_NATIVE_OK:SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
static bool redirect_status(int32_t status) { return solar_os_http_redirect_status(status); }
#if SOLAR_OS_PACKAGE_SERVICE_DOWNLOAD
static int http_download(solar_os_native_handle_t id, const solar_os_native_http_options_v1_t *in,
    const char *path, size_t max_size, size_t expected_size, const char *expected_sha256,
    solar_os_native_download_progress_fn progress, void *user, solar_os_native_download_result_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->response.status = -1;
    out->response.content_length = -1;
    native_http_context_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_HTTP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;

    solar_os_http_request_options_t options;
    solar_os_http_header_t *headers = NULL;
    int result = atomic_load(&state->closing) ? SOLAR_OS_NATIVE_ERROR_INVALID_STATE : options_in(in, &options, &headers);
    if (!result) {
        options.should_cancel = http_cancelled;
        options.cancel_user_data = (void *)(uintptr_t)id;
        solar_os_download_result_t value = { .response = {.status_code = -1, .content_length = -1} };
        result = http_result(state, solar_os_download_file(&options, path, max_size, expected_size,
            expected_sha256, progress, user, &value));
        out->bytes = value.bytes;
        if (!result) memcpy(out->sha256, value.sha256, sizeof(out->sha256));
        result_out(&value.response, result, &out->response);
    } else {
        out->response.error = result;
    }
    solar_os_memory_free(headers);
    solar_os_native_handle_release(id);
    return result;
}
#endif

static const solar_os_native_http_api_v1_t http_api={
    .abi_version=SOLAR_OS_NATIVE_HTTP_ABI,.struct_size=sizeof(http_api),
    .create=http_create,.close=http_close,.cancel=http_cancel,.limits=http_limits,.method_parse=http_method_parse,
    .perform=http_perform,.request=http_request,.response_close=response_close,.response_info=response_info,
    .response_read=response_read,.response_header=response_header,.connection_open=connection_open,.connection_close=connection_close,
    .stream_open=stream_open,.stream_read=stream_read,.stream_close=stream_close,.close_all=http_close_all,
    .origin_parse=origin_parse,.same_origin=same_origin,.redirect_status=redirect_status,.redirect_url=redirect_url,
#if SOLAR_OS_PACKAGE_SERVICE_DOWNLOAD
    .download = http_download,
#endif
};
#endif
const void *solar_os_native_http_get_service(const char *name,uint32_t abi,uint32_t minimum)
{
#include "generated/native_http_dispatch.inc"
    (void)name;(void)abi;(void)minimum;return NULL;
}
