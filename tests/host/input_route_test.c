#define main input_service_tests
#include "input_test.c"
#undef main

static bool size_for_display(const char *target,uint16_t *width,uint16_t *height)
{ assert(!strcmp(target,"display0"));*width=320;*height=240;return true; }
static solar_os_input_route_event_t routed(const void *owner,solar_os_input_route_event_type_t type)
{
    solar_os_input_route_event_t event;bool reset;
    assert(solar_os_input_route_read(owner,&event,&reset));
    assert(!reset && event.type==type);
    return event;
}
static void no_routed(const void *owner)
{ solar_os_input_route_event_t event;bool reset;assert(!solar_os_input_route_read(owner,&event,&reset)); }
static void pointer_contact(solar_os_input_source_t source,uint32_t time,
    solar_os_input_pointer_action_t action,int x,int y,uint8_t id)
{
    now_us=(int64_t)time*1000;
    solar_os_input_pointer_sample_t sample={.time_ms=time,
        .event={.pointer_id=id,.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE,.action=action,.x=x,.y=y,
            .buttons=action==SOLAR_OS_INPUT_POINTER_RELEASE?0:1,.target="display0"}};
    assert(solar_os_input_write_pointer_sample(source,&sample)==ESP_OK);
}
static void pointer(solar_os_input_source_t source,uint32_t time,
    solar_os_input_pointer_action_t action,int x,int y)
{ pointer_contact(source,time,action,x,y,0); }
int main(void)
{
    assert(solar_os_input_init()==ESP_OK);
    solar_os_input_source_t real,virtual,touch,other;
    assert(solar_os_input_keyboard_source_open("real",true,&real)==ESP_OK);
    assert(solar_os_input_keyboard_source_open("virtual",true,&virtual)==ESP_OK);
    assert(solar_os_input_touch_source_open("touch",&touch)==ESP_OK);
    assert(solar_os_input_keyboard_source_open("other",true,&other)==ESP_OK);
    solar_os_input_set_display_size_resolver(size_for_display);
    int owner,guest;
    solar_os_input_route_config_t config={.target="display0",.excluded_source=virtual,
        .trigger_key=' ',.trigger_modifiers=SOLAR_OS_INPUT_MOD_LEFT_CTRL,
        .edge_enabled=true,.edge_direction=SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH,
        .native_width=320,.native_height=240,.scale=1,.x=0,.y=120,.width=320,.height=120};
    assert(solar_os_input_route_claim(&owner,&config)==ESP_OK);
    assert(solar_os_input_route_claim(&guest,&config)==ESP_ERR_INVALID_STATE);
    assert(solar_os_input_capture_keyboard("real",&guest)==ESP_OK);
    assert(solar_os_input_write_key(real,1,0x2c,' ',SOLAR_OS_INPUT_MOD_RIGHT_CTRL,SOLAR_OS_INPUT_KEY_PRESS)==ESP_OK);
    assert(routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY).data.key.key==' ');
    assert(solar_os_input_write_key(real,1,0x2c,' ',0,SOLAR_OS_INPUT_KEY_RELEASE)==ESP_OK);
    assert(routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY).data.key.action==SOLAR_OS_INPUT_KEY_RELEASE);
    solar_os_input_key_event_t key,events[4];bool reset;
    assert(!solar_os_input_read_captured_key(&guest,&key,&reset));
    /* A key held by the app before the overlay opens keeps its release. */
    assert(solar_os_input_write_key(real,9,0x52,SOLAR_OS_KEY_UP,0,SOLAR_OS_INPUT_KEY_PRESS)==ESP_OK);
    assert(solar_os_input_read_captured_key(&guest,&key,&reset) && key.action==SOLAR_OS_INPUT_KEY_PRESS);
    assert(solar_os_input_read_events(events,4)==0);

    config.navigation=config.visible=true;
    assert(solar_os_input_route_update(&owner,&config)==ESP_OK);
    assert(solar_os_input_write_key(real,9,0x52,SOLAR_OS_KEY_UP,0,SOLAR_OS_INPUT_KEY_RELEASE)==ESP_OK);
    assert(solar_os_input_read_captured_key(&guest,&key,&reset) && key.action==SOLAR_OS_INPUT_KEY_RELEASE);
    no_routed(&owner);
    assert(solar_os_input_write_key_tap(real,0,0x52,SOLAR_OS_KEY_UP,SOLAR_OS_INPUT_MOD_LEFT_SHIFT)==ESP_OK);
    assert(solar_os_input_read_captured_key(&guest,&key,&reset) && key.modifiers==SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(solar_os_input_read_captured_key(&guest,&key,&reset) && key.action==SOLAR_OS_INPUT_KEY_RELEASE);
    no_routed(&owner);
    assert(solar_os_input_write_key(real,2,0x52,SOLAR_OS_KEY_UP,0,SOLAR_OS_INPUT_KEY_PRESS)==ESP_OK);
    (void)routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY);
    config.navigation=config.visible=false;
    assert(solar_os_input_route_update(&owner,&config)==ESP_OK);
    assert(solar_os_input_write_key(real,2,0x52,SOLAR_OS_KEY_UP,0,SOLAR_OS_INPUT_KEY_RELEASE)==ESP_OK);
    (void)routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY);
    assert(!solar_os_input_read_captured_key(&guest,&key,&reset));
    /* Generated Enter bypasses the overlay navigation route. */
    config.navigation=config.visible=true;
    assert(solar_os_input_route_update(&owner,&config)==ESP_OK);
    assert(solar_os_input_write_key_tap(virtual,0,0x28,'\r',0)==ESP_OK);
    assert(solar_os_input_read_events(events,4)==2);
    assert(events[0].source==virtual && events[0].key=='\r');no_routed(&owner);
    /* Ordinary typing remains with the guest's exclusive capture. */
    assert(solar_os_input_write_key_tap(real,0,4,'a',0)==ESP_OK);
    assert(solar_os_input_read_captured_key(&guest,&key,&reset) && key.key=='a');
    assert(solar_os_input_read_captured_key(&guest,&key,&reset) && key.action==SOLAR_OS_INPUT_KEY_RELEASE);
    no_routed(&owner);

    /* A source-local physical ID is independent of the translated key. */
    config.trigger_physical_key=42;config.trigger_source=real;config.trigger_key=0;
    config.trigger_modifiers=0;config.navigation=config.visible=false;
    assert(solar_os_input_route_update(&owner,&config)==ESP_OK);
    assert(solar_os_input_write_key_tap(real,42,0,'z',0)==ESP_OK);
    assert(routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY).data.key.physical_key==42);
    (void)routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY);
    assert(solar_os_input_write_key_tap(real,41,0,'z',0)==ESP_OK);
    assert(solar_os_input_read_captured_key(&guest,&key,&reset));
    assert(solar_os_input_read_captured_key(&guest,&key,&reset));
    assert(solar_os_input_write_key_tap(other,42,0,'z',0)==ESP_OK);
    assert(solar_os_input_read_events(events,4)==2 && events[0].source==other);
    no_routed(&owner);
    config.trigger_physical_key=0;config.trigger_source=0;

    /* Character-only sources must not remain captured after a trigger changes. */
    config.trigger_key='=';config.trigger_modifiers=0;config.navigation=config.visible=false;
    assert(solar_os_input_route_update(&owner,&config)==ESP_OK);
    assert(solar_os_input_write_char(real,'=')==ESP_OK);
    (void)routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY);
    assert(routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY).data.key.action==SOLAR_OS_INPUT_KEY_RELEASE);
    config.trigger_key=0;
    assert(solar_os_input_route_update(&owner,&config)==ESP_OK);
    assert(solar_os_input_write_char(real,'=')==ESP_OK);
    assert(solar_os_input_read_captured_key(&guest,&key,&reset) && key.key=='=');no_routed(&owner);
    solar_os_input_release_keyboard(&guest);

    /* Reserved touch strokes precede exclusive application capture. */
    solar_os_input_source_t captured;
    assert(solar_os_input_capture_pointer("touch",&guest,&captured)==ESP_OK);
    pointer(touch,100,SOLAR_OS_INPUT_POINTER_PRESS,160,239);
    pointer(touch,200,SOLAR_OS_INPUT_POINTER_RELEASE,160,159);
    assert(routed(&owner,SOLAR_OS_INPUT_ROUTE_POINTER).data.pointer.event.action==SOLAR_OS_INPUT_POINTER_PRESS);
    assert(routed(&owner,SOLAR_OS_INPUT_ROUTE_POINTER).data.pointer.event.action==SOLAR_OS_INPUT_POINTER_RELEASE);
    solar_os_input_route_event_t gesture=routed(&owner,SOLAR_OS_INPUT_ROUTE_GESTURE);
    assert(gesture.data.gesture.gesture==SOLAR_OS_INPUT_GESTURE_EDGE_SWIPE &&
        gesture.data.gesture.direction==SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH);
    solar_os_input_pointer_sample_t sample;
    assert(solar_os_input_read_captured_pointer(touch,&guest,&sample,&reset)==ESP_ERR_NOT_FOUND);
    solar_os_input_pointer_event_t p;
    assert(solar_os_input_read_pointer_events(&p,1)==0);
    solar_os_input_gesture_event_t g;
    assert(solar_os_input_read_gesture_events(&g,1)==0);
    solar_os_input_source_diagnostics_t diagnostics;
    assert(solar_os_input_source_get_diagnostics(touch,&diagnostics));
    assert(diagnostics.last_gesture.gesture==SOLAR_OS_INPUT_GESTURE_EDGE_SWIPE);
    pointer(touch,300,SOLAR_OS_INPUT_POINTER_PRESS,160,100);
    pointer(touch,400,SOLAR_OS_INPUT_POINTER_RELEASE,160,60);
    assert(solar_os_input_read_captured_pointer(touch,&guest,&sample,&reset)==ESP_OK);
    assert(solar_os_input_read_captured_pointer(touch,&guest,&sample,&reset)==ESP_OK);
    no_routed(&owner);
    /* A stroke that starts in the keyboard stays reserved after it hides. */
    config.visible=config.navigation=true;
    assert(solar_os_input_route_update(&owner,&config)==ESP_OK);
    pointer(touch,500,SOLAR_OS_INPUT_POINTER_PRESS,160,180);
    (void)routed(&owner,SOLAR_OS_INPUT_ROUTE_POINTER);
    pointer_contact(touch,510,SOLAR_OS_INPUT_POINTER_PRESS,20,30,1);
    solar_os_input_route_event_t cancelled;
    assert(!solar_os_input_route_read(&owner,&cancelled,&reset) && reset);
    pointer(touch,520,SOLAR_OS_INPUT_POINTER_RELEASE,160,180);
    pointer_contact(touch,530,SOLAR_OS_INPUT_POINTER_RELEASE,20,30,1);
    assert(solar_os_input_read_captured_pointer(touch,&guest,&sample,&reset)==ESP_ERR_NOT_FOUND);
    no_routed(&owner);
    /* A batch that began in the app stays there, including a second finger
     * over the keyboard. No unmatched releases can cross owners. */
    pointer(touch,700,SOLAR_OS_INPUT_POINTER_PRESS,160,50);
    pointer_contact(touch,710,SOLAR_OS_INPUT_POINTER_PRESS,160,180,1);
    pointer(touch,720,SOLAR_OS_INPUT_POINTER_RELEASE,160,50);
    pointer_contact(touch,730,SOLAR_OS_INPUT_POINTER_RELEASE,160,180,1);
    for (unsigned i=0;i<4;i++)
        assert(solar_os_input_read_captured_pointer(touch,&guest,&sample,&reset)==ESP_OK);
    no_routed(&owner);
    pointer(touch,900,SOLAR_OS_INPUT_POINTER_PRESS,160,180);
    (void)routed(&owner,SOLAR_OS_INPUT_ROUTE_POINTER);
    solar_os_input_route_release(&owner);
    pointer(touch,1000,SOLAR_OS_INPUT_POINTER_RELEASE,160,180);
    assert(solar_os_input_read_captured_pointer(touch,&guest,&sample,&reset)==ESP_ERR_NOT_FOUND);
    now_us=1400000;solar_os_input_poll();
    assert(solar_os_input_read_gesture_events(&g,1)==0);
    solar_os_input_release_pointer(touch,&guest);

    /* Releasing/reclaiming a route cannot replay stale key releases. */
    config.trigger_key=' ';config.trigger_modifiers=SOLAR_OS_INPUT_MOD_LEFT_CTRL;
    config.visible=config.navigation=false;
    assert(solar_os_input_route_claim(&owner,&config)==ESP_OK);
    assert(solar_os_input_write_key(real,3,0x2c,' ',SOLAR_OS_INPUT_MOD_LEFT_CTRL,SOLAR_OS_INPUT_KEY_PRESS)==ESP_OK);
    (void)routed(&owner,SOLAR_OS_INPUT_ROUTE_KEY);
    solar_os_input_route_release(&owner);
    assert(solar_os_input_route_claim(&guest,&config)==ESP_OK);
    assert(solar_os_input_write_key(real,3,0x2c,' ',0,SOLAR_OS_INPUT_KEY_RELEASE)==ESP_OK);
    no_routed(&guest);
    assert(solar_os_input_read_events(events,4)==0);
    solar_os_input_route_release(&guest);
    solar_os_input_source_close(real);solar_os_input_source_close(virtual);solar_os_input_source_close(touch);
    solar_os_input_source_close(other);
    puts("OS input route tests: ok");
}
