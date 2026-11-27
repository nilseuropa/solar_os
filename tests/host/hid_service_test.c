#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "solar_os_task.h"
#include "hid_port.h"

enum { FAIL_NONE,FAIL_MUTEX,FAIL_QUEUE,FAIL_TASK };
static unsigned failure,mutexes,queues,task_starts,task_token,notifications;
static bool available,connected,fail_send;
static atomic_bool pause_task,task_entered,task_continue;
static hid_port_keyboard_report_t sent_keyboard;
static hid_port_mouse_report_t sent_mouse;
static hid_port_gamepad_report_t sent_gamepad;
struct hid_test_queue { unsigned capacity,size,head,count;unsigned char bytes[]; };
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if(failure==FAIL_MUTEX)return NULL;
    SemaphoreHandle_t value=malloc(sizeof(*value));assert(value);
    assert(!pthread_mutex_init(value,NULL));++mutexes;return value;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex,TickType_t timeout)
{ (void)timeout;assert(mutex && !pthread_mutex_lock(mutex));return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(mutex && !pthread_mutex_unlock(mutex));return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t mutex)
{ assert(mutexes && !pthread_mutex_destroy(mutex));--mutexes;free(mutex); }
QueueHandle_t xQueueCreate(UBaseType_t count,UBaseType_t size)
{
    if(failure==FAIL_QUEUE)return NULL;
    QueueHandle_t queue=calloc(1,sizeof(*queue)+count*size);assert(queue);
    queue->capacity=count;queue->size=size;++queues;return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t timeout)
{
    assert(!timeout);
    if(queue->count==queue->capacity)return pdFALSE;
    memcpy(queue->bytes+((queue->head+queue->count)%queue->capacity)*queue->size,item,queue->size);
    ++queue->count;return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t queue,void *item,TickType_t timeout)
{
    assert(!timeout);
    if(!queue->count)return pdFALSE;
    memcpy(item,queue->bytes+queue->head*queue->size,queue->size);return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t timeout)
{
    if(!xQueuePeek(queue,item,timeout))return pdFALSE;
    --queue->count;queue->head=(queue->head+1)%queue->capacity;return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t queue) { queue->head=queue->count=0;return pdTRUE; }
void vQueueDelete(QueueHandle_t queue) { assert(queues);--queues;free(queue); }
bool hid_port_available(void) { return available; }
bool hid_port_is_connected(void) { return connected; }
esp_err_t hid_port_send_report(uint8_t id,const void *report,size_t size)
{
    if(fail_send)return ESP_FAIL;
    if(id==HID_PORT_REPORT_KEYBOARD){assert(size==sizeof(sent_keyboard));memcpy(&sent_keyboard,report,size);}
    else if(id==HID_PORT_REPORT_MOUSE){assert(size==sizeof(sent_mouse));memcpy(&sent_mouse,report,size);}
    else {assert(id==HID_PORT_REPORT_GAMEPAD && size==sizeof(sent_gamepad));memcpy(&sent_gamepad,report,size);}
    return ESP_OK;
}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t timeout)
{ (void)clear;(void)timeout;assert(!"The deterministic test drives individual drain steps");return 0; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task==&task_token);++notifications; }
BaseType_t solar_os_task_create_pinned_external(TaskFunction_t entry,const char *name,uint32_t stack,
    void *user,UBaseType_t priority,TaskHandle_t *out,BaseType_t core,solar_os_task_role_t role)
{
    assert(entry && !strcmp(name,"solar_os_hid") && stack==3072 && !user && priority==2 &&
        core==tskNO_AFFINITY && role==SOLAR_OS_TASK_ROLE_SYSTEM);
    *out=NULL;
    if(failure==FAIL_TASK)return pdFAIL;
    if(atomic_load(&pause_task)) {
        atomic_store(&task_entered,true);
        while(!atomic_load(&task_continue)){struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);}
    }
    *out=&task_token;++task_starts;return pdPASS;
}

#include "../../src/services/solar_os_hid.c"
static void hid_test_shutdown(void)
{
    if(hid.keyboard_queue)vQueueDelete(hid.keyboard_queue);
    if(hid.mutex)vSemaphoreDelete(hid.mutex);
    hid=(hid_state_t){0};
    assert(!queues && !mutexes);
}
static void *initialize(void *result)
{ *(esp_err_t *)result=solar_os_hid_init();return NULL; }
static int hid_service_checks(void)
{
    assert(solar_os_hid_init()==ESP_ERR_NOT_SUPPORTED && !mutexes && !queues);
    available=connected=true;
    for(unsigned step=FAIL_MUTEX;step<=FAIL_TASK;++step) {
        failure=step;
        assert(solar_os_hid_init()==ESP_ERR_NO_MEM);
        assert(!mutexes && !queues && !hid.task);
        solar_os_hid_status_t status;solar_os_hid_get_status(&status);
        assert(!status.initialized && !status.connected);
        failure=FAIL_NONE;assert(!solar_os_hid_init());hid_test_shutdown();
    }
    failure=FAIL_NONE;
    atomic_store(&pause_task,true);atomic_store(&task_entered,false);atomic_store(&task_continue,false);
    esp_err_t result=ESP_FAIL;pthread_t thread;assert(!pthread_create(&thread,NULL,initialize,&result));
    for(unsigned i=0;!atomic_load(&task_entered) && i<2000;++i){struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);}
    assert(atomic_load(&task_entered) && mutexes==1 && queues==1);
    unsigned before=task_starts;
    assert(solar_os_hid_init()==ESP_ERR_INVALID_STATE);
    solar_os_hid_status_t status;solar_os_hid_get_status(&status);
    assert(!status.initialized && !status.connected);
    const uint16_t key=4;
    assert(solar_os_hid_keyboard_press(&key,1)==ESP_ERR_INVALID_STATE);
    solar_os_hid_release_all();
    atomic_store(&task_continue,true);assert(!pthread_join(thread,NULL) && !result);
    atomic_store(&pause_task,false);
    assert(task_starts==before+1 && !solar_os_hid_init() && task_starts==before+1);
    solar_os_hid_get_status(&status);assert(status.initialized && status.connected);
    assert(!solar_os_hid_keyboard_press(&key,1));fail_send=true;
    assert(!hid_send_keyboard() && hid.keyboard_queue->count==1);
    fail_send=false;assert(hid_send_keyboard() && sent_keyboard.keycode[0]==4);
    assert(!solar_os_hid_keyboard_release(&key,1));solar_os_hid_release_all();
    assert(hid.keyboard_queue->count==2 && hid_send_keyboard() && !sent_keyboard.keycode[0]);
    assert(hid_send_keyboard() && !sent_keyboard.keycode[0]);
    assert(!solar_os_hid_mouse_move(INT32_MAX,INT32_MIN));assert(hid_send_mouse());
    assert(sent_mouse.x==127 && sent_mouse.y==-128);
    assert(!solar_os_hid_gamepad_axis(SOLAR_OS_HID_AXIS_X,INT16_MIN));
    assert(!solar_os_hid_gamepad_button(32,true) && !solar_os_hid_gamepad_hat(SOLAR_OS_HID_HAT_UP));
    assert(!solar_os_hid_gamepad_send() && hid_send_gamepad());
    assert(sent_gamepad.x==-127 && sent_gamepad.buttons==(UINT32_C(1)<<31) && sent_gamepad.hat==SOLAR_OS_HID_HAT_UP);
    solar_os_hid_release_all();connected=false;
    solar_os_hid_release_all();assert(hid.keyboard_queue->count==1);
    connected=true;assert(hid_send_keyboard() && !sent_keyboard.modifier && !sent_keyboard.keycode[0]);
    assert(notifications);
    puts("HID service: initialization rollback/retry, concurrent admission, publication, queued transitions and report drains passed");
    return 0;
}
#ifndef SOLAR_OS_HID_SERVICE_FIXTURE
int main(void) { int result=hid_service_checks();hid_test_shutdown();return result; }
#endif
