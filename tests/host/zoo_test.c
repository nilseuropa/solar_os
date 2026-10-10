/* Real zoo and ZIP services, with host storage/HTTP/crypto adapters.
 * No model is instantiated: these tests verify publication and install safety. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <zlib.h>
#include "cJSON.h"
#include "miniz.h"
#include "nvs.h"
#include "solar_os_zoo.h"
#include "solar_os_crypto.h"
#include "solar_os_http_client.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "freertos/task.h"

static const char *mount_root, *server, *mode;
static char custom_source[320];
static volatile bool cancel;
static size_t allocations;
void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t c, const char *tag)
{ (void)c; (void)tag; void *p = malloc(n); if (p) ++allocations; return p; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t c, const char *tag)
{ void *p = solar_os_memory_alloc(n*size, c, tag); if (p) memset(p, 0, n*size); return p; }
void *solar_os_memory_realloc(void *p, size_t n, solar_os_memory_class_t c, const char *tag)
{ if (!p) return solar_os_memory_alloc(n,c,tag); return realloc(p,n); }
void solar_os_memory_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
size_t strlcpy(char *out, const char *s, size_t n)
{ size_t len = strlen(s); if (n) { size_t copy = len < n-1 ? len : n-1; memcpy(out,s,copy); out[copy] = 0; } return len; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
static void *json_alloc(size_t n) { return solar_os_memory_alloc(n,0,""); }
struct solar_os_json_doc { cJSON *root; };
esp_err_t solar_os_json_init(void) { cJSON_Hooks h = {json_alloc, solar_os_memory_free}; cJSON_InitHooks(&h); return 0; }
esp_err_t solar_os_json_parse(const char *s,size_t n,solar_os_json_doc_t **out)
{ cJSON *root = cJSON_ParseWithLength(s,n); if (!root) return ESP_ERR_INVALID_ARG; *out=json_alloc(sizeof(**out)); (*out)->root=root; return 0; }
const solar_os_json_value_t *solar_os_json_root(const solar_os_json_doc_t *d) { return d?d->root:NULL; }
void solar_os_json_free(solar_os_json_doc_t *d) { if(d){ cJSON_Delete(d->root); solar_os_memory_free(d); } }
bool solar_os_storage_sd_is_mounted(void) { return true; }
bool solar_os_storage_flash_is_mounted(void) { return false; }
const char *solar_os_storage_sd_mount_point(void) { return mount_root; }
const char *solar_os_storage_flash_mount_point(void) { return mount_root; }
esp_err_t solar_os_storage_path_mount_point(const char *path,char *out,size_t size)
{ (void)path; return strlcpy(out,mount_root,size)>=size?ESP_ERR_INVALID_SIZE:ESP_OK; }
esp_err_t solar_os_storage_join_path(const char *base,const char *name,char *out,size_t size)
{ int n=snprintf(out,size,"%s/%s",base,name); return n<0||(size_t)n>=size?ESP_ERR_INVALID_SIZE:0; }
esp_err_t solar_os_storage_sibling_path(const char *base,const char *suffix,char *out,size_t size)
{ int n=snprintf(out,size,"%s%s",base,suffix); return n<0||(size_t)n>=size?ESP_ERR_INVALID_SIZE:0; }
esp_err_t solar_os_storage_makedirs(const char *path,bool ok)
{ (void)ok; char copy[160]; strlcpy(copy,path,sizeof(copy)); for(char *s=copy+1;*s;s++)if(*s=='/'){*s=0;if(mkdir(copy,0755)&&errno!=EEXIST)return ESP_FAIL;*s='/';} return mkdir(copy,0755)&&errno!=EEXIST?ESP_FAIL:0; }
esp_err_t solar_os_storage_sync_file(FILE *f) { return fflush(f)||fsync(fileno(f))?ESP_FAIL:0; }
esp_err_t solar_os_storage_write_file(const char *path,const void *data,size_t n,bool append)
{ if(n>65536)return ESP_ERR_INVALID_SIZE; FILE *f=fopen(path,append?"ab":"wb");if(!f)return ESP_FAIL;esp_err_t e=fwrite(data,1,n,f)==n?solar_os_storage_sync_file(f):ESP_FAIL;fclose(f);return e; }
esp_err_t solar_os_storage_get_usage_for_path(const char *p,solar_os_storage_usage_t *usage)
{ (void)p;usage->free_bytes=!strcmp(mode,"no-space")?1:128*1024*1024;return 0; }
esp_err_t solar_os_storage_replace_file(const char *new_path,const char *active,const char *backup)
{ unlink(backup); bool old=!rename(active,backup); if(rename(new_path,active)){if(old)rename(backup,active);return ESP_FAIL;} unlink(backup);return 0; }
int __real_rename(const char *, const char *);
int __wrap_rename(const char *a, const char *b)
{ if(!strcmp(mode,"swap-failure")&&strstr(a,".stage")){errno=EIO;return -1;}return __real_rename(a,b); }
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (!strcmp(mode, "extract-manifest-corruption") && size && count) {
        char link[64], path[512];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fileno(file));
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = 0;
            if (strstr(path, ".stage/bundle.json")) {
                size_t bytes = size * count;
                unsigned char *changed = malloc(bytes); assert(changed);
                memcpy(changed, data, bytes); changed[0] ^= 1;
                size_t written = __real_fwrite(changed, size, count, file);
                free(changed); return written;
            }
        }
    }
    return __real_fwrite(data, size, count, file);
}
esp_err_t nvs_open(const char *name,nvs_open_mode_t m,nvs_handle_t *h) { (void)name;(void)m;*h=1;return 0; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *s,size_t *n)
{ (void)h;if(strcmp(key,"source")||!*custom_source)return ESP_ERR_NOT_FOUND;if(strlen(custom_source)>=*n)return ESP_ERR_INVALID_SIZE;strcpy(s,custom_source);return 0; }
esp_err_t nvs_set_str(nvs_handle_t h,const char *key,const char *value)
{ (void)h;if(!strcmp(key,"source"))strlcpy(custom_source,value,sizeof(custom_source));return 0; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;return 0; }
void nvs_close(nvs_handle_t h) { (void)h; }
void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *s) { memset(s,0,sizeof(*s)); }
void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *s) { EVP_MD_CTX_free(s->ctx.pointer); }
esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *s)
{ s->ctx.pointer=EVP_MD_CTX_new();return EVP_DigestInit_ex(s->ctx.pointer,EVP_sha256(),NULL)?0:ESP_FAIL; }
esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *s,const void *p,size_t n)
{return EVP_DigestUpdate(s->ctx.pointer,p,n)?0:ESP_FAIL;}
esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *s,uint8_t out[32])
{return EVP_DigestFinal_ex(s->ctx.pointer,out,NULL)?0:ESP_FAIL;}
esp_err_t solar_os_crypto_sha256_once(const void *p,size_t n,uint8_t out[32])
{solar_os_crypto_sha256_t s;solar_os_crypto_sha256_init(&s);solar_os_crypto_sha256_start(&s);solar_os_crypto_sha256_update(&s,p,n);solar_os_crypto_sha256_finish(&s,out);solar_os_crypto_sha256_free(&s);return 0;}
bool solar_os_crypto_sha256_hex_is_valid(const char *s)
{if(!s||strlen(s)!=64)return false;for(size_t i=0;i<64;i++)if(!strchr("0123456789abcdef",s[i]))return false;return true;}
bool solar_os_crypto_sha256_matches_hex(const uint8_t d[32],const char *s)
{char out[65];for(size_t i=0;i<32;i++)snprintf(out+2*i,3,"%02x",d[i]);return s&&!strcmp(out,s);}
struct solar_os_http_request { solar_os_http_request_options_t options; };
esp_err_t solar_os_http_request_create(const solar_os_http_request_options_t *o,solar_os_http_request_t **out)
{*out=calloc(1,sizeof(**out));(*out)->options=*o;return 0;}
esp_err_t solar_os_http_request_destroy(solar_os_http_request_t *r) {free(r);return 0;}
esp_err_t solar_os_http_request_perform(solar_os_http_request_t *r,solar_os_http_response_t *out)
{
    const char *name=strrchr(r->options.url,'/');assert(name);char path[512];snprintf(path,sizeof(path),"%s/%s",server,name+1);
    FILE *f=fopen(path,"rb");if(!f)return ESP_ERR_NOT_FOUND;
    out->status_code=!strcmp(mode,"http-error")?404:200;esp_err_t error=0;uint8_t buffer[1024];size_t n;
    /* Match ESP-IDF: header callbacks precede final status assignment. */
    solar_os_http_event_t header={.type=SOLAR_OS_HTTP_EVENT_HEADER,.status_code=-1,
        .header_name="Content-Type",.header_value="application/octet-stream"};
    error=r->options.event_handler(&header,r->options.user_data);
    solar_os_http_event_t response={.type=SOLAR_OS_HTTP_EVENT_RESPONSE,.status_code=out->status_code};
    if(!error)error=r->options.event_handler(&response,r->options.user_data);
    const size_t fragments[] = {37, 509, 3, 1023, 7}; size_t chunk = 0;
    while(!error && (n=fread(buffer,1,!strcmp(mode,"fragmented") ?
        fragments[chunk++ % (sizeof(fragments)/sizeof(fragments[0]))] : sizeof(buffer),f))){
        solar_os_http_event_t e={.type=SOLAR_OS_HTTP_EVENT_DATA,.status_code=out->status_code,.data=buffer,.data_len=n};
        if(!strcmp(mode,"cancel-download")&&strstr(name,".zip"))cancel=true;
        if(r->options.cancel_flag&&*r->options.cancel_flag){error=ESP_ERR_TIMEOUT;break;}
        error=r->options.event_handler(&e,r->options.user_data);
        memset(buffer, 0xa5, sizeof(buffer)); /* Transport storage is borrowed. */
    }fclose(f);return error;
}
/* Stored ZIP paths use the real service. Deflate is deliberately unsupported in
 * this host adapter, matching the zoo's stored-only publication contract. */
mz_ulong mz_crc32(mz_ulong crc,const unsigned char *p,size_t n) { return crc32(crc,p,n); }
tinfl_status tinfl_decompress(tinfl_decompressor *r,const mz_uint8 *i,size_t *n,mz_uint8 *start,mz_uint8 *o,size_t *size,mz_uint32 flags)
{(void)r;(void)i;(void)n;(void)start;(void)o;(void)size;(void)flags;return TINFL_STATUS_FAILED;}
int main(int argc,char **argv)
{
    assert(argc==4);mount_root=argv[1];server=argv[2];mode=argv[3];
    solar_os_zoo_t *z=NULL; assert(!solar_os_zoo_open(&z));
    esp_err_t cached=solar_os_zoo_reload(z);
    esp_err_t error=solar_os_zoo_refresh(z,&cancel,NULL,NULL);
    if(!error&&!strcmp(mode,"source-change")) {solar_os_zoo_set_source("https://other.example/catalog.json");solar_os_zoo_close(z);assert(!solar_os_zoo_open(&z));error=solar_os_zoo_reload(z);}
    else if(!error&&strcmp(mode,"refresh-only")) error=solar_os_zoo_install(z,0,&cancel,NULL,NULL);
    size_t count=solar_os_zoo_count(z);bool installed=count&&solar_os_zoo_installed(z,0);
    printf("{\"error\":%d,\"cached\":%d,\"count\":%zu,\"installed\":%s}\n",error,cached,count,installed?"true":"false");
    solar_os_zoo_close(z);assert(!allocations);return 0;
}
