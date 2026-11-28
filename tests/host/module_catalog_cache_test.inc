#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_http_client.h"
#include "solar_os_module_packages.h"
#define SOLAR_OS_VERSION "1.2.3"
#define SOLAR_OS_MODULE_REPOSITORY_URL "https://example.invalid"
/* LIMITS */
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 1
#define SOLAR_OS_LOGW(...) ((void)0)
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int module_catalog_cache_lock;
static struct { size_t count; char ids[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX][32]; } module_catalog_id_cache;
static bool fail_write, fail_publish, installed;
static unsigned allocations, signature_checks, parses;
size_t strlcpy(char *d,const char *s,size_t n) { size_t len=strlen(s);if(n)snprintf(d,n,"%s",s);return len; }
static void *solar_os_memory_alloc(size_t n,int cls,const char *tag) { (void)cls;(void)tag;void *p=malloc(n);if(p)allocations++;return p; }
static void *solar_os_memory_calloc(size_t n,size_t size,int cls,const char *tag) { void *p=solar_os_memory_alloc(n*size,cls,tag);if(p)memset(p,0,n*size);return p; }
static void solar_os_memory_free(void *p) { if(p){allocations--;free(p);} }
static void module_set_detail(char *d,size_t n,const char *s) { if(d && n)strlcpy(d,s,n); }
esp_err_t solar_os_http_perform_buffered(const solar_os_http_request_options_t *options,
    size_t max_body_bytes, solar_os_http_buffered_response_t *response)
{
    if (max_body_bytes > SOLAR_OS_HTTP_BUFFERED_MAX_BODY) return ESP_ERR_INVALID_ARG;
    const char *body = "current";
    if (strcmp(options->url, SOLAR_OS_MODULE_REPOSITORY_URL "/" SOLAR_OS_VERSION "/catalog.json")) {
        assert(!strcmp(options->url, SOLAR_OS_MODULE_REPOSITORY_URL "/" SOLAR_OS_VERSION "/catalog.sig"));
        body = "valid";
    }
    response->body_len = strlen(body);
    assert(max_body_bytes > response->body_len);
    response->body = malloc(max_body_bytes);
    assert(response->body);
    memcpy(response->body, body, response->body_len);
    response->response.status_code = 200;
    return ESP_OK;
}

void solar_os_http_buffered_response_clear(solar_os_http_buffered_response_t *response)
{
    free(response->body);
}
/* Keep the cache test independent of the signature algorithm. These adapters
 * enforce that verification precedes parsing and that host checks run on reload. */
static esp_err_t module_verify_catalog_signature(const void *body,size_t n,const char *sig)
{ (void)body;(void)n;signature_checks++;return !strcmp(sig,"valid")?ESP_OK:ESP_ERR_INVALID_CRC; }
static esp_err_t module_parse_catalog(const void *body,size_t n,const char *url,solar_os_module_catalog_t *out)
{ (void)url;parses++;if(n!=7 || memcmp(body,"current",7))return ESP_ERR_NOT_SUPPORTED;out->packages=solar_os_memory_calloc(1,sizeof(*out->packages),1,"entries");if(!out->packages)return ESP_ERR_NO_MEM;out->count=1;strcpy(out->packages[0].id,"robot");out->packages[0].installed=installed;return ESP_OK; }
esp_err_t solar_os_storage_default_path(const char *r,char *out,size_t n) { return snprintf(out,n,"%s",r)<(int)n?ESP_OK:ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_makedirs(const char *p,bool ok)
{ (void)ok;(void)mkdir("modules",0700);return mkdir(p,0700)==0 || access(p,F_OK)==0?ESP_OK:ESP_FAIL; }
esp_err_t solar_os_storage_write_file(const char *p,const void *data,size_t n,bool append)
{ assert(!append);if(fail_write)return ESP_FAIL;FILE *f=fopen(p,"wb");if(!f)return ESP_FAIL;bool good=fwrite(data,1,n,f)==n;return fclose(f)==0 && good?ESP_OK:ESP_FAIL; }
esp_err_t solar_os_storage_read_file(const char *p,void *data,size_t n,size_t *read)
{ FILE *f=fopen(p,"rb");if(!f)return ESP_ERR_NOT_FOUND;*read=fread(data,1,n,f);bool ok=!ferror(f);return fclose(f)==0 && ok?ESP_OK:ESP_FAIL; }
esp_err_t solar_os_storage_stat(const char *p,solar_os_storage_metadata_t *m)
{ struct stat s;if(stat(p,&s))return ESP_ERR_NOT_FOUND;m->size_bytes=s.st_size;m->type=S_ISREG(s.st_mode)?SOLAR_OS_STORAGE_ENTRY_FILE:SOLAR_OS_STORAGE_ENTRY_DIRECTORY;return ESP_OK; }
esp_err_t solar_os_storage_replace_files(const solar_os_storage_replacement_t *items,size_t n)
{ if(fail_publish)return ESP_FAIL;for(size_t i=0;i<n;i++){(void)remove(items[i].backup_path);(void)rename(items[i].active_path,items[i].backup_path);assert(!rename(items[i].staged_path,items[i].active_path));(void)remove(items[i].backup_path);}return ESP_OK; }

/* CACHE */

int main(void) {
    solar_os_module_catalog_t *out=(void*)1;char detail[128];
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_ERR_NOT_FOUND && !out && !allocations);
    assert(module_catalog_fetch(&out,NULL,NULL,detail,sizeof(detail))==ESP_OK && !detail[0]);
    assert(out->signature_verified && out->count==1 && !out->packages[0].installed);
    solar_os_module_catalog_free(out);
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK);
    assert(out->signature_verified && out->count==1 && !out->packages[0].installed);
    assert(signature_checks==2 && parses==2 && module_catalog_id_cache.count==1);
    solar_os_module_catalog_free(out);assert(!allocations);
    installed=true;
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK && out->packages[0].installed);
    solar_os_module_catalog_free(out);
    fail_write=true;assert(module_catalog_save("broken",6,"invalid",7)==ESP_FAIL);fail_write=false;
    fail_publish=true;assert(module_catalog_save("broken",6,"invalid",7)==ESP_FAIL);fail_publish=false;
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK);solar_os_module_catalog_free(out);
    assert(module_catalog_save("current",7,"invalid",7)==ESP_OK);
    unsigned old_parses=parses;
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_ERR_INVALID_CRC && !out && parses==old_parses);
    assert(module_catalog_save("outdated",8,"valid",5)==ESP_OK);
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_ERR_NOT_SUPPORTED && !out);
    char oversized[MODULE_SIGNATURE_MAX_BYTES+1];memset(oversized,'a',sizeof(oversized));
    assert(module_catalog_save("current",7,oversized,sizeof(oversized))==ESP_OK);
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_ERR_INVALID_SIZE && !out);
    assert(!allocations);
    assert(!atomic_flag_test_and_set(&module_catalog_file_busy));
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_ERR_INVALID_STATE && !out);
    assert(module_catalog_save("current",7,"valid",5)==ESP_ERR_INVALID_STATE);
    atomic_flag_clear(&module_catalog_file_busy);
    assert(module_catalog_save("current",7,"valid",5)==ESP_OK);
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK);solar_os_module_catalog_free(out);
    assert(!allocations);
    puts("pkg catalog: HTTP download, offline reload, verification, host checks, installed freshness and failed save preservation passed");
}
