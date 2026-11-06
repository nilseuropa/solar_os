#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_module_packages.h"
#define MODULE_CATALOG_MAX_BYTES 32768
#define MODULE_SIGNATURE_MAX_BYTES 512
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 1
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int module_catalog_cache_lock;
static struct { size_t count; char ids[16][32]; } module_catalog_id_cache;
static bool fail_write, fail_publish, installed;
static unsigned allocations, signature_checks, parses;
size_t strlcpy(char *d,const char *s,size_t n) { size_t len=strlen(s);if(n)snprintf(d,n,"%s",s);return len; }
static void *solar_os_memory_alloc(size_t n,int cls,const char *tag) { (void)cls;(void)tag;void *p=malloc(n);if(p)allocations++;return p; }
static void *solar_os_memory_calloc(size_t n,size_t size,int cls,const char *tag) { void *p=solar_os_memory_alloc(n*size,cls,tag);if(p)memset(p,0,n*size);return p; }
static void solar_os_memory_free(void *p) { if(p){allocations--;free(p);} }
static void module_set_detail(char *d,size_t n,const char *s) { if(d && n)strlcpy(d,s,n); }
static esp_err_t module_release_url(char *out,size_t n) { strlcpy(out,"https://example.invalid/4.17.6",n);return ESP_OK; }
/* Keep the cache test independent of the signature algorithm. These adapters
 * enforce that verification precedes parsing and that host checks run on reload. */
static esp_err_t module_verify_catalog_signature(const void *body,size_t n,const char *sig)
{ (void)body;(void)n;signature_checks++;return !strcmp(sig,"valid")?ESP_OK:ESP_ERR_INVALID_CRC; }
static esp_err_t module_parse_catalog(const void *body,size_t n,const char *url,solar_os_module_catalog_t *out)
{ (void)url;parses++;if(n!=7 || memcmp(body,"current",7))return ESP_ERR_NOT_SUPPORTED;out->count=1;strcpy(out->packages[0].id,"robot");out->packages[0].installed=installed;return ESP_OK; }
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
    assert(module_catalog_save("current",7,"valid",5)==ESP_OK);
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK);
    assert(out->signature_verified && out->count==1 && !out->packages[0].installed);
    assert(signature_checks==1 && parses==1 && module_catalog_id_cache.count==1);
    solar_os_memory_free(out);assert(!allocations);
    installed=true;
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK && out->packages[0].installed);
    solar_os_memory_free(out);
    fail_write=true;assert(module_catalog_save("broken",6,"invalid",7)==ESP_FAIL);fail_write=false;
    fail_publish=true;assert(module_catalog_save("broken",6,"invalid",7)==ESP_FAIL);fail_publish=false;
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK);solar_os_memory_free(out);
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
    assert(solar_os_module_catalog_load(&out,detail,sizeof(detail))==ESP_OK);solar_os_memory_free(out);
    assert(!allocations);
    puts("pkg cache: offline reload, verification, host checks, installed freshness and failed save preservation passed");
}
