#include <pthread.h>
#define main legacy_input_checks
#include "input_test.c"
#undef main

static pthread_mutex_t callback_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_condition=PTHREAD_COND_INITIALIZER;
static bool entered,finish;
static unsigned calls;
static solar_os_input_source_t gesture_source,touch_source;
static void block(void)
{
    pthread_mutex_lock(&callback_lock);++calls;entered=true;
    pthread_cond_broadcast(&callback_condition);
    while(!finish)pthread_cond_wait(&callback_condition,&callback_lock);
    pthread_mutex_unlock(&callback_lock);
}
static void gesture(const solar_os_input_gesture_event_t *event,void *context)
{ assert(event && context==&calls);block(); }
static bool pointer(const solar_os_input_pointer_event_t *event,void *context)
{ assert(event && context==&calls);block();return true; }
static void wait_for_callback(void)
{
    pthread_mutex_lock(&callback_lock);
    while(!entered)pthread_cond_wait(&callback_condition,&callback_lock);
    pthread_mutex_unlock(&callback_lock);
}
static void finish_callback(pthread_t thread)
{
    pthread_mutex_lock(&callback_lock);finish=true;pthread_cond_broadcast(&callback_condition);
    pthread_mutex_unlock(&callback_lock);assert(!pthread_join(thread,NULL));
    entered=false;finish=false;
}
static void *publish(void *unused)
{
    (void)unused;const solar_os_input_gesture_event_t event={.gesture=SOLAR_OS_INPUT_GESTURE_FLICK,.direction=SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST};
    assert(!solar_os_input_write_gesture(gesture_source,&event));return NULL;
}
static void *filter(void *unused)
{
    (void)unused;const solar_os_input_pointer_event_t event={.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE};
    assert(solar_os_input_pointer_filter_event(&event));return NULL;
}
static void *poll_touch(void *unused) { (void)unused;solar_os_input_poll();return NULL; }
static bool retire_self(const solar_os_input_pointer_event_t *event,void *context)
{
    assert(event && context==&calls);++calls;
    assert(solar_os_input_pointer_filter_try_unregister(retire_self,context)==ESP_ERR_INVALID_STATE);
    return false;
}
int main(void)
{
    assert(!solar_os_input_init());
    assert(!solar_os_input_source_open_typed("gesture",SOLAR_OS_INPUT_SOURCE_GESTURE,SOLAR_OS_INPUT_CAP_GESTURE_EVENTS,true,&gesture_source));
    assert(!solar_os_input_gesture_observer_register(gesture,&calls));
    pthread_t worker;assert(!pthread_create(&worker,NULL,publish,NULL));wait_for_callback();
    assert(solar_os_input_gesture_observer_try_unregister(gesture,&calls)==ESP_ERR_INVALID_STATE);
    /* Retirement blocks new snapshots, but already pinned code/context stays alive. */
    const solar_os_input_gesture_event_t event={.gesture=SOLAR_OS_INPUT_GESTURE_FLICK};
    assert(!solar_os_input_write_gesture(gesture_source,&event) && calls==1);
    assert(solar_os_input_gesture_observer_register(gesture,&calls)==ESP_ERR_INVALID_STATE);
    finish_callback(worker);
    assert(solar_os_input_gesture_observer_try_unregister(gesture,&calls)==ESP_ERR_NOT_FOUND);
    assert(!solar_os_input_gesture_observer_register(gesture,&calls));
    assert(!solar_os_input_gesture_observer_try_unregister(gesture,&calls));

    assert(!solar_os_input_pointer_filter_register(pointer,&calls));
    assert(!pthread_create(&worker,NULL,filter,NULL));wait_for_callback();
    assert(solar_os_input_pointer_filter_try_unregister(pointer,&calls)==ESP_ERR_INVALID_STATE);
    const solar_os_input_pointer_event_t point={.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE};
    assert(!solar_os_input_pointer_filter_event(&point) && calls==2);
    assert(solar_os_input_pointer_filter_register(pointer,&calls)==ESP_ERR_INVALID_STATE);
    finish_callback(worker);
    assert(solar_os_input_pointer_filter_try_unregister(pointer,&calls)==ESP_ERR_NOT_FOUND);
    assert(!solar_os_input_pointer_filter_register(retire_self,&calls));
    assert(!solar_os_input_pointer_filter_event(&point) && calls==3);
    assert(!solar_os_input_pointer_filter_event(&point) && calls==3);
    assert(solar_os_input_pointer_filter_try_unregister(retire_self,&calls)==ESP_ERR_NOT_FOUND);

    /* Timed touch recognition uses the same pinned-delivery path. */
    assert(!solar_os_input_touch_source_open("touch",&touch_source));
    assert(!solar_os_input_gesture_observer_register(gesture,&calls));
    now_us=1000000;
    solar_os_input_pointer_event_t touch={.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE,.action=SOLAR_OS_INPUT_POINTER_PRESS,.buttons=SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY,.x=50,.y=50};
    assert(!solar_os_input_write_pointer(touch_source,&touch));
    now_us+=100000;touch.action=SOLAR_OS_INPUT_POINTER_RELEASE;touch.buttons=0;
    assert(!solar_os_input_write_pointer(touch_source,&touch));
    now_us+=400000;
    assert(!pthread_create(&worker,NULL,poll_touch,NULL));wait_for_callback();
    assert(solar_os_input_gesture_observer_try_unregister(gesture,&calls)==ESP_ERR_INVALID_STATE);
    finish_callback(worker);
    assert(solar_os_input_gesture_observer_try_unregister(gesture,&calls)==ESP_ERR_NOT_FOUND);
    assert(calls==4);
    solar_os_input_source_close(touch_source);solar_os_input_source_close(gesture_source);
    solar_os_input_pointer_event_t pointers[32];(void)solar_os_input_read_pointer_events(pointers,32);
    solar_os_input_gesture_event_t gestures[32];(void)solar_os_input_read_gesture_events(gestures,32);
    return 0;
}
