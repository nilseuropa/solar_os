#include "solar_os_native_handles.h"
#define main original_scope_checks
#include "application_scopes_test.c"
#undef main

static const solar_os_app_t failed_app, stateless_app;
static unsigned objects[6]={1,2,3,4,5,6};
static unsigned order[8], released;
static bool destroy_busy;
static solar_os_native_handle_t resource;
static int destroy(void *state)
{
    const unsigned value=*(unsigned *)state;
    if(value<=2) assert(cold && solar_os_application_descriptor()==&parent);
    if(value==4) assert(cold && solar_os_application_descriptor()==&failed_app);
    if(value==5) assert(!cold && solar_os_application_descriptor()==&stateless_app);
    if(value==2 && destroy_busy) return SOLAR_OS_NATIVE_ERROR_BUSY;
    order[released++]=value;
    return SOLAR_OS_NATIVE_OK;
}
static esp_err_t failed_start(solar_os_context_t *ctx)
{
    assert(ctx && cold && solar_os_application_descriptor()==&failed_app);
    assert(!solar_os_native_handle_create_owned(200,objects+3,destroy,&failed_app,&resource));
    return ESP_FAIL;
}
static esp_err_t stateless_start(solar_os_context_t *ctx)
{
    assert(ctx && !cold && solar_os_application_descriptor()==&stateless_app);
    return solar_os_native_handle_create_owned(200,objects+4,destroy,&stateless_app,&resource);
}
static const solar_os_app_t failed_app={.name="parent",.app_class=SOLAR_OS_APP_CLASS_COMMAND,
    .start=failed_start,.state_slot=&cold,.state_size=16,.state_storage=SOLAR_OS_APP_STATE_TRANSIENT};
static const solar_os_app_t stateless_app={.name="stateless",.app_class=SOLAR_OS_APP_CLASS_COMMAND,.start=stateless_start};
int main(void)
{
    assert(!original_scope_checks() && !solar_os_native_handle_count());
    start_failure=false;ready=true;
    assert(!solar_os_app_start(&parent,&contexts[0]) && cold && allocations==1);
    solar_os_native_handle_t first,last,foreign,unowned;
    assert(!solar_os_native_handle_create_owned(200,objects,destroy,&parent,&first));
    assert(!solar_os_native_handle_create_owned(200,objects+1,destroy,&parent,&last));
    assert(!solar_os_native_handle_create_owned(200,objects+2,destroy,&child,&foreign));
    assert(!solar_os_native_handle_create(200,objects+5,destroy,&unowned));
    assert(!solar_os_native_handle_acquire_owned(last,200,&child));
    assert(solar_os_native_handle_close_owned(last,200,&child)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(solar_os_native_handle_acquire_owned(last,200,&parent)==objects+1);
    ready=false;
    assert(!solar_os_app_stop_checked(&parent,&contexts[0]) && !released && allocations==1);
    ready=true;
    assert(!solar_os_app_stop_checked(&parent,&contexts[0]) && !released && allocations==1);
    solar_os_native_handle_release(last);
    destroy_busy=true;
    assert(!solar_os_app_stop_checked(&parent,&contexts[0]) && !released && allocations==1);
    assert(solar_os_native_handle_acquire_owned(first,200,&parent)==objects);
    solar_os_native_handle_release(first);destroy_busy=false;
    assert(solar_os_app_stop_checked(&parent,&contexts[0]) && !allocations && !cold);
    assert(released==2 && order[0]==2 && order[1]==1 && solar_os_native_handle_count()==2);
    assert(!solar_os_native_handle_acquire(first,200));
    assert(!solar_os_native_handle_close_owner(&child) && order[2]==3);
    assert(!solar_os_native_handle_close(unowned) && order[3]==6 && !solar_os_native_handle_count());
    assert(solar_os_app_start(&failed_app,&contexts[0])==ESP_FAIL && !cold && !allocations);
    assert(released==5 && order[4]==4 && !solar_os_native_handle_count());
    assert(!solar_os_app_start(&stateless_app,&contexts[0]) && !cold && !allocations);
    solar_os_app_suspend(&stateless_app,&contexts[0]);solar_os_app_resume(&stateless_app,&contexts[0]);
    assert(solar_os_native_handle_count()==1 && released==5);
    assert(solar_os_app_stop_checked(&stateless_app,&contexts[0]) && order[5]==5 && !solar_os_native_handle_count());
    resource=999;
    assert(solar_os_native_handle_create_owned(200,objects,destroy,NULL,&resource)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !resource);
    assert(solar_os_native_handle_close_owner(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!solar_os_application_descriptor());
    return 0;
}
