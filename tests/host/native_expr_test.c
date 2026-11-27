#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_expr.h"
#include "solar_os_native_expr_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"

static unsigned live, allocations, callbacks;
static bool fail_allocation;
static solar_os_native_handle_t active;
static const solar_os_native_expr_api_v1_t *api;
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{
    assert(kind==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag,"native.expr"));
    if(fail_allocation) return NULL;
    void *p=calloc(n,size);if(p){++live;++allocations;}return p;
}
void solar_os_memory_free(void *p) { if(p){assert(live);--live;}free(p); }
static bool variable(void *user,const char *name,double *value)
{ assert(user==&callbacks && !strcmp(name,"a"));++callbacks;*value=3;return true; }
static bool function(void *user,const char *name,const double *args,uint32_t count,double *value)
{
    assert(user==&callbacks && !strcmp(name,"twice") && count==1);
    assert(api->close(active)==SOLAR_OS_NATIVE_ERROR_BUSY);
    ++callbacks;*value=2*args[0];return true;
}
static void *evaluate_thread(void *user)
{
    double value=0;solar_os_native_expr_context_v1_t context={.struct_size=sizeof(context),.flags=SOLAR_OS_NATIVE_EXPR_HAS_X,.x=(uintptr_t)user};
    for(unsigned i=0;i<10000;++i) assert(!api->evaluate(active,&context,&value,NULL) && value==context.x*context.x);
    return NULL;
}
int main(void)
{
    api=solar_os_native_expr_get_service("expr",1,8);assert(api && api->struct_size==sizeof(*api));
    assert(!solar_os_native_expr_get_service("expr",2,8));
    assert(!solar_os_native_expr_get_service("expr",1,sizeof(*api)+1));
    assert(!solar_os_native_expr_get_service("absent",1,8));
    solar_os_native_expr_error_v1_t error;solar_os_native_handle_t id=123;double value=-1;bool uses_x=true;
    assert(api->compile(NULL,&id,&error)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !id && !error.message[0]);
    assert(api->compile("2+",&id,&error)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !id && error.message[0] && !live);
    fail_allocation=true;
    assert(api->compile("2",&id,&error)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !id && !live);fail_allocation=false;
    char source[]="2+3*4";assert(!api->compile(source,&id,&error));memset(source,'!',sizeof(source)-1);
    assert(!api->evaluate(id,NULL,&value,&error) && value==14 && !error.message[0]);
    assert(!api->uses_x(id,&uses_x) && !uses_x);assert(!api->close(id) && !live);
    assert(api->evaluate(id,NULL,&value,&error)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !value);
    assert(api->close(id)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->compile("a+twice(x)",&active,&error) && !api->uses_x(active,&uses_x) && uses_x);
    solar_os_native_expr_context_v1_t context={.struct_size=sizeof(context),.flags=SOLAR_OS_NATIVE_EXPR_HAS_X,.x=4,
        .user=&callbacks,.variable=variable,.function=function};
    unsigned before=allocations;
    assert(!api->evaluate(active,&context,&value,&error) && value==11 && callbacks==2 && allocations==before);
    context.struct_size=8;assert(api->evaluate(active,&context,&value,&error)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !value);
    context.struct_size=sizeof(context);context.flags=0x80000000;
    assert(api->evaluate(active,&context,&value,&error)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->close(active));
    assert(!api->compile("sqrt(-1)",&id,&error));
    assert(api->evaluate(id,NULL,&value,&error)==SOLAR_OS_NATIVE_ERROR_FAILED && !value && error.message[0]);assert(!api->close(id));
    assert(!api->compile("x*x",&active,NULL));
    pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,evaluate_thread,(void *)3));
    assert(!pthread_create(&threads[1],NULL,evaluate_thread,(void *)4));
    assert(!pthread_join(threads[0],NULL) && !pthread_join(threads[1],NULL));assert(!api->close(active));
    /* Pins prevent close during evaluation; slots and stale IDs remain typed. */
    assert(!api->compile("2",&id,NULL));
    void *p=solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_EXPR);assert(p);
    assert(!solar_os_native_handle_acquire(id,SOLAR_OS_NATIVE_HANDLE_FILE));
    assert(solar_os_native_handle_close_kind(id,SOLAR_OS_NATIVE_HANDLE_FILE)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->close(id)==SOLAR_OS_NATIVE_ERROR_BUSY);solar_os_native_handle_release(id);assert(!api->close(id));
    solar_os_native_handle_t handles[128];
    for(unsigned i=0;i<128;++i) assert(!api->compile("2",handles+i,NULL));
    assert(live==128 && api->compile("2",&id,NULL)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !id && live==128);
    for(unsigned i=0;i<128;++i) assert(!api->close(handles[i]));
    assert(!live && !solar_os_native_handle_count());
    return 0;
}
