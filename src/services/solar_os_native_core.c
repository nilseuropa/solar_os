#include "solar_os_native_core.h"
#include <string.h>
#include "solar_os_native_crypto_abi.h"
#include "solar_os_native_ports_abi.h"
#include "solar_os_native_clock_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_crypto.h"
#include "solar_os_memory.h"
#include "solar_os_port.h"
#include "solar_os_time.h"

#define CORE_BYTES_MAX 65536U
static int hash_destroy(void *state)
{
    solar_os_crypto_sha256_free(state);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}
static int random_destroy(void *state)
{
    solar_os_crypto_rng_free(state);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}
static int hash_create(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    solar_os_crypto_sha256_t *state=solar_os_memory_calloc(1,sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.sha256");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_crypto_sha256_init(state);
    int result=solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_SHA256,state,hash_destroy,out);
    if (result) hash_destroy(state);
    return result;
}
static int hash_start(solar_os_native_handle_t id)
{
    solar_os_crypto_sha256_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_SHA256);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_crypto_sha256_start(state));
    solar_os_native_handle_release(id);
    return result;
}
static int hash_update(solar_os_native_handle_t id, const void *bytes, size_t size)
{
    if (!bytes && size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_crypto_sha256_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_SHA256);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_crypto_sha256_update(state,bytes,size));
    solar_os_native_handle_release(id);
    return result;
}
static int hash_finish(solar_os_native_handle_t id, uint8_t digest[32])
{
    if (!digest) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_crypto_sha256_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_SHA256);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_crypto_sha256_finish(state,digest));
    solar_os_native_handle_release(id);
    return result;
}
static int hash_once(const void *bytes, size_t size, uint8_t digest[32])
{
    if ((!bytes && size) || !digest) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_crypto_sha256_once(bytes,size,digest));
}
static int crypto_close(solar_os_native_handle_t id)
{
    int result=solar_os_native_handle_close_kind(id,SOLAR_OS_NATIVE_HANDLE_SHA256);
    return result==SOLAR_OS_NATIVE_ERROR_NOT_FOUND ?
        solar_os_native_handle_close_kind(id,SOLAR_OS_NATIVE_HANDLE_RANDOM) : result;
}
static int random_bytes(uint8_t *bytes, size_t size)
{
    if (!bytes && size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_crypto_random(bytes,size));
}
static int random_create(const char *personalization, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    if (personalization && strlen(personalization)>128) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_crypto_rng_t *state=solar_os_memory_calloc(1,sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.random");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result=solar_os_native_result(solar_os_crypto_rng_init(state,personalization));
    if (!result) result=solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_RANDOM,state,random_destroy,out);
    if (result) random_destroy(state);
    return result;
}
static int random_read(solar_os_native_handle_t id, uint8_t *bytes, size_t size)
{
    if (!bytes && size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_crypto_rng_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_RANDOM);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=SOLAR_OS_NATIVE_OK;
    while (size && !result) {
        size_t chunk=size>MBEDTLS_CTR_DRBG_MAX_REQUEST?MBEDTLS_CTR_DRBG_MAX_REQUEST:size;
        if (solar_os_crypto_rng_mbedtls(state,bytes,chunk)) result=SOLAR_OS_NATIVE_ERROR_FAILED;
        else { bytes+=chunk;size-=chunk; }
    }
    solar_os_native_handle_release(id);
    return result;
}
static int bytes_to_hex(const uint8_t *bytes,size_t size,char *hex,size_t capacity)
{
    if (size>(SIZE_MAX-1)/2) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return solar_os_native_result(solar_os_crypto_bytes_to_hex(bytes,size,hex,capacity));
}
static int hex_to_bytes(const char *hex,uint8_t *bytes,size_t size)
{
    if (size>SIZE_MAX/2) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return solar_os_native_result(solar_os_crypto_hex_to_bytes(hex,bytes,size));
}
static int base64_decode(const char *text,uint8_t *bytes,size_t capacity,size_t *size)
{
    if (!text || !size)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size=0;
    return solar_os_native_result(solar_os_crypto_base64_decode(text,bytes,capacity,size));
}
static int verify_signature(const char *pem,const void *data,size_t size,
    const uint8_t *signature,size_t signature_size)
{
    if (!pem || (!data && size) || !signature || signature_size>96)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_crypto_ecdsa_p256_sha256_verify_pem(pem,data,size,signature,signature_size));
}
static const solar_os_native_crypto_api_v1_t crypto_api = {
    .abi_version=SOLAR_OS_NATIVE_CRYPTO_ABI,.struct_size=sizeof(crypto_api),
    .hash_create=hash_create,.hash_start=hash_start,.hash_update=hash_update,
    .hash_finish=hash_finish,.hash_once=hash_once,.close=crypto_close,
    .random=random_bytes,.random_create=random_create,.random_read=random_read,
    .hash_hex_valid=solar_os_crypto_sha256_hex_is_valid,.bytes_to_hex=bytes_to_hex,
    .hex_to_bytes=hex_to_bytes,.hash_matches_hex=solar_os_crypto_sha256_matches_hex,
    .base64_decode=base64_decode,.verify_p256_sha256=verify_signature,
};

static void port_info_copy(const solar_os_port_info_t *source,solar_os_native_port_info_v1_t *out)
{
    memset(out,0,sizeof(*out));
    memcpy(out->name,source->name,sizeof(out->name));
    memcpy(out->label,source->label,sizeof(out->label));
    memcpy(out->owner,source->owner,sizeof(out->owner));
    out->capabilities=
        ((source->capabilities & SOLAR_OS_PORT_CAP_READ) ? SOLAR_OS_NATIVE_PORT_READ : 0) |
        ((source->capabilities & SOLAR_OS_PORT_CAP_WRITE) ? SOLAR_OS_NATIVE_PORT_WRITE : 0) |
        ((source->capabilities & SOLAR_OS_PORT_CAP_CONFIG) ? SOLAR_OS_NATIVE_PORT_CONFIG : 0) |
        ((source->capabilities & SOLAR_OS_PORT_CAP_SERIAL) ? SOLAR_OS_NATIVE_PORT_SERIAL : 0) |
        ((source->capabilities & SOLAR_OS_PORT_CAP_USB_JTAG) ? SOLAR_OS_NATIVE_PORT_USB_JTAG : 0);
    out->claimed=source->claimed;out->operations=source->operations;
}
static int ports_list(solar_os_native_port_info_v1_t *ports,size_t capacity,size_t *count)
{
    if (!count || (!ports && capacity) || capacity>128) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_port_info_t local[SOLAR_OS_PORT_MAX];
    *count=solar_os_port_list(local,SOLAR_OS_PORT_MAX);
    size_t copied=*count<capacity ? *count : capacity;
    for (size_t i=0;i<copied;++i) port_info_copy(&local[i],&ports[i]);
    return SOLAR_OS_NATIVE_OK;
}
static int port_info(const char *name,solar_os_native_port_info_v1_t *info)
{
    if (!name || !info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info,0,sizeof(*info));solar_os_port_info_t value={0};
    int result=solar_os_native_result(solar_os_port_get_info(name,&value));
    if (!result) port_info_copy(&value,info);
    return result;
}
static int port_destroy(void *state)
{
    solar_os_port_handle_t *port=state;
    if (!port->token) { solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK; }
    int result=solar_os_native_result(solar_os_port_release(state));
    if (!result) solar_os_memory_free(state);
    return result;
}
static int port_claim(const char *name,const char *owner,solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    if (!name || !owner || !*owner || strlen(owner)>=SOLAR_OS_PORT_OWNER_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_port_handle_t *state=solar_os_memory_alloc(sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.port");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    *state=(solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;
    int result=solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_PORT,state,port_destroy,out);
    if (result) { solar_os_memory_free(state);return result; }
    result=solar_os_native_result(solar_os_port_claim(name,owner,state));
    if (result && !solar_os_native_handle_close_kind(*out,SOLAR_OS_NATIVE_HANDLE_PORT)) *out=0;
    return result;
}
static int port_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id,SOLAR_OS_NATIVE_HANDLE_PORT); }
static bool port_valid(solar_os_native_handle_t id)
{
    solar_os_port_handle_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_PORT);
    if (!state) return false;
    bool valid=solar_os_port_handle_valid(state);
    solar_os_native_handle_release(id);
    return valid;
}
static int port_read(solar_os_native_handle_t id,uint8_t *bytes,size_t capacity,uint32_t timeout,size_t *size)
{
    if (!size || (!bytes && capacity) || capacity>CORE_BYTES_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size=0;
    solar_os_port_handle_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_PORT);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_port_read(state,bytes,capacity,timeout,size));
    solar_os_native_handle_release(id);
    return result;
}
static int port_write(solar_os_native_handle_t id,const uint8_t *bytes,size_t size,size_t *written)
{
    if (!written || (!bytes && size) || size>CORE_BYTES_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *written=0;
    solar_os_port_handle_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_PORT);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_port_write(state,bytes,size,written));
    solar_os_native_handle_release(id);
    return result;
}
static int port_serial_get(solar_os_native_handle_t id,solar_os_native_serial_config_v1_t *config)
{
    if (!config) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(config,0,sizeof(*config));
    solar_os_port_handle_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_PORT);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_port_serial_config_t value={0};
    int result=solar_os_native_result(solar_os_port_serial_get(state,&value));
    if (!result) *config=(solar_os_native_serial_config_v1_t){
        value.baud_rate,value.data_bits,value.parity,value.stop_bits,value.dtr,value.rts};
    solar_os_native_handle_release(id);
    return result;
}
static int port_serial_set(solar_os_native_handle_t id,const solar_os_native_serial_config_v1_t *config,uint32_t mask)
{
    if (!config || mask & ~SOLAR_OS_NATIVE_SERIAL_ALL ||
        ((mask & SOLAR_OS_NATIVE_SERIAL_BITS) && (config->data_bits<5 || config->data_bits>8)) ||
        ((mask & SOLAR_OS_NATIVE_SERIAL_PARITY) && config->parity>4) ||
        ((mask & SOLAR_OS_NATIVE_SERIAL_STOP) && config->stop_bits>2) ||
        ((mask & SOLAR_OS_NATIVE_SERIAL_DTR) && config->dtr>1) ||
        ((mask & SOLAR_OS_NATIVE_SERIAL_RTS) && config->rts>1))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_port_handle_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_PORT);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_port_serial_config_t value={
        config->baud_rate,config->data_bits,config->parity,config->stop_bits,config->dtr!=0,config->rts!=0};
    uint32_t selected=
        ((mask & SOLAR_OS_NATIVE_SERIAL_BAUD) ? SOLAR_OS_SERIAL_BAUD : 0) |
        ((mask & SOLAR_OS_NATIVE_SERIAL_BITS) ? SOLAR_OS_SERIAL_BITS : 0) |
        ((mask & SOLAR_OS_NATIVE_SERIAL_PARITY) ? SOLAR_OS_SERIAL_PARITY : 0) |
        ((mask & SOLAR_OS_NATIVE_SERIAL_STOP) ? SOLAR_OS_SERIAL_STOP : 0) |
        ((mask & SOLAR_OS_NATIVE_SERIAL_DTR) ? SOLAR_OS_SERIAL_DTR : 0) |
        ((mask & SOLAR_OS_NATIVE_SERIAL_RTS) ? SOLAR_OS_SERIAL_RTS : 0);
    int result=solar_os_native_result(solar_os_port_serial_configure(state,&value,selected));
    solar_os_native_handle_release(id);
    return result;
}
static int port_serial_break(solar_os_native_handle_t id,uint16_t milliseconds)
{
    solar_os_port_handle_t *state=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_PORT);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result=solar_os_native_result(solar_os_port_serial_break(state,milliseconds));
    solar_os_native_handle_release(id);
    return result;
}
static const solar_os_native_ports_api_v1_t ports_api = {
    .abi_version=SOLAR_OS_NATIVE_PORTS_ABI,.struct_size=sizeof(ports_api),
    .list=ports_list,.info=port_info,.claim=port_claim,.close=port_close,.valid=port_valid,
    .read=port_read,.write=port_write,.serial_get=port_serial_get,.serial_set=port_serial_set,
    .serial_break=port_serial_break,
};

static bool datetime_in(const solar_os_native_datetime_v1_t *in,solar_os_datetime_t *out)
{
    if (!in || !out || in->year>UINT16_MAX || in->month>UINT8_MAX || in->day>UINT8_MAX ||
        in->hour>UINT8_MAX || in->minute>UINT8_MAX || in->second>UINT8_MAX ||
        in->weekday>UINT8_MAX || in->clock_integrity>1) return false;
    *out=(solar_os_datetime_t){.year=in->year,.month=in->month,.day=in->day,
        .hour=in->hour,.minute=in->minute,.second=in->second,.weekday=in->weekday,
        .clock_integrity=in->clock_integrity!=0};
    return true;
}
static void datetime_out(const solar_os_datetime_t *in,solar_os_native_datetime_v1_t *out)
{
    *out=(solar_os_native_datetime_v1_t){in->year,in->month,in->day,in->hour,
        in->minute,in->second,in->weekday,in->clock_integrity};
}
static int clock_get(uint32_t zone,solar_os_native_datetime_v1_t *out)
{
    if (!out || zone>SOLAR_OS_NATIVE_CLOCK_UTC) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));solar_os_datetime_t value={0};
    int result=solar_os_native_result(zone==SOLAR_OS_NATIVE_CLOCK_UTC ?
        solar_os_time_get_utc_datetime(&value) : solar_os_time_get_datetime(&value));
    if (!result) datetime_out(&value,out);
    return result;
}
static int clock_set(uint32_t zone,const solar_os_native_datetime_v1_t *in)
{
    solar_os_datetime_t value;
    if (zone>SOLAR_OS_NATIVE_CLOCK_UTC || !datetime_in(in,&value))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(zone==SOLAR_OS_NATIVE_CLOCK_UTC ?
        solar_os_time_set_utc_datetime(&value) : solar_os_time_set_datetime(&value));
}
static int clock_convert(uint32_t zone,const solar_os_native_datetime_v1_t *in,
    solar_os_native_datetime_v1_t *out)
{
    solar_os_datetime_t source,destination={0};
    if (!out || zone>SOLAR_OS_NATIVE_CLOCK_UTC || !datetime_in(in,&source))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int result=solar_os_native_result(zone==SOLAR_OS_NATIVE_CLOCK_UTC ?
        solar_os_time_local_to_utc(&source,&destination) : solar_os_time_utc_to_local(&source,&destination));
    memset(out,0,sizeof(*out));
    if (!result) datetime_out(&destination,out);
    return result;
}
static bool clock_valid(const solar_os_native_datetime_v1_t *in)
{
    solar_os_datetime_t value;
    return datetime_in(in,&value) && solar_os_time_datetime_is_valid(&value);
}
static int clock_timezone(solar_os_native_timezone_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    solar_os_time_get_timezone(out->name,sizeof(out->name),out->posix,sizeof(out->posix));
    return SOLAR_OS_NATIVE_OK;
}
static int clock_set_timezone(const char *name)
{
    if (!name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_time_set_timezone(name));
}
static int clock_ntp(const char *server,uint32_t timeout,solar_os_native_datetime_v1_t *utc,
    solar_os_native_datetime_v1_t *local)
{
    if (!utc || !local) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(utc,0,sizeof(*utc));memset(local,0,sizeof(*local));
    solar_os_datetime_t u={0},l={0};
    int result=solar_os_native_result(solar_os_time_ntp_sync(server,timeout,&u,&l));
    if (!result) { datetime_out(&u,utc);datetime_out(&l,local); }
    return result;
}
static int clock_epoch(uint64_t *milliseconds)
{
    if (!milliseconds) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *milliseconds=0;
    return solar_os_native_result(solar_os_time_get_utc_epoch_ms(milliseconds));
}
static const solar_os_native_clock_api_v1_t clock_api={
    .abi_version=SOLAR_OS_NATIVE_CLOCK_ABI,.struct_size=sizeof(clock_api),
    .has_provider=solar_os_time_has_provider,.uptime_ms=solar_os_time_uptime_ms,
    .format_uptime=solar_os_time_format_uptime,.epoch_ms=clock_epoch,.get=clock_get,.set=clock_set,
    .convert=clock_convert,.valid=clock_valid,.get_timezone=clock_timezone,
    .set_timezone=clock_set_timezone,.ntp_sync=clock_ntp,
};
const void *solar_os_native_core_get_service(const char *name,uint32_t abi,uint32_t minimum)
{
#include "generated/native_core_dispatch.inc"
    return NULL;
}
