#define SOLAR_OS_NATIVE_BLE_CLIENT_FIXTURE
#include "native_ble_client_test.c"
#include "solar_os_native_bluetooth_abi.h"

static const solar_os_native_bluetooth_api_v1_t *modern;
typedef struct {
    atomic_bool cancel;
    atomic_uint checks;
    unsigned cancel_after;
    bool alive;
    pthread_t caller;
} cancellation_t;
static bool operation_cancelled(void *user)
{
    cancellation_t *context=user;
    assert(context && context->alive && pthread_equal(context->caller,pthread_self()));
    unsigned checks=atomic_fetch_add(&context->checks,1)+1;
    return atomic_load(&context->cancel) || (context->cancel_after && checks>context->cancel_after);
}
static solar_os_ble_call_options_v1_t call_options(cancellation_t *context)
{
    context->alive=true;context->caller=pthread_self();
    return (solar_os_ble_call_options_v1_t){.struct_size=sizeof(solar_os_ble_call_options_v1_t),.cancelled=operation_cancelled,.user=context};
}
typedef struct {
    solar_os_ble_session_t owner;
    solar_os_ble_peer_t peer;
    cancellation_t *context;
    int result;
    uint8_t bytes[2];
    size_t length;
} operation_t;
static void *read_with_options(void *value)
{
    operation_t *call=value;
    if(call->context){
        solar_os_ble_call_options_v1_t options=call_options(call->context);
        call->result=modern->peer_read_with_options(call->owner,call->peer,3,call->bytes,sizeof(call->bytes),&call->length,5000,&options);
    }else call->result=modern->peer_read(call->owner,call->peer,3,call->bytes,sizeof(call->bytes),&call->length,5000);
    return NULL;
}
static void wait_deferred(unsigned count)
{
    for(unsigned i=0;atomic_load(&deferred_reads)<count && i<2000;++i){
        struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    assert(atomic_load(&deferred_reads)>=count);
}
static void complete_read(uint32_t epoch,uint32_t request)
{
    uint8_t byte=0x5a;
    solar_os_ble_backend_event_t event={.type=SOLAR_OS_BLE_BACKEND_READ,.epoch=epoch,.request=request,
        .conn_id=peer_for(epoch)->conn,.handle=3,.value=&byte,.value_len=1};
    solar_os_ble_service_event(&event);
}
int main(void)
{
    assert(!native_ble_client_checks());
    modern=solar_os_native_ble_get_service("ble",1,sizeof(*modern));assert(modern);
    solar_os_ble_session_t owner;assert(!modern->session_create("call-options",&owner));
    uint8_t address[6]={1,2,3,4,5,1},bytes[2];size_t length=999;
    solar_os_ble_call_options_v1_t invalid={0};solar_os_ble_peer_t failed=999;
    assert(modern->peer_connect_with_options(owner,address,0,100,&failed,&invalid)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !failed);
    assert(modern->session_read_with_options(owner,3,bytes,sizeof(bytes),&length,100,&invalid)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !length);

    cancellation_t connect_context={.cancel_after=1};solar_os_ble_call_options_v1_t options=call_options(&connect_context);
    defer_discovery=true;
    assert(modern->session_connect_with_options(owner,address,0,1000,&options)==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    defer_discovery=false;connect_context.alive=false;
    assert(atomic_load(&connect_context.checks)>1);
    cancellation_t session_context={0};options=call_options(&session_context);
    assert(!modern->session_connect_with_options(owner,address,0,100,&options));
    assert(!modern->session_pair_with_options(owner,123456,100,&options));
    assert(!modern->session_read_with_options(owner,3,bytes,sizeof(bytes),&length,100,&options));
    assert(!modern->session_write_with_options(owner,3,bytes,1,true,100,&options));
    session_context.alive=false;assert(!modern->session_cancel(owner));

    solar_os_ble_peer_t a,b,c;
    cancellation_t peer_context={0};options=call_options(&peer_context);
    assert(!modern->peer_connect_with_options(owner,address,0,100,&a,&options));
    assert(!modern->peer_pair_with_options(owner,a,123456,100,&options));
    assert(!modern->peer_write_with_options(owner,a,3,bytes,1,true,100,&options));
    peer_context.alive=false;
    address[5]=2;assert(!modern->peer_connect(owner,address,0,100,&b));
    cancellation_t context_a={0},context_b={0};
    operation_t read_a={.owner=owner,.peer=a,.context=&context_a},read_b={.owner=owner,.peer=b,.context=&context_b};
    pthread_t worker_a,worker_b;unsigned before=atomic_load(&deferred_reads);atomic_store(&defer_all_reads,true);
    assert(!pthread_create(&worker_a,NULL,read_with_options,&read_a));assert(!pthread_create(&worker_b,NULL,read_with_options,&read_b));
    wait_deferred(before+2);
    atomic_store(&context_a.cancel,true);assert(!pthread_join(worker_a,NULL) && read_a.result==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    context_a.alive=false;
    solar_os_ble_session_info_t info;assert(!modern->peer_get_info(owner,b,&info) && info.busy);
    atomic_store(&context_b.cancel,true);assert(!pthread_join(worker_b,NULL) && read_b.result==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    context_b.alive=false;atomic_store(&defer_all_reads,false);
    assert(!modern->peer_disconnect(owner,a));assert(!modern->peer_disconnect(owner,b));
    assert(atomic_load(&context_a.checks)>1 && atomic_load(&context_b.checks)>1);

    address[5]=3;assert(!modern->peer_connect(owner,address,0,100,&c));
    uint32_t epoch=find_locked(c)->link.epoch;atomic_store(&delayed_epoch,epoch);atomic_store(&read_request,0);
    cancellation_t success_context={0};operation_t success={.owner=owner,.peer=c,.context=&success_context};
    before=atomic_load(&deferred_reads);assert(!pthread_create(&worker_a,NULL,read_with_options,&success));wait_deferred(before+1);
    complete_read(epoch,atomic_load(&read_request));assert(!pthread_join(worker_a,NULL) && !success.result && success.length==1 && success.bytes[0]==0x5a);
    success_context.alive=false;unsigned checks=atomic_load(&success_context.checks);
    /* The next blocking read on the same peer must not reuse the expired
     * options record, callback or context from the preceding call. */
    operation_t plain={.owner=owner,.peer=c};before=atomic_load(&deferred_reads);atomic_store(&read_request,0);
    assert(!pthread_create(&worker_a,NULL,read_with_options,&plain));wait_deferred(before+1);
    struct timespec delay={.tv_nsec=10000000};nanosleep(&delay,NULL);
    assert(atomic_load(&success_context.checks)==checks);complete_read(epoch,atomic_load(&read_request));
    assert(!pthread_join(worker_a,NULL) && !plain.result);atomic_store(&delayed_epoch,0);

    cancellation_t subscribe_context={.cancel_after=1};options=call_options(&subscribe_context);defer_subscription=true;
    assert(modern->peer_subscribe_with_options(owner,c,3,1,1000,&options)==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    defer_subscription=false;subscribe_context.alive=false;assert(!modern->peer_disconnect(owner,c));
    cancellation_t peripheral_context={.cancel=true};options=call_options(&peripheral_context);
    solar_os_ble_server_request_t server={.op=SOLAR_OS_BLE_SERVER_CREATE};strcpy(server.text,"Scoped");
    assert(modern->server_request_with_options(owner,&server,&options)==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    solar_os_ble_hid_request_t hid={.op=SOLAR_OS_BLE_HID_OP_START};strcpy(hid.name,"Scoped");
    assert(modern->hid_request_with_options(owner,&hid,&options)==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    peripheral_context.alive=false;checks=atomic_load(&peripheral_context.checks);
    assert(modern->server_request(owner,&server)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(modern->hid_request_with_options(owner,&hid,NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(atomic_load(&peripheral_context.checks)==checks);
    hid.key_count=9;assert(modern->hid_request_with_options(owner,&hid,NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    hid.key_count=0;hid.host_count=SOLAR_OS_BLE_HID_HOST_MAX+1;
    assert(modern->hid_request_with_options(owner,&hid,NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!modern->session_close(owner) && !entries() && !adapter_allocations);
    puts("Bluetooth call options: caller-thread callbacks, independent peer cancellation, borrowed context expiry, preflight checks and peripheral bounds passed");
    return 0;
}
