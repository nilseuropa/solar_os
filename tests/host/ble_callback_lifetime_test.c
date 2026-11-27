#define main baseline_ble_service_checks
#include "ble_service_test.c"
#undef main

typedef struct {
    atomic_bool entered, finish, alive;
    atomic_uint calls;
    bool cancel;
} callback_context_t;
typedef struct {
    solar_os_ble_session_t owner;
    callback_context_t *context;
    bool hid;
    esp_err_t result;
    atomic_bool done;
} probe_request_t;
typedef struct { solar_os_ble_session_t owner; atomic_bool started, done; esp_err_t result; } close_t;

static bool cancellation_probe(void *user)
{
    callback_context_t *context=user;
    assert(atomic_load(&context->alive));
    atomic_fetch_add(&context->calls,1);
    atomic_store(&context->entered,true);
    /* Deliberately pause this normally nonblocking callback to observe its
     * lifetime and service-lock boundaries from another task. */
    while(!atomic_load(&context->finish)) {
        struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    return context->cancel;
}
static void *peripheral_request(void *user)
{
    probe_request_t *call=user;
    const solar_os_ble_call_options_v1_t options={.struct_size=sizeof(options),
        .cancelled=cancellation_probe,.user=call->context};
    if(call->hid) {
        solar_os_ble_hid_request_t request={.op=SOLAR_OS_BLE_HID_OP_START,.name="Probe"};
        call->result=solar_os_ble_hid_request_with_options(call->owner,&request,&options);
    } else {
        solar_os_ble_server_request_t request={.op=SOLAR_OS_BLE_SERVER_CREATE,.text="Probe"};
        call->result=solar_os_ble_server_request_with_options(call->owner,&request,&options);
    }
    atomic_store(&call->done,true);
    return NULL;
}
static void *close_owner(void *user)
{
    close_t *call=user;atomic_store(&call->started,true);
    call->result=solar_os_ble_session_close(call->owner);atomic_store(&call->done,true);
    return NULL;
}
static void wait_flag(atomic_bool *flag)
{
    for(unsigned i=0;!atomic_load(flag) && i<2000;++i) {
        struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    assert(atomic_load(flag));
}
static void check_peripheral(bool hid, bool cancel)
{
    solar_os_ble_session_t owner;
    assert(!solar_os_ble_session_create("borrowed-callback",&owner));
    callback_context_t context={.alive=true,.cancel=cancel};
    probe_request_t call={.owner=owner,.hid=hid,.context=&context};pthread_t worker,closer;
    assert(!pthread_create(&worker,NULL,peripheral_request,&call));
    wait_flag(&context.entered);
    /* Metadata remains available while the callback is active. */
    solar_os_ble_session_info_t info;
    assert(!solar_os_ble_session_get_info(owner,&info) && !atomic_load(&call.done));
    close_t close={.owner=owner};
    if(!cancel) {
        assert(!pthread_create(&closer,NULL,close_owner,&close));wait_flag(&close.started);
        assert(!atomic_load(&close.done));
    }
    atomic_store(&context.finish,true);
    assert(!pthread_join(worker,NULL));
    assert(call.result==(cancel?SOLAR_OS_BLE_ERR_CANCELLED:ESP_OK));
    atomic_store(&context.alive,false);
    const unsigned calls=atomic_load(&context.calls);
    assert(calls==1);
    if(!cancel) {
        assert(!pthread_join(closer,NULL) && !close.result);
        assert(solar_os_ble_session_get_info(owner,&info)==ESP_ERR_INVALID_STATE);
        assert(!fake_server_owner && !fake_hid_owner);
    } else {
        /* No previous predicate is reused by a later ordinary request. */
        if(hid) assert(!solar_os_ble_hid_device_start(owner,"Later"));
        else {
            solar_os_ble_server_request_t request={.op=SOLAR_OS_BLE_SERVER_CREATE,.text="Later"};
            assert(!solar_os_ble_server_request(owner,&request));
        }
        assert(!solar_os_ble_session_close(owner));
    }
    assert(atomic_load(&context.calls)==calls);
}
int main(void)
{
    assert(!baseline_ble_service_checks());
    check_peripheral(false,false);check_peripheral(true,false);
    check_peripheral(false,true);check_peripheral(true,true);
    puts("BLE borrowed callbacks: caller lifetime, metadata access, concurrent close and no later reuse passed");
    return 0;
}
