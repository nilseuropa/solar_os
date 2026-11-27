#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "solar_os_native_client.h"
#include "solar_os_native_core.h"
#include "solar_os_crypto.h"
#include "solar_os_memory.h"
#include "solar_os_port.h"
#include "solar_os_time.h"
/* Include the production registry so the exhaustion boundary can be exercised
 * without manufacturing billions of tokens or changing the public contract. */
#include "solar_os_native_handles.c"

static unsigned allocations, releases;
static bool fail_allocation, fail_port_close, claimed, block_read, reading, proceed;
static pthread_mutex_t test_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_condition=PTHREAD_COND_INITIALIZER;
static solar_os_port_serial_config_t serial={115200,8,0,0,true,false};
static solar_os_datetime_t current={2026,11,24,13,14,15,2,true};

void *solar_os_memory_alloc(size_t bytes,solar_os_memory_class_t cls,const char *owner)
{
    assert(cls==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && owner);
    if(fail_allocation) return NULL;
    void *value=malloc(bytes);if(value)++allocations;return value;
}
void *solar_os_memory_calloc(size_t count,size_t bytes,solar_os_memory_class_t cls,const char *owner)
{
    void *value=solar_os_memory_alloc(count*bytes,cls,owner);
    if(value)memset(value,0,count*bytes);
    return value;
}
void solar_os_memory_free(void *value) { if(value){assert(allocations);--allocations;}free(value); }

/* An independent host SHA provider tests forwarding and object lifetime. The
 * production adapter still calls the existing firmware crypto service. */
void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *sha) { memset(sha,0,sizeof(*sha)); }
void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *sha)
{ EVP_MD_CTX_free(sha->ctx.pointer);sha->ctx.pointer=NULL;sha->started=false; }
esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *sha)
{
    if(!sha->ctx.pointer)sha->ctx.pointer=EVP_MD_CTX_new();
    sha->started=sha->ctx.pointer && EVP_DigestInit_ex(sha->ctx.pointer,EVP_sha256(),NULL);
    return sha->started?ESP_OK:ESP_FAIL;
}
esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *sha,const void *data,size_t size)
{
    if(!sha->started)return ESP_ERR_INVALID_STATE;
    return EVP_DigestUpdate(sha->ctx.pointer,data,size)?ESP_OK:ESP_FAIL;
}
esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *sha,uint8_t out[32])
{
    if(!sha->started)return ESP_ERR_INVALID_STATE;
    unsigned length=0;int result=EVP_DigestFinal_ex(sha->ctx.pointer,out,&length);
    sha->started=false;return result && length==32?ESP_OK:ESP_FAIL;
}
esp_err_t solar_os_crypto_sha256_once(const void *data,size_t size,uint8_t out[32])
{
    solar_os_crypto_sha256_t sha;solar_os_crypto_sha256_init(&sha);
    esp_err_t error=solar_os_crypto_sha256_start(&sha);
    if(!error)error=solar_os_crypto_sha256_update(&sha,data,size);
    if(!error)error=solar_os_crypto_sha256_finish(&sha,out);
    solar_os_crypto_sha256_free(&sha);return error;
}
bool solar_os_crypto_sha256_hex_is_valid(const char *hex)
{
    if(!hex || strlen(hex)!=64)return false;
    for(unsigned i=0;i<64;++i)if(!isxdigit((unsigned char)hex[i]))return false;
    return true;
}
esp_err_t solar_os_crypto_bytes_to_hex(const uint8_t *data,size_t size,char *hex,size_t capacity)
{
    if(!data || !hex || capacity<size*2+1)return ESP_ERR_INVALID_ARG;
    for(size_t i=0;i<size;++i)sprintf(hex+i*2,"%02x",data[i]);
    return ESP_OK;
}
esp_err_t solar_os_crypto_hex_to_bytes(const char *hex,uint8_t *bytes,size_t size)
{
    if(!hex || !bytes || strlen(hex)!=size*2)return ESP_ERR_INVALID_ARG;
    for(size_t i=0;i<size;++i) { unsigned value=0;if(sscanf(hex+i*2,"%2x",&value)!=1)return ESP_ERR_INVALID_ARG;bytes[i]=value; }
    return ESP_OK;
}
bool solar_os_crypto_sha256_matches_hex(const uint8_t digest[32],const char *hex)
{ char actual[65];return digest && solar_os_crypto_sha256_hex_is_valid(hex) &&
    !solar_os_crypto_bytes_to_hex(digest,32,actual,sizeof(actual)) && !strcmp(actual,hex); }
esp_err_t solar_os_crypto_random(uint8_t *out,size_t size)
{ if(!out && size)return ESP_ERR_INVALID_ARG;if(size)memset(out,0xa5,size);return ESP_OK; }
esp_err_t solar_os_crypto_rng_init(solar_os_crypto_rng_t *rng,const char *text)
{ (void)text;memset(rng,0,sizeof(*rng));rng->seeded=true;return ESP_OK; }
void solar_os_crypto_rng_free(solar_os_crypto_rng_t *rng) { rng->seeded=false; }
int solar_os_crypto_rng_mbedtls(void *value,unsigned char *out,size_t size)
{ solar_os_crypto_rng_t *rng=value;assert(rng->seeded && size<=MBEDTLS_CTR_DRBG_MAX_REQUEST);return solar_os_crypto_random(out,size); }
esp_err_t solar_os_crypto_base64_decode(const char *text,uint8_t *out,size_t capacity,size_t *size)
{
    if(strcmp(text,"YWJj"))return ESP_ERR_INVALID_ARG;
    *size=3;if(capacity<3)return ESP_ERR_INVALID_SIZE;
    memcpy(out,"abc",3);return ESP_OK;
}
esp_err_t solar_os_crypto_ecdsa_p256_sha256_verify_pem(const char *pem,const void *data,
    size_t size,const uint8_t *signature,size_t signature_size)
{
    assert(!strcmp(pem,"fixture-key") && size==3 && !memcmp(data,"abc",3));
    return signature_size==1 && signature[0]==0x42?ESP_OK:ESP_ERR_INVALID_CRC;
}

esp_err_t solar_os_port_claim(const char *name,const char *owner,solar_os_port_handle_t *out)
{
    assert(owner && *owner);if(strcmp(name,"serial0"))return ESP_ERR_NOT_FOUND;
    if(claimed)return ESP_ERR_INVALID_STATE;
    claimed=true;*out=(solar_os_port_handle_t){0,123};return ESP_OK;
}
esp_err_t solar_os_port_release(solar_os_port_handle_t *port)
{
    assert(port->token==123 && claimed);++releases;
    if(fail_port_close)return ESP_ERR_TIMEOUT;
    port->token=0;claimed=false;return ESP_OK;
}
bool solar_os_port_handle_valid(const solar_os_port_handle_t *port)
{ return port && port->token==123 && claimed; }
esp_err_t solar_os_port_get_info(const char *name,solar_os_port_info_t *info)
{
    if(strcmp(name,"serial0"))return ESP_ERR_NOT_FOUND;
    memset(info,0,sizeof(*info));strcpy(info->name,"serial0");strcpy(info->label,"test port");
    info->capabilities=SOLAR_OS_PORT_CAP_READ|SOLAR_OS_PORT_CAP_WRITE|SOLAR_OS_PORT_CAP_CONFIG|SOLAR_OS_PORT_CAP_SERIAL;
    info->claimed=claimed;return ESP_OK;
}
const char *solar_os_port_capabilities_text(uint32_t capabilities, char *buffer, size_t capacity)
{
    (void)capabilities;
    if (capacity) buffer[0] = '\0';
    return buffer;
}
size_t solar_os_port_list(solar_os_port_info_t *ports,size_t capacity)
{ if(capacity)assert(!solar_os_port_get_info("serial0",ports));return 1; }
esp_err_t solar_os_port_read(const solar_os_port_handle_t *port,uint8_t *data,size_t capacity,
    uint32_t timeout,size_t *size)
{
    assert(solar_os_port_handle_valid(port) && timeout==37);
    pthread_mutex_lock(&test_lock);reading=true;pthread_cond_broadcast(&test_condition);
    while(block_read && !proceed)pthread_cond_wait(&test_condition,&test_lock);
    pthread_mutex_unlock(&test_lock);
    *size=capacity<2?capacity:2;if(*size)memcpy(data,"OK",*size);return ESP_OK;
}
esp_err_t solar_os_port_write(const solar_os_port_handle_t *port,const uint8_t *data,size_t size,size_t *written)
{ assert(solar_os_port_handle_valid(port) && size==2 && !memcmp(data,"hi",2));*written=size;return ESP_OK; }
esp_err_t solar_os_port_serial_get(const solar_os_port_handle_t *port,solar_os_port_serial_config_t *out)
{ assert(solar_os_port_handle_valid(port));*out=serial;return ESP_OK; }
esp_err_t solar_os_port_serial_configure(const solar_os_port_handle_t *port,const solar_os_port_serial_config_t *value,uint32_t mask)
{
    assert(solar_os_port_handle_valid(port) && mask==SOLAR_OS_SERIAL_BAUD);
    serial.baud_rate=value->baud_rate;return ESP_OK;
}
esp_err_t solar_os_port_serial_break(const solar_os_port_handle_t *port,uint16_t ms)
{ assert(solar_os_port_handle_valid(port) && ms==100);return ESP_OK; }

bool solar_os_time_has_provider(void) { return true; }
int solar_os_native_clock_provider_register(const solar_os_native_clock_provider_v1_t *in,
                                             solar_os_native_handle_t *out)
{
    assert(in && out && !strcmp(in->name, "fixture"));
    *out = 123;
    return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_clock_provider_close(solar_os_native_handle_t handle)
{
    assert(handle == 123);
    return SOLAR_OS_NATIVE_OK;
}
uint64_t solar_os_time_uptime_ms(void) { return UINT64_C(9876543210); }
void solar_os_time_format_uptime(uint64_t value,char *text,size_t capacity)
{ snprintf(text,capacity,"%llu",(unsigned long long)value); }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *out) { *out=UINT64_C(1795526055000);return ESP_OK; }
esp_err_t solar_os_time_get_datetime(solar_os_datetime_t *out) { *out=current;out->hour+=1;return ESP_OK; }
esp_err_t solar_os_time_get_utc_datetime(solar_os_datetime_t *out) { *out=current;return ESP_OK; }
esp_err_t solar_os_time_set_utc_datetime(const solar_os_datetime_t *value) { current=*value;return ESP_OK; }
esp_err_t solar_os_time_set_datetime(const solar_os_datetime_t *value) { current=*value;--current.hour;return ESP_OK; }
esp_err_t solar_os_time_utc_to_local(const solar_os_datetime_t *in,solar_os_datetime_t *out)
{ *out=*in;++out->hour;return ESP_OK; }
esp_err_t solar_os_time_local_to_utc(const solar_os_datetime_t *in,solar_os_datetime_t *out)
{ *out=*in;--out->hour;return ESP_OK; }
bool solar_os_time_datetime_is_valid(const solar_os_datetime_t *value)
{ return value && value->year>=1970 && value->month>=1 && value->month<=12 && value->hour<24; }
void solar_os_time_get_timezone(char *name,size_t names,char *posix,size_t positions)
{ snprintf(name,names,"Europe/Berlin");snprintf(posix,positions,"CET-1"); }
esp_err_t solar_os_time_set_timezone(const char *value) { return !strcmp(value,"UTC")?ESP_OK:ESP_ERR_INVALID_ARG; }
esp_err_t solar_os_time_ntp_sync(const char *server,uint32_t timeout,solar_os_datetime_t *utc,solar_os_datetime_t *local)
{ assert(!strcmp(server,"fixture") && timeout==400);*utc=current;*local=current;++local->hour;return ESP_OK; }

static const solar_os_native_ports_api_v1_t *ports;
static void *read_worker(void *argument)
{
    solar_os_native_handle_t id=*(solar_os_native_handle_t *)argument;
    uint8_t text[4];size_t size=0;assert(!ports->read(id,text,sizeof(text),37,&size));
    assert(size==2 && !memcmp(text,"OK",2));return NULL;
}
static void test_ports(void)
{
    solar_os_native_port_info_v1_t info[16];size_t count=0;
    assert(!ports->list(info,16,&count) && count==1 && !strcmp(info[0].name,"serial0"));
    assert(info[0].capabilities==(SOLAR_OS_NATIVE_PORT_READ|SOLAR_OS_NATIVE_PORT_WRITE|SOLAR_OS_NATIVE_PORT_CONFIG|SOLAR_OS_NATIVE_PORT_SERIAL));
    solar_os_native_handle_t id=99;
    fail_allocation=true;assert(ports->claim("serial0","test",&id)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !id);
    fail_allocation=false;assert(!ports->claim("serial0","test",&id) && id && claimed);
    assert(ports->valid(id));size_t written=0;
    const solar_os_native_crypto_api_v1_t *crypto=solar_os_native_core_get_service("crypto",1,sizeof(*crypto));
    assert(crypto->close(id)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && ports->valid(id));
    assert(!ports->write(id,(const uint8_t *)"hi",2,&written) && written==2);
    solar_os_native_serial_config_v1_t config;
    assert(!ports->serial_get(id,&config) && config.baud_rate==115200 && config.dtr==1);
    config.baud_rate=9600;config.parity=UINT32_MAX;
    assert(!ports->serial_set(id,&config,SOLAR_OS_NATIVE_SERIAL_BAUD));
    assert(serial.baud_rate==9600 && serial.data_bits==8);
    assert(ports->serial_set(id,&config,SOLAR_OS_NATIVE_SERIAL_PARITY)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!ports->serial_break(id,100));
    pthread_mutex_lock(&test_lock);block_read=true;pthread_mutex_unlock(&test_lock);
    pthread_t worker;assert(!pthread_create(&worker,NULL,read_worker,&id));
    pthread_mutex_lock(&test_lock);while(!reading)pthread_cond_wait(&test_condition,&test_lock);
    pthread_mutex_unlock(&test_lock);
    unsigned before=releases;
    assert(ports->close(id)==SOLAR_OS_NATIVE_ERROR_BUSY && releases==before);
    pthread_mutex_lock(&test_lock);proceed=true;pthread_cond_broadcast(&test_condition);pthread_mutex_unlock(&test_lock);
    assert(!pthread_join(worker,NULL));
    fail_port_close=true;
    assert(ports->close(id)==SOLAR_OS_NATIVE_ERROR_TIMEOUT && ports->valid(id));
    fail_port_close=false;assert(!ports->close(id) && !claimed);
    assert(ports->close(id)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !ports->valid(id));
    assert(!allocations && !solar_os_native_handle_count());
}
static void test_crypto(void)
{
    const solar_os_native_crypto_api_v1_t *api=solar_os_native_core_get_service("crypto",1,sizeof(*api));
    assert(api && api->hash_create(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    uint32_t hash=123;
    fail_allocation=true;assert(api->hash_create(&hash)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !hash);
    fail_allocation=false;assert(!api->hash_create(&hash) && hash);
    assert(api->hash_update(hash,"a",1)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->hash_start(hash));
    assert(!api->hash_update(hash,"a",1) && !api->hash_update(hash,"bc",2));
    uint8_t digest[32],other[32];assert(!api->hash_finish(hash,digest));
    assert(api->hash_finish(hash,other)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    static const char expected[]="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    assert(api->hash_matches_hex(digest,expected));
    assert(!api->hash_once("abc",3,other) && !memcmp(digest,other,32));
    char hex[65];assert(!api->bytes_to_hex(digest,32,hex,sizeof(hex)) && !strcmp(hex,expected));
    assert(api->hash_hex_valid(hex) && !api->hash_hex_valid("bad"));
    assert(!api->hex_to_bytes(hex,other,32) && !memcmp(digest,other,32));
    assert(!api->close(hash));assert(api->close(hash)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    uint32_t rng=0;assert(!api->random_create(NULL,&rng) && rng!=hash);
    assert(api->hash_start(rng)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->hash_start(hash)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    uint8_t data[2049];assert(!api->random_read(rng,data,sizeof(data)) && data[0]==0xa5 && data[2048]==0xa5);
    assert(!api->close(rng));
    assert(!api->random(data,sizeof(data)) && data[2048]==0xa5);
    size_t size=0;assert(api->base64_decode("YWJj",NULL,0,&size)==SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && size==3);
    assert(!api->base64_decode("YWJj",data,sizeof(data),&size) && size==3 && !memcmp(data,"abc",3));
    api->wipe(data + 1, 3);
    assert(data[0] == 'a' && data[1] == 0 && data[2] == 0 && data[3] == 0 && data[4] == 0xa5);
    uint8_t signature=0x42;
    assert(!api->verify_p256_sha256("fixture-key","abc",3,&signature,1));
    signature=0;
    assert(api->verify_p256_sha256("fixture-key","abc",3,&signature,1)==SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!allocations && !solar_os_native_handle_count());
}
static void test_clock(void)
{
    const solar_os_native_clock_api_v1_t *clock=solar_os_native_core_get_service("clock",1,sizeof(*clock));
    assert(clock && clock->has_provider() && clock->uptime_ms()==UINT64_C(9876543210));
    uint64_t epoch=0;assert(!clock->epoch_ms(&epoch) && epoch==UINT64_C(1795526055000));
    solar_os_native_datetime_v1_t utc,local;
    assert(!clock->get(SOLAR_OS_NATIVE_CLOCK_UTC,&utc) && utc.year==2026 && utc.hour==13);
    assert(!clock->get(SOLAR_OS_NATIVE_CLOCK_LOCAL,&local) && local.hour==14);
    assert(clock->valid(&utc));utc.year=UINT32_MAX;assert(!clock->valid(&utc));
    assert(clock->set(SOLAR_OS_NATIVE_CLOCK_UTC,&utc)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!clock->get(SOLAR_OS_NATIVE_CLOCK_UTC,&utc));
    assert(!clock->convert(SOLAR_OS_NATIVE_CLOCK_LOCAL,&utc,&local) && local.hour==utc.hour+1);
    assert(!clock->convert(SOLAR_OS_NATIVE_CLOCK_UTC,&local,&local) && local.hour==utc.hour);
    solar_os_native_timezone_v1_t zone;assert(!clock->get_timezone(&zone) && !strcmp(zone.name,"Europe/Berlin"));
    assert(clock->resolve_timezone("UTC+5:30", zone.name, sizeof(zone.name), zone.posix, sizeof(zone.posix)));
    assert(!strcmp(zone.name, "UTC+5:30") && !strcmp(zone.posix, "UTC-5:30"));
    assert(clock->resolve_timezone("Berlin", zone.name, sizeof(zone.name), zone.posix, sizeof(zone.posix)));
    assert(!strcmp(zone.name, "Europe/Berlin") && clock->timezone_is_raw_posix(zone.posix));
    assert(!clock->timezone_is_raw_posix("Europe/Berlin"));
    assert(!clock->resolve_timezone("UTC+26", zone.name, sizeof(zone.name), zone.posix, sizeof(zone.posix)));
    assert(!clock->resolve_timezone("UTC", zone.name, 1, zone.posix, sizeof(zone.posix)));
    assert(!clock->set_timezone("UTC"));
    assert(clock->set_timezone("unknown")==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!clock->ntp_sync("fixture",400,&utc,&local) && local.hour==utc.hour+1);
    assert(clock->get(2,&utc)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(clock->epoch_ms(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    const solar_os_native_clock_provider_v1_t provider = {.name = "fixture"};
    solar_os_native_handle_t handle;
    assert(!clock->provider_register(&provider, &handle));
    assert(!clock->provider_close(handle));
}
static int dummy_destroy(void *value)
{
    unsigned *counter=value;++*counter;
    /* Destruction runs outside the registry lock. */
    assert(solar_os_native_handle_count()>0);
    return SOLAR_OS_NATIVE_OK;
}
static bool destroying, finish_destroy;
static unsigned resource_owner;
static int owned_destroy(void *state)
{
    /* The owner must remain queryable after the object has been freed and
     * while another task is still completing its destructor. */
    free(state);
    pthread_mutex_lock(&test_lock);
    destroying=true;pthread_cond_broadcast(&test_condition);
    while(!finish_destroy)pthread_cond_wait(&test_condition,&test_lock);
    pthread_mutex_unlock(&test_lock);
    return SOLAR_OS_NATIVE_OK;
}
static void *close_worker(void *argument)
{
    const solar_os_native_handle_t id=*(solar_os_native_handle_t *)argument;
    assert(!solar_os_native_handle_close_owned(id,199,&resource_owner));return NULL;
}
static void test_owner_during_concurrent_close(void)
{
    void *state=malloc(8);assert(state);
    solar_os_native_handle_t id;
    assert(!solar_os_native_handle_create_owned(199,state,owned_destroy,&resource_owner,&id));
    pthread_t worker;assert(!pthread_create(&worker,NULL,close_worker,&id));
    pthread_mutex_lock(&test_lock);
    while(!destroying)pthread_cond_wait(&test_condition,&test_lock);
    pthread_mutex_unlock(&test_lock);
    assert(solar_os_native_handle_close_owner(&resource_owner)==SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!solar_os_native_handle_acquire_owned(id,199,&resource_owner));
    void *borrowed;
    assert(solar_os_native_handle_acquire_exclusive(id, 199, &borrowed)==SOLAR_OS_NATIVE_ERROR_BUSY && !borrowed);
    solar_os_native_handle_release(id); /* A stray release cannot clear closing. */
    assert(solar_os_native_handle_close_owner(&resource_owner)==SOLAR_OS_NATIVE_ERROR_BUSY);
    pthread_mutex_lock(&test_lock);finish_destroy=true;pthread_cond_broadcast(&test_condition);
    pthread_mutex_unlock(&test_lock);
    assert(!pthread_join(worker,NULL) && !solar_os_native_handle_count());
    assert(!solar_os_native_handle_close_owner(&resource_owner));
}
static void test_registry_capacity_and_exhaustion(void)
{
    uint32_t ids[NATIVE_HANDLES_MAX];unsigned destroyed=0;
    for(unsigned i=0;i<NATIVE_HANDLES_MAX;++i)
        assert(!solar_os_native_handle_create(199,&destroyed,dummy_destroy,&ids[i]));
    uint32_t extra=123;
    assert(ports->claim("serial0","capacity",&extra)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !extra && !claimed);
    assert(!allocations);
    assert(solar_os_native_handle_acquire(ids[0],200)==NULL);
    assert(solar_os_native_handle_acquire(ids[0],199)==&destroyed);
    assert(solar_os_native_handle_close(ids[0])==SOLAR_OS_NATIVE_ERROR_BUSY);
    void *borrowed;
    assert(solar_os_native_handle_acquire_exclusive(ids[0], 199, &borrowed)==SOLAR_OS_NATIVE_ERROR_BUSY && !borrowed);
    solar_os_native_handle_release(ids[0]);
    assert(!solar_os_native_handle_acquire_exclusive(ids[0], 199, &borrowed) && borrowed==&destroyed);
    assert(!solar_os_native_handle_acquire(ids[0], 199));
    assert(solar_os_native_handle_acquire_exclusive(ids[0], 199, &borrowed)==SOLAR_OS_NATIVE_ERROR_BUSY && !borrowed);
    assert(solar_os_native_handle_acquire_exclusive(ids[0], 200, &borrowed)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !borrowed);
    solar_os_native_handle_release(ids[0]);
    assert(solar_os_native_handle_close(ids[0])==SOLAR_OS_NATIVE_ERROR_BUSY);
    solar_os_native_handle_release_exclusive(ids[0]);
    native_handle_slot_t *slot=find_handle(ids[0]);
    slot->reference_state=NATIVE_HANDLE_REFERENCE_MAX;
    assert(!solar_os_native_handle_acquire(ids[0],199));
    solar_os_native_handle_release(ids[0]);
    assert(solar_os_native_handle_acquire(ids[0],199)==&destroyed);
    assert(slot->reference_state==NATIVE_HANDLE_REFERENCE_MAX);
    slot->reference_state=0;
    for(unsigned i=0;i<NATIVE_HANDLES_MAX;++i)assert(!solar_os_native_handle_close(ids[i]));
    assert(destroyed==NATIVE_HANDLES_MAX && !solar_os_native_handle_count());
    next_handle=UINT32_MAX-1;
    assert(!solar_os_native_handle_create(199,&destroyed,dummy_destroy,&extra) && extra==UINT32_MAX);
    assert(!solar_os_native_handle_close(extra));
    assert(solar_os_native_handle_create(199,&destroyed,dummy_destroy,&extra)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !extra);
}

int main(void)
{
    ports=solar_os_native_core_get_service("io.ports",1,sizeof(*ports));assert(ports);
    assert(!solar_os_native_core_get_service(NULL,1,8));
    assert(!solar_os_native_core_get_service("io.ports",2,8));
    test_ports();
    test_crypto();test_clock();test_owner_during_concurrent_close();test_registry_capacity_and_exhaustion();
    puts("native core API ownership passed");
    return 0;
}
