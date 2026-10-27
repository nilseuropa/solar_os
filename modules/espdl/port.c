/* Private ESP32-S3 runtime. Provider calls are serialized; kernel workers use
 * synchronized allocation/resource tracking and clear TLS before joining. */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include "solar_os_native_library_abi.h"
#include "solar_os_native_thread_abi.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_memory.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const solar_os_native_memory_api_v1_t *memory;
static const solar_os_native_thread_api_v1_t *threads;
static const solar_os_native_time_api_v1_t *clock_api;
static const solar_os_native_libc_api_v1_t *libc_api;
const solar_os_native_thread_api_v1_t *solar_os_espdl_port_threads(void) { return threads; }
int solar_os_espdl_port_bind(const solar_os_native_library_host_api_v1_t *host)
{
    memory = host->get_service("memory", 1, sizeof(*memory));
    threads = host->get_service("thread", 1, sizeof(*threads));
    clock_api = host->get_service("time", 1, sizeof(*clock_api));
    libc_api = host->get_service("libc", 1, sizeof(*libc_api));
    return memory && threads && clock_api && libc_api ? 0 : -6;
}

struct allocation {
    struct allocation *next, *previous;
    void *base;
    size_t size;
};
static struct allocation *allocations;
static unsigned allocation_lock;
static void lock_allocations(void)
{ while (__atomic_exchange_n(&allocation_lock,1,__ATOMIC_ACQUIRE)) clock_api->sleep_ms(1); }
static void unlock_allocations(void) { __atomic_store_n(&allocation_lock,0,__ATOMIC_RELEASE); }
static void *allocate(size_t bytes, size_t alignment, bool internal)
{
    if (alignment < 16) alignment = 16;
    if ((alignment & (alignment - 1)) || alignment > 4096 ||
        bytes > SIZE_MAX - alignment - sizeof(struct allocation)) return NULL;
    const size_t total = bytes + alignment + sizeof(struct allocation);
    void *base = internal ? memory->alloc_internal(total) : memory->alloc(total);
    if (!base) return NULL;
    const uintptr_t value = ((uintptr_t)base + sizeof(struct allocation) + alignment - 1) & ~(alignment - 1);
    struct allocation *record = (struct allocation *)value - 1;
    lock_allocations();
    *record = (struct allocation){allocations, NULL, base, bytes};
    if (allocations) allocations->previous = record;
    allocations = record;
    unlock_allocations();
    return (void *)value;
}
void *malloc(size_t n) { return allocate(n, 16, false); }
void *solar_os_memory_dl_alloc(size_t bytes) { return allocate(bytes, 16, false); }
size_t solar_os_memory_dl_free_bytes(bool external) { return memory->free_bytes(external); }
void free(void *buffer)
{
    if (!buffer) return;
    struct allocation *record = (struct allocation *)buffer - 1;
    lock_allocations();
    if (record->previous) record->previous->next = record->next;
    else allocations = record->next;
    if (record->next) record->next->previous = record->previous;
    unlock_allocations();
    memory->free(record->base);
}
void *calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) return NULL;
    void *buffer = malloc(n * size);
    if (buffer) memset(buffer, 0, n * size);
    return buffer;
}
void *realloc(void *old, size_t size)
{
    if (!old) return malloc(size);
    if (!size) { free(old); return NULL; }
    void *buffer = malloc(size);
    if (buffer) {
        const size_t before = ((struct allocation *)old - 1)->size;
        memcpy(buffer, old, size < before ? size : before);
        free(old);
    }
    return buffer;
}
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{ return allocate(size, alignment, (caps & MALLOC_CAP_INTERNAL) != 0); }
void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)
{
    if (size && n > SIZE_MAX / size) return NULL;
    void *buffer = heap_caps_aligned_alloc(alignment, n * size, caps);
    if (buffer) memset(buffer, 0, n * size);
    return buffer;
}
void heap_caps_free(void *buffer) { free(buffer); }
size_t heap_caps_get_free_size(uint32_t caps)
{ return memory->free_bytes((caps & MALLOC_CAP_SPIRAM) != 0); }
size_t heap_caps_get_largest_free_block(uint32_t caps)
{ return memory->largest_free_bytes((caps & MALLOC_CAP_SPIRAM) != 0); }
bool esp_ptr_external_ram(const void *pointer) { return memory->is_external(pointer); }

/* DTO/model-file allocations belong directly to the host and may outlive this
 * library. Runtime allocations above are private, with no escaped pointers. */
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)tag; return cls == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED ? memory->alloc(size) : NULL; }
void solar_os_memory_free(void *buffer) { memory->free(buffer); }
int64_t esp_timer_get_time(void) { return clock_api->now_us(); }
void vTaskDelay(TickType_t ticks)
{ clock_api->sleep_ms(((uint64_t)ticks*1000+configTICK_RATE_HZ-1)/configTICK_RATE_HZ); }

void *memchr(const void *buffer, int ch, size_t size)
{ const unsigned char *p = buffer; for (size_t i=0; i<size; ++i) if (p[i] == (unsigned char)ch) return (void *)(p+i); return NULL; }
char *strchr(const char *p, int ch)
{ for (;; ++p) { if (*p == (char)ch) return (char *)p; if (!*p) return NULL; } }
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a-(unsigned char)*b; }
size_t strlcpy(char *dst, const char *src, size_t n)
{ size_t bytes = strlen(src); if (n) { size_t take=bytes<n-1?bytes:n-1; memcpy(dst,src,take); dst[take]=0; } return bytes; }
static int private_errno;
int *__errno(void) { return &private_errno; }
char *getenv(const char *name) { (void)name; return NULL; }
float strtof(const char *text, char **end) { return libc_api->parse_float(text,end); }
double floor(double value) { return libc_api->floor_value(value); }
float expf(float value) { return libc_api->exp_float(value); }
float floorf(float value) { return libc_api->floor_float(value); }
float fmodf(float value,float divisor) { return libc_api->mod_float(value,divisor); }
float logf(float value) { return libc_api->log_float(value); }
float powf(float value,float exponent) { return libc_api->pow_float(value,exponent); }
float sqrtf(float value) { return libc_api->sqrt_float(value); }
int vsnprintf(char *dst, size_t n, const char *fmt, va_list args)
{ return libc_api->format(dst,n,fmt,args); }
int snprintf(char *dst, size_t n, const char *fmt, ...)
{ va_list args; va_start(args,fmt); int count=vsnprintf(dst,n,fmt,args); va_end(args); return count; }
/* Logging is disabled in this profile; numerical/status results use the C API. */
int printf(const char *format, ...) { (void)format; return 0; }
int putchar(int ch) { return ch; }
uint32_t esp_log_timestamp(void) { return clock_api->now_us()/1000; }
void esp_log(esp_log_config_t config, const char *tag, const char *format, ...)
{ (void)config; (void)tag; (void)format; }
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; }

/* These tables own host synchronization resources until private runtime teardown.
 * No provider worker tasks are created by this single-core profile. */
static void *mutexes[32], *conditions[8];
static uint32_t keys[8];
static bool key_active[8];
static void (*key_destructors[8])(void *);
static int remember(void **slots, size_t count, void *value)
{
    lock_allocations();
    int result=ENOMEM;
    for (size_t i=0;i<count;++i) if (!slots[i]) {slots[i]=value;result=0;break;}
    unlock_allocations();return result;
}
static void forget(void **slots, size_t count, void *value)
{
    lock_allocations();
    for (size_t i=0;i<count;++i) if (slots[i]==value) {slots[i]=NULL;break;}
    unlock_allocations();
}
static unsigned lazy_lock;
static void lock_lazy(void)
{ while (__atomic_exchange_n(&lazy_lock,1,__ATOMIC_ACQUIRE)) clock_api->sleep_ms(1); }
static void unlock_lazy(void) { __atomic_store_n(&lazy_lock,0,__ATOMIC_RELEASE); }
int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{ memset(attr,0,sizeof(*attr));attr->is_initialized=1;attr->type=PTHREAD_MUTEX_NORMAL;return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{ if (type!=PTHREAD_MUTEX_NORMAL && type!=PTHREAD_MUTEX_RECURSIVE) return EINVAL; attr->type=type;return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) { attr->is_initialized=0;return 0; }
int pthread_mutex_init(pthread_mutex_t *slot, const pthread_mutexattr_t *attr)
{
    void *mutex=NULL;
    int result=threads->mutex_create(&mutex,attr && attr->type==PTHREAD_MUTEX_RECURSIVE);
    if (!result && (result=remember(mutexes,32,mutex))) threads->mutex_destroy(mutex);
    if (!result) *slot=(uintptr_t)mutex;
    return result;
}
int pthread_mutex_lock(pthread_mutex_t *slot)
{
    lock_lazy();
    int r=0;
    if (*slot==0 || *slot==UINT32_MAX) r=pthread_mutex_init(slot,NULL);
    unlock_lazy();if(r)return r;
    return threads->mutex_lock((void *)(uintptr_t)*slot);
}
int pthread_mutex_unlock(pthread_mutex_t *slot) { return threads->mutex_unlock((void *)(uintptr_t)*slot); }
int pthread_mutex_destroy(pthread_mutex_t *slot)
{
    void *mutex=(void *)(uintptr_t)*slot;int result=threads->mutex_destroy(mutex);
    if (!result) {forget(mutexes,32,mutex);*slot=UINT32_MAX;} return result;
}
int pthread_cond_init(pthread_cond_t *slot, const pthread_condattr_t *attr)
{
    if (attr) return ENOTSUP;
    void *condition=NULL;int result=threads->condition_create(&condition);
    if (!result && (result=remember(conditions,8,condition))) threads->condition_destroy(condition);
    if (!result) *slot=(uintptr_t)condition;return result;
}
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    lock_lazy();int r=0;
    if (*condition==0 || *condition==UINT32_MAX) r=pthread_cond_init(condition,NULL);
    unlock_lazy();if(r)return r;
    return threads->condition_wait((void *)(uintptr_t)*condition,(void *)(uintptr_t)*mutex);
}
int pthread_cond_broadcast(pthread_cond_t *condition)
{ return *condition==UINT32_MAX ? 0 : threads->condition_broadcast((void *)(uintptr_t)*condition); }
int pthread_cond_destroy(pthread_cond_t *slot)
{
    void *condition=(void *)(uintptr_t)*slot;int result=threads->condition_destroy(condition);
    if (!result) {forget(conditions,8,condition);*slot=UINT32_MAX;} return result;
}
int pthread_once(pthread_once_t *once, void (*function)(void))
{
    if (!once || !function || !once->is_initialized) return EINVAL;
    int expected=0;
    if (__atomic_compare_exchange_n(&once->init_executed,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        function(); __atomic_store_n(&once->init_executed,2,__ATOMIC_RELEASE);
    } else while (__atomic_load_n(&once->init_executed,__ATOMIC_ACQUIRE)!=2) clock_api->sleep_ms(1);
    return 0;
}
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    lock_allocations();
    for (size_t i=0;i<8;++i) if (!key_active[i]) {
        uint32_t value;int result=threads->key_create(&value,destructor);
        if (!result) {keys[i]=value;key_active[i]=true;key_destructors[i]=destructor;*key=value;}
        unlock_allocations();return result;
    }
    unlock_allocations();return ENOMEM;
}
int pthread_key_delete(pthread_key_t key)
{
    /* IDF key deletion leaves per-task values behind; clear ours first. */
    int clear=threads->key_set(key,NULL);
    if (clear) return clear;
    int result=threads->key_delete(key);
    if (!result) for (size_t i=0;i<8;++i) if (key_active[i] && keys[i]==key) key_active[i]=false;
    return result;
}
void *pthread_getspecific(pthread_key_t key) { return threads->key_get(key); }
int pthread_setspecific(pthread_key_t key, const void *value) { return threads->key_set(key,value); }
void solar_os_espdl_port_leave_task(void)
{
    /* No C++ exception crosses the provider table. End all private TLS state on
     * the task that created it, before returning to a potentially long-lived
     * shell/client task. This also prevents IDF's key reuse from seeing old data. */
    for(size_t i=0;i<8;++i) if(key_active[i]) {
        void *value=threads->key_get(keys[i]);
        if(value && !threads->key_set(keys[i],NULL) && key_destructors[i])
            key_destructors[i](value);
    }
}

void *__dso_handle=&__dso_handle;
struct destructor { void (*function)(void *); void *argument, *dso; };
static struct destructor destructors[128];
static unsigned destructor_count;
int __cxa_atexit(void (*function)(void *), void *argument, void *dso)
{ if (destructor_count==128) return -1;destructors[destructor_count++]=(struct destructor){function,argument,dso};return 0; }
void __cxa_finalize(void *dso)
{ for(unsigned i=destructor_count;i>0;--i) {struct destructor *d=&destructors[i-1];if(d->function && (!dso || dso==d->dso)) {void(*fn)(void*)=d->function;d->function=NULL;fn(d->argument);}} }
void solar_os_espdl_port_cleanup(void)
{
    solar_os_espdl_port_leave_task();
    for(size_t i=0;i<8;++i) if(key_active[i]) {threads->key_delete(keys[i]);key_active[i]=false;}
    for(size_t i=0;i<8;++i) if(conditions[i]) {threads->condition_destroy(conditions[i]);conditions[i]=NULL;}
    for(size_t i=0;i<32;++i) if(mutexes[i]) {threads->mutex_destroy(mutexes[i]);mutexes[i]=NULL;}
    /* Private libstdc++ emergency pools and current-task exception globals have
     * no escaped pointers. Key deletion precedes release of their storage. */
    while(allocations) {struct allocation *record=allocations;allocations=record->next;memory->free(record->base);}
}
__attribute__((noreturn)) void abort(void) { __builtin_trap(); }
__attribute__((noreturn)) void __assert_func(const char *file,int line,const char *fn,const char *text)
{ (void)file;(void)line;(void)fn;(void)text;abort(); }
__attribute__((noreturn)) void _esp_error_check_failed(int result,const char*file,int line,const char*fn,const char*expression)
{ (void)result;(void)file;(void)line;(void)fn;(void)expression;abort(); }

/* FbsLoader's file/partition/crypto methods share archive objects with model
 * support. This provider only accepts validated, unencrypted memory buffers.
 * The adapter never invokes these unavailable model-source operations. */
FILE *fopen(const char *path,const char *mode) { (void)path;(void)mode;private_errno=ENOTSUP;return NULL; }
size_t fread(void *data,size_t size,size_t count,FILE *file)
{ (void)data;(void)size;(void)count;(void)file;private_errno=ENOTSUP;return 0; }
int fclose(FILE *file) { (void)file;return EOF; }
int fseek(FILE *file,long offset,int origin) { (void)file;(void)offset;(void)origin;return -1; }
/* Signatures intentionally belong only to this private link. Unreachable SDK
 * functions trap instead of fabricating successful partition/crypto results. */
#define UNAVAILABLE(name) void name(void) { abort(); }
UNAVAILABLE(esp_aes_init) UNAVAILABLE(esp_aes_free) UNAVAILABLE(esp_aes_setkey)
UNAVAILABLE(esp_aes_crypt_ctr) UNAVAILABLE(esp_fill_random)
UNAVAILABLE(mbedtls_sha256_init) UNAVAILABLE(mbedtls_sha256_free)
UNAVAILABLE(mbedtls_sha256_starts) UNAVAILABLE(mbedtls_sha256_update)
UNAVAILABLE(mbedtls_sha256_finish) UNAVAILABLE(esp_partition_find_first)
UNAVAILABLE(esp_partition_mmap) UNAVAILABLE(esp_partition_munmap)
UNAVAILABLE(spi_flash_mmap_get_free_pages) UNAVAILABLE(__xpg_strerror_r)
uint32_t arc4random(void) { abort(); }
unsigned long strtoul(const char *text,char **end,int base)
{ (void)text;(void)end;(void)base;abort(); }
int getentropy(void *buffer,size_t size) { (void)buffer;(void)size;private_errno=ENOTSUP;return -1; }
int read(int file,void *buffer,size_t size) { (void)file;(void)buffer;(void)size;private_errno=ENOTSUP;return -1; }
