#include "solar_os_native_input_actions.h"
#include "solar_os_native_input_actions_abi.h"
#include "solar_os_input_actions.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"
#define main baseline_input_checks
#include "input_test.c"
#undef main

struct test_queue { unsigned head,count;size_t capacity,size;unsigned char items[8][8]; };
static struct test_queue queue;
static bool fail_queue,fail_task,skip_join,keep_queued_on_reset,stop_inside_runner;
static TaskFunction_t pending;
static void *pending_arg;
static unsigned task_token,main_token,commands_run,workers_created,workers_deleted;
static TaskHandle_t current=&main_token;
static const solar_os_native_input_actions_api_v1_t *api;
static char last_command[SOLAR_OS_INPUT_ACTION_COMMAND_MAX];
QueueHandle_t solar_os_queue_create(UBaseType_t capacity,UBaseType_t size)
{
    assert(capacity==8 && size<=8);
    if(fail_queue)return NULL;
    queue=(struct test_queue){.capacity=capacity,.size=size};return &queue;
}
BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t wait)
{
    assert(q==&queue && !wait);
    if(q->count==q->capacity)return pdFALSE;
    memcpy(q->items[(q->head+q->count)%q->capacity],item,q->size);++q->count;return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *item,TickType_t wait)
{
    assert(q==&queue);
    if(!q->count){now_us+=(int64_t)wait*1000;return pdFALSE;}
    memcpy(item,q->items[q->head],q->size);q->head=(q->head+1)%q->capacity;--q->count;return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t q)
{
    assert(q==&queue);
    /* Model an old observer delivery enqueued just after the reset. */
    if(keep_queued_on_reset){keep_queued_on_reset=false;return pdTRUE;}
    q->head=q->count=0;return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { assert(q==&queue);return q->count; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(now_us/1000); }
void vTaskDelay(TickType_t ticks) { now_us+=(int64_t)ticks*1000; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t entry,const char *name,uint32_t stack,
    void *arg,UBaseType_t priority,TaskHandle_t *out,BaseType_t core,solar_os_task_role_t role)
{
    assert(!strcmp(name,"input_actions") && stack==6144 && priority==3 && core==tskNO_AFFINITY && role==SOLAR_OS_TASK_ROLE_BACKGROUND);
    if(fail_task)return pdFALSE;
    assert(!pending);pending=entry;pending_arg=arg;*out=&task_token;++workers_created;return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t task) { assert(!task && current==&task_token);++workers_deleted; }
static void run_worker(void)
{
    assert(pending);TaskFunction_t entry=pending;pending=NULL;
    current=&task_token;entry(pending_arg);current=&main_token;
}
bool solar_os_task_wait_done(TaskHandle_t task,volatile bool *done,uint32_t timeout)
{
    assert(timeout==SOLAR_OS_TASK_STOP_WAIT_MS);
    if(skip_join)return false;
    if(task && pending)run_worker();
    return !task || *done;
}
static esp_err_t runner(const char *command)
{
    assert(current==&task_token);strlcpy(last_command,command,sizeof(last_command));++commands_run;
    if(stop_inside_runner)assert(api->stop()==SOLAR_OS_NATIVE_ERROR_BUSY);
    return ESP_OK;
}
static solar_os_input_source_t source;
static void publish(solar_os_input_gesture_t gesture,solar_os_input_gesture_direction_t direction)
{
    solar_os_input_gesture_event_t event={.gesture=gesture,.direction=direction},routed;
    assert(!solar_os_input_write_gesture(source,&event));
    /* The observer neither executes a command nor consumes foreground input. */
    assert(solar_os_input_read_gesture_events(&routed,1)==1 && routed.gesture==gesture && routed.source==source);
}
int main(void)
{
    assert(!solar_os_input_init());solar_os_input_actions_set_runner(runner);
    api=solar_os_native_input_actions_get_service("input.actions",1,8);assert(api && api->struct_size==sizeof(*api));
    assert(!solar_os_native_input_actions_get_service("input.actions",2,8));
    assert(!solar_os_native_input_actions_get_service("input.actions",1,sizeof(*api)+1));
    assert(!solar_os_native_input_actions_get_service("missing",1,8));
    solar_os_native_input_action_limits_v1_t limits;
    assert(!api->limits(&limits) && limits.maximum_bindings==16 && limits.maximum_cooldown_ms==3600000 && limits.default_cooldown_ms==250);
    assert(api->limits(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    uint32_t id=999;char name[]="gesture0",command[]="echo old";
    assert(api->bind("",SOLAR_OS_INPUT_GESTURE_FLICK,false,SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,250,command,&id)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !id);
    assert(api->bind(name,SOLAR_OS_INPUT_GESTURE_COUNT,true,0,0,command,&id)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !id);
    assert(api->bind(name,SOLAR_OS_INPUT_GESTURE_FLICK,true,0,limits.maximum_cooldown_ms+1,command,&id)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !id);
    assert(!api->bind(name,SOLAR_OS_INPUT_GESTURE_FLICK,false,SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,250,command,&id) && id && api->count()==1);
    memset(name,'x',sizeof(name)-1);memset(command,'x',sizeof(command)-1);
    solar_os_input_action_binding_t binding;
    assert(api->get(0,&binding) && !strcmp(binding.source,"gesture0") && !strcmp(binding.command,"echo old"));
    assert(!api->get(0,NULL));assert(!api->get(1,&binding) && !binding.id);
    assert(api->get(0,&binding));
    solar_os_input_gesture_event_t event={.gesture=SOLAR_OS_INPUT_GESTURE_FLICK,.direction=SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST};
    assert(api->matches(&binding,"gesture0",&event,false,0,1000));
    assert(!api->matches(&binding,"other",&event,false,0,1000));
    assert(!api->matches(&binding,"gesture0",&event,true,1000,1249));
    assert(api->matches(&binding,"gesture0",&event,true,1000,1250));
    assert(!api->matches(NULL,"gesture0",&event,false,0,1000));
    fail_queue=true;assert(api->start()==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !api->running() && !api->worker_active());fail_queue=false;
    assert(!api->start() && api->running() && !api->worker_active() && !workers_created);
    assert(api->start()==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!solar_os_input_source_open_typed("gesture0",SOLAR_OS_INPUT_SOURCE_GESTURE,SOLAR_OS_INPUT_CAP_GESTURE_EVENTS,true,&source));
    now_us=1000000;publish(event.gesture,event.direction);assert(!commands_run && api->worker_active());
    now_us=1149000;publish(event.gesture,event.direction);assert(queue.count==1);
    now_us=1250000;publish(event.gesture,event.direction);assert(queue.count==2);
    assert(api->get(0,&binding) && binding.trigger_count==2 && !binding.dropped_count);
    run_worker();assert(commands_run==2 && !strcmp(last_command,"echo old") && !api->worker_active());
    assert(workers_created==workers_deleted);
    publish(event.gesture,SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST);assert(!pending);
    now_us=2000000;publish(event.gesture,event.direction);assert(pending);
    keep_queued_on_reset=true;assert(api->clear()==1);
    assert(!api->bind(NULL,event.gesture,true,0,0,"echo new",&id) && id==1);
    unsigned before=commands_run;run_worker();assert(commands_run==before); /* Old generation cannot run a reused id. */
    publish(event.gesture,event.direction);run_worker();assert(commands_run==before+1 && !strcmp(last_command,"echo new"));
    for(unsigned i=0;i<10;++i)publish(event.gesture,event.direction);
    assert(api->get(0,&binding) && binding.trigger_count==11 && binding.dropped_count==2);
    before=commands_run;run_worker();assert(commands_run==before+8 && !api->worker_active());
    fail_task=true;publish(event.gesture,event.direction);fail_task=false;
    assert(!api->worker_active() && !pending && api->get(0,&binding) && binding.dropped_count==3);
    publish(event.gesture,event.direction);skip_join=true;
    assert(api->stop()==SOLAR_OS_NATIVE_ERROR_BUSY && !api->running() && api->worker_active());
    before=commands_run;run_worker();assert(commands_run==before && !api->worker_active());
    skip_join=false;assert(!api->stop() && api->count()==1);
    assert(!api->unbind(id) && !api->count());
    assert(api->unbind(id)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && api->unbind(0)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    for(uint32_t i=0;i<limits.maximum_bindings;++i)assert(!api->bind("*",SOLAR_OS_INPUT_GESTURE_WAVE,true,0,0,"echo full",NULL));
    assert(api->bind(NULL,event.gesture,true,0,0,"overflow",&id)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !id);
    assert(api->clear()==limits.maximum_bindings);
    assert(!api->bind(NULL,event.gesture,true,0,0,"stop self",&id));
    assert(!api->start());stop_inside_runner=true;publish(event.gesture,event.direction);run_worker();stop_inside_runner=false;
    assert(!api->worker_active() && !api->running() && !api->stop());assert(api->clear()==1);
    assert(api->emit_key("NO_SUCH_KEY")==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->emit_key("ALT+RIGHT"));
    solar_os_input_key_event_t keys[8];size_t count=solar_os_input_read_events(keys,8);
    assert(count==2 && keys[0].key==SOLAR_OS_KEY_RIGHT && keys[0].modifiers==SOLAR_OS_INPUT_MOD_LEFT_ALT && keys[1].action==SOLAR_OS_INPUT_KEY_RELEASE);
    assert(!solar_os_input_get_pressed(keys,8));
    solar_os_input_source_info_t info;assert(solar_os_input_source_find("input.emit",&info));solar_os_input_source_close(info.source);
    solar_os_input_source_close(source);solar_os_input_init();
    assert(workers_created==workers_deleted);
    puts("native input actions: copied rules, independent foreground routing, deferred workers, cooldowns, overload, generations, retryable stop and key emission passed");
    return 0;
}
