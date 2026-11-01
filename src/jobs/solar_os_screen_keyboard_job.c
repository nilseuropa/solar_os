#include "solar_os_screen_keyboard_job.h"

#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_display.h"
#include "solar_os_display_layer.h"
#include "solar_os_fonts.h"
#include "solar_os_input_route.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_screen_keyboard_layout.h"
#include "solar_os_sessions.h"
#include "solar_os_shell_io.h"

#define SCREEN_KEYBOARD_NAME "screen-keyboard"
enum { REQUEST_NONE, REQUEST_SHOW, REQUEST_HIDE, REQUEST_TOGGLE, REQUEST_FULL, REQUEST_SYMBOLS };
typedef struct {
    const solar_os_screen_keyboard_layout_t *layout;
    void *layout_state;
    solar_os_input_route_config_t route;
    char trigger_source_name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    char touch_source_name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    solar_os_screen_keyboard_rect_t panel;
    u8g2_t canvas;
    uint8_t *pixels, *bits;
    uint8_t modifiers;
    bool caps, symbols, auto_hide, visible, dirty, contact, cancel_contact, routed;
    solar_os_input_source_t pointer_source;
    uint8_t pointer_id;
    uint8_t session_id;
    bool last_ui_stroke;
    solar_os_input_source_t last_touch_source;
    unsigned request;
    esp_err_t last_error;
    uint32_t emitted;
} screen_keyboard_t;
static screen_keyboard_t *keyboard;
static portMUX_TYPE keyboard_lock=portMUX_INITIALIZER_UNLOCKED;

static void update_route(screen_keyboard_t *s)
{
    s->route.visible=s->visible;
    s->route.navigation=s->visible;
    s->route.x=s->panel.x; s->route.y=s->panel.y;
    s->route.width=s->panel.width; s->route.height=s->panel.height;
    (void)solar_os_input_route_update(s,&s->route);
}

static void redraw_app(screen_keyboard_t *s)
{
    uint8_t id;
    const solar_os_event_t event={.type=SOLAR_OS_EVENT_RESUME,
        .data.tick_ms=(uint32_t)(esp_timer_get_time()/1000)};
    if (solar_os_sessions_active_for_display(s->route.target,&id))
        (void)solar_os_sessions_dispatch_session_event(id,&event);
    solar_os_sessions_mark_foreground_dirty();
}

void solar_os_screen_keyboard_hide(void)
{
    portENTER_CRITICAL(&keyboard_lock);
    screen_keyboard_t *s=keyboard;
    portEXIT_CRITICAL(&keyboard_lock);
    if (!s || !s->visible) return;
    s->visible=false; s->modifiers=0; s->caps=false;
    s->contact=s->cancel_contact=s->last_ui_stroke=false;
    s->layout->cancel(s->layout_state);
    update_route(s);
    (void)solar_os_display_clear_layer(s->route.target,SCREEN_KEYBOARD_NAME);
    solar_os_input_source_release_all(s->route.excluded_source);
    solar_os_memory_free(s->pixels); s->pixels=NULL; s->bits=NULL;
}

typedef struct {
    u8g2_t *canvas;
    size_t font;
    bool measuring;
} keyboard_paint_t;
static const uint8_t *const keyboard_fonts[]={
    u8g2_font_solar_os_default_b_20_tf,u8g2_font_solar_os_default_b_18_tf,
    u8g2_font_solar_os_default_b_16_tf,u8g2_font_solar_os_default_b_14_tf,
    u8g2_font_solar_os_default_b_12_tf,u8g2_font_solar_os_default_b_10_tf,
    u8g2_font_5x8_tf,
};
static int glyph_width(solar_os_screen_keyboard_glyph_t glyph,int height)
{
    return glyph==SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SPACE || glyph==SOLAR_OS_SCREEN_KEYBOARD_GLYPH_ENTER ||
        glyph==SOLAR_OS_SCREEN_KEYBOARD_GLYPH_BACKSPACE ? height*3/2 : height;
}
static void measure_label(keyboard_paint_t *p,solar_os_screen_keyboard_rect_t r,
    const char *text,solar_os_screen_keyboard_glyph_t glyph)
{
    for (;;) {
        u8g2_SetFont(p->canvas,keyboard_fonts[p->font]);
        int height=u8g2_GetAscent(p->canvas)-u8g2_GetDescent(p->canvas);
        int width=text ? u8g2_GetStrWidth(p->canvas,text) : glyph_width(glyph,height);
        if ((width<=r.width-4 && height<=r.height-4) ||
            p->font+1==sizeof(keyboard_fonts)/sizeof(keyboard_fonts[0])) return;
        p->font++;
    }
}
static void paint_box(void *context,solar_os_screen_keyboard_rect_t r,bool filled)
{
    keyboard_paint_t *p=context;
    if (p->measuring) return;
    u8g2_t *canvas=p->canvas;
    u8g2_SetDrawColor(canvas,1);
    if (filled) u8g2_DrawBox(canvas,r.x,r.y,r.width,r.height);
    else u8g2_DrawFrame(canvas,r.x,r.y,r.width,r.height);
}
static void paint_text(void *context,solar_os_screen_keyboard_rect_t r,const char *text,bool inverse)
{
    keyboard_paint_t *p=context;
    if (p->measuring) {measure_label(p,r,text,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_NONE);return;}
    u8g2_t *canvas=p->canvas;
    int width=u8g2_GetStrWidth(canvas,text),ascent=u8g2_GetAscent(canvas);
    int height=ascent-u8g2_GetDescent(canvas);
    u8g2_SetClipWindow(canvas,r.x,r.y,r.x+r.width,r.y+r.height);
    u8g2_SetDrawColor(canvas,inverse ? 0 : 1);
    u8g2_DrawStr(canvas,r.x+(r.width-width)/2,r.y+(r.height-height)/2+ascent,text);
    u8g2_SetMaxClipWindow(canvas);
}
static void draw_stroke(u8g2_t *canvas,int x0,int y0,int x1,int y1,unsigned weight)
{
    int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,error=dx+dy;
    for (;;) {
        u8g2_DrawBox(canvas,x0,y0,weight,weight);
        if (x0==x1 && y0==y1) break;
        int step=2*error;
        if (step>=dy) {error+=dy;x0+=sx;}
        if (step<=dx) {error+=dx;y0+=sy;}
    }
}
static void paint_line(void *context,int x0,int y0,int x1,int y1)
{
    keyboard_paint_t *p=context;
    if (p->measuring) return;
    u8g2_SetDrawColor(p->canvas,1);draw_stroke(p->canvas,x0,y0,x1,y1,1);
}
static void draw_arrow(u8g2_t *canvas,int x0,int y0,int x1,int y1,int head,unsigned weight)
{
    draw_stroke(canvas,x0,y0,x1,y1,weight);
    if (x0==x1) {
        int base=y1+(y0<y1 ? -head : head);
        draw_stroke(canvas,x1,y1,x1-head,base,weight);
        draw_stroke(canvas,x1,y1,x1+head,base,weight);
    } else {
        int base=x1+(x0<x1 ? -head : head);
        draw_stroke(canvas,x1,y1,base,y1-head,weight);
        draw_stroke(canvas,x1,y1,base,y1+head,weight);
    }
}
static void draw_polyline(u8g2_t *canvas,const int points[][2],size_t count,unsigned weight)
{
    for (size_t i=1;i<count;i++)
        draw_stroke(canvas,points[i-1][0],points[i-1][1],points[i][0],points[i][1],weight);
}
static void paint_glyph(void *context,solar_os_screen_keyboard_rect_t r,
    solar_os_screen_keyboard_glyph_t glyph,bool inverse)
{
    keyboard_paint_t *p=context;
    if (p->measuring) {measure_label(p,r,NULL,glyph);return;}
    u8g2_t *canvas=p->canvas;
    int height=u8g2_GetAscent(canvas)-u8g2_GetDescent(canvas);
    if (height>r.height-4) height=r.height-4;
    if (height<3) height=3;
    int width=glyph_width(glyph,height);
    if (width>r.width-4) width=r.width-4;
    int left=r.x+(r.width-width)/2,right=left+width-2;
    int top=r.y+(r.height-height)/2,bottom=top+height-2;
    int cx=(left+right)/2,cy=(top+bottom)/2,head=height/3;
    unsigned weight=height>=12 ? 2U : 1U;
    u8g2_SetClipWindow(canvas,r.x,r.y,r.x+r.width,r.y+r.height);
    u8g2_SetDrawColor(canvas,inverse?0:1);
    switch (glyph) {
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SPACE: {
        const int points[][2]={{left,bottom-head},{left,bottom},{right,bottom},{right,bottom-head}};
        draw_polyline(canvas,points,4,weight);break;
    }
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_ENTER:
        draw_stroke(canvas,right,top,right,cy,weight);
        draw_arrow(canvas,right,cy,left,cy,head,weight);break;
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_BACKSPACE: {
        const int points[][2]={{left,cy},{left+head,top},{right,top},{right,bottom},
            {left+head,bottom},{left,cy}};
        draw_polyline(canvas,points,6,weight);
        int middle=(left+head+right)/2,cross=head/2;
        draw_stroke(canvas,middle-cross,cy-cross,middle+cross,cy+cross,weight);
        draw_stroke(canvas,middle-cross,cy+cross,middle+cross,cy-cross,weight);break;
    }
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SHIFT: {
        int shaft=height/6;
        const int points[][2]={{cx,top},{right,cy},{cx+shaft,cy},{cx+shaft,bottom},
            {cx-shaft,bottom},{cx-shaft,cy},{left,cy},{cx,top}};
        draw_polyline(canvas,points,8,weight);break;
    }
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_TAB:
        draw_arrow(canvas,left,cy,right-3,cy,head,weight);
        draw_stroke(canvas,right,top,right,bottom,weight);break;
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_LEFT:
        draw_arrow(canvas,right,cy,left,cy,head,weight);break;
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_RIGHT:
        draw_arrow(canvas,left,cy,right,cy,head,weight);break;
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_UP:
        draw_arrow(canvas,cx,bottom,cx,top,head,weight);break;
    case SOLAR_OS_SCREEN_KEYBOARD_GLYPH_DOWN:
        draw_arrow(canvas,cx,top,cx,bottom,head,weight);break;
    default: break;
    }
    u8g2_SetMaxClipWindow(canvas);
}

static esp_err_t draw(screen_keyboard_t *s)
{
    u8g2_ClearBuffer(&s->canvas);
    u8g2_SetFontRefHeightText(&s->canvas);
    u8g2_SetFontPosBaseline(&s->canvas);
    u8g2_SetFontMode(&s->canvas,1);
    keyboard_paint_t paint={.canvas=&s->canvas,.measuring=true};
    const solar_os_screen_keyboard_painter_t painter={.context=&paint,
        .box=paint_box,.text=paint_text,.glyph=paint_glyph,.line=paint_line};
    s->layout->draw(s->layout_state,&painter,s->modifiers,s->caps);
    paint.measuring=false;
    u8g2_SetFont(&s->canvas,keyboard_fonts[paint.font]);
    s->layout->draw(s->layout_state,&painter,s->modifiers,s->caps);
    solar_os_display_canvas_pack(&s->canvas,s->bits,s->panel.x,s->panel.y,s->panel.width,s->panel.height);
    return solar_os_display_set_layer(s->route.target,SCREEN_KEYBOARD_NAME,s->bits,
        (size_t)((s->panel.width+7)/8)*s->panel.height,
        s->panel.x,s->panel.y,s->panel.width,s->panel.height);
}

static esp_err_t show(screen_keyboard_t *s)
{
    if (s->visible) return ESP_OK;
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(s->route.target,&target) || !target.ready || !target.u8g2)
        return ESP_ERR_NOT_FOUND;
    if (solar_os_sessions_focus_display(s->route.target)!=ESP_OK) return ESP_ERR_INVALID_STATE;
    if (!solar_os_sessions_active_for_display(s->route.target,&s->session_id)) return ESP_ERR_INVALID_STATE;
    int width=u8g2_GetDisplayWidth(target.u8g2),height=u8g2_GetDisplayHeight(target.u8g2);
    if (width<80 || height<64) return ESP_ERR_NOT_SUPPORTED;
    s->panel=s->layout->resize(s->layout_state,width,height);
    size_t mono=(size_t)u8g2_GetBufferTileWidth(target.u8g2)*u8g2_GetBufferTileHeight(target.u8g2)*8U;
    size_t bitmap=(size_t)((s->panel.width+7)/8)*s->panel.height;
    s->pixels=solar_os_memory_calloc(1,mono+bitmap,SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"screen-keyboard.canvas");
    if (!s->pixels) return ESP_ERR_NO_MEM;
    s->canvas=*target.u8g2;
    s->canvas.tile_buf_ptr=s->pixels;
    s->bits=s->pixels+mono;
    s->layout->cancel(s->layout_state);
    s->visible=true;
    update_route(s);
    esp_err_t err=draw(s);
    s->dirty=false;
    if (err!=ESP_OK) solar_os_screen_keyboard_hide();
    else redraw_app(s);
    return err;
}

static void apply(screen_keyboard_t *s,solar_os_screen_keyboard_action_t action)
{
    if (action.type==SOLAR_OS_SCREEN_KEYBOARD_HIDE) solar_os_screen_keyboard_hide();
    else if (action.type==SOLAR_OS_SCREEN_KEYBOARD_MODIFIER) s->modifiers^=action.modifier;
    else if (action.type==SOLAR_OS_SCREEN_KEYBOARD_CAPS) s->caps=!s->caps;
    else if (action.type==SOLAR_OS_SCREEN_KEYBOARD_EMIT) {
        uint8_t key=action.key,mods=s->modifiers;
        bool shift=(mods&SOLAR_OS_INPUT_MOD_SHIFT)!=0;
        if (key>='a' && key<='z') shift^=s->caps;
        if (shift && action.shifted) key=action.shifted;
        if (shift) mods|=SOLAR_OS_INPUT_MOD_LEFT_SHIFT;
        else mods&=(uint8_t)~SOLAR_OS_INPUT_MOD_SHIFT;
        mods|=action.modifier;
        s->last_error=solar_os_input_write_key_tap(s->route.excluded_source,
            SOLAR_OS_INPUT_PHYSICAL_NONE,action.usage,key,mods);
        if (s->last_error==ESP_OK) {
            s->emitted++;s->modifiers=0;
            if (s->auto_hide) solar_os_screen_keyboard_hide();
        }
    }
    if (action.type!=SOLAR_OS_SCREEN_KEYBOARD_NONE) s->dirty=true;
}

static unsigned modifiers(uint8_t mods)
{
    return ((mods&SOLAR_OS_INPUT_MOD_CTRL)?1U:0U)|((mods&SOLAR_OS_INPUT_MOD_SHIFT)?2U:0U)|
        ((mods&SOLAR_OS_INPUT_MOD_ALT)?4U:0U)|
        ((mods&(SOLAR_OS_INPUT_MOD_LEFT_GUI|SOLAR_OS_INPUT_MOD_RIGHT_GUI))?8U:0U);
}
static bool trigger(screen_keyboard_t *s,const solar_os_input_key_event_t *e)
{
    return (!s->route.trigger_source || s->route.trigger_source==e->source) &&
        (s->route.trigger_physical_key ? s->route.trigger_physical_key==e->physical_key :
            s->route.trigger_key && s->route.trigger_key==e->key) &&
        modifiers(s->route.trigger_modifiers)==modifiers(e->modifiers);
}
static void toggle(screen_keyboard_t *s)
{
    if (s->visible) solar_os_screen_keyboard_hide();
    else s->last_error=show(s);
}

static void pump(screen_keyboard_t *s)
{
    solar_os_display_target_t target;
    solar_os_terminal_profile_t profile;
    if (!solar_os_display_find_target(s->route.target,&target) ||
        solar_os_display_get_terminal_profile(s->route.target,&profile)!=ESP_OK) {
        solar_os_screen_keyboard_hide();
        solar_os_input_route_release(s);s->routed=false;s->last_error=ESP_ERR_NOT_FOUND;
        return;
    }
    char focus[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    uint8_t session_id=0;
    if (s->visible && ((!solar_os_display_has_layer(s->route.target,SCREEN_KEYBOARD_NAME)) ||
        !solar_os_sessions_active_for_display(s->route.target,&session_id) || session_id!=s->session_id ||
        (solar_os_sessions_input_focus(focus,sizeof(focus)) && strcmp(focus,s->route.target))))
        solar_os_screen_keyboard_hide();
    s->route.native_width=target.native_width ? target.native_width : target.width;
    s->route.native_height=target.native_height ? target.native_height : target.height;
    s->route.orientation=profile.orientation_degrees;
    s->route.scale=profile.scale ? profile.scale : 1;
    solar_os_input_source_info_t source;
    if (s->trigger_source_name[0])
        s->route.trigger_source=solar_os_input_source_find(s->trigger_source_name,&source) &&
            (source.capabilities&SOLAR_OS_INPUT_CAP_KEY_EVENTS) ? source.source : UINT8_MAX;
    if (s->touch_source_name[0])
        s->route.touch_source=solar_os_input_source_find(s->touch_source_name,&source) &&
            source.source_class==SOLAR_OS_INPUT_SOURCE_TOUCH ? source.source : UINT8_MAX;
    if (!s->routed) {
        s->last_error=solar_os_input_route_claim(s,&s->route);
        s->routed=s->last_error==ESP_OK;
        if (!s->routed) return;
    }
    update_route(s);
    portENTER_CRITICAL(&keyboard_lock);
    unsigned request=s->request;s->request=REQUEST_NONE;
    portEXIT_CRITICAL(&keyboard_lock);
    if (request==REQUEST_SHOW) s->last_error=show(s);
    else if (request==REQUEST_HIDE) solar_os_screen_keyboard_hide();
    else if (request==REQUEST_TOGGLE) toggle(s);
    else if (request==REQUEST_FULL || request==REQUEST_SYMBOLS) {
        bool visible=s->visible;
        solar_os_screen_keyboard_hide();
        s->symbols=request==REQUEST_SYMBOLS;
        s->layout->init(s->layout_state,s->symbols);
        if (visible) s->last_error=show(s);
    }
    for (unsigned n=0;n<64;n++) {
        solar_os_input_route_event_t event;
        bool reset=false;
        bool have=solar_os_input_route_read(s,&event,&reset);
        if (reset) {
            s->layout->cancel(s->layout_state);s->modifiers=0;
            s->contact=s->cancel_contact=s->last_ui_stroke=false;s->dirty=true;
        }
        if (!have) break;
        if (event.type==SOLAR_OS_INPUT_ROUTE_KEY) {
            const solar_os_input_key_event_t *e=&event.data.key;
            if (e->action==SOLAR_OS_INPUT_KEY_RELEASE) continue;
            if (trigger(s,e)) {if(e->action==SOLAR_OS_INPUT_KEY_PRESS)toggle(s);continue;}
            if (!s->visible) continue;
            if (e->key==SOLAR_OS_KEY_ESCAPE) solar_os_screen_keyboard_hide();
            else if (e->action==SOLAR_OS_INPUT_KEY_PRESS ||
                (e->key!= '\r' && e->key!='\n' && e->key!='\t'))
                {apply(s,s->layout->key(s->layout_state,e->key));s->dirty=true;}
        } else if (event.type==SOLAR_OS_INPUT_ROUTE_POINTER) {
            solar_os_input_pointer_event_t e=event.data.pointer.event;
            if (!s->visible) {s->last_ui_stroke=false;continue;}
            uint16_t width=s->route.native_width,height=s->route.native_height;
            solar_os_input_source_diagnostics_t diagnostics;
            if (solar_os_input_source_get_diagnostics(e.source,&diagnostics) && diagnostics.calibration_enabled) {
                width=diagnostics.calibration.width;height=diagnostics.calibration.height;
            }
            (void)solar_os_input_pointer_apply_display_transform(&e,width,
                height,s->route.orientation,s->route.scale);
            if (e.action==SOLAR_OS_INPUT_POINTER_PRESS) {
                if (s->contact) {s->layout->cancel(s->layout_state);s->cancel_contact=true;s->contact=false;continue;}
                s->last_ui_stroke=e.x>=s->panel.x && e.x<s->panel.x+s->panel.width &&
                    e.y>=s->panel.y && e.y<s->panel.y+s->panel.height;
                s->last_touch_source=e.source;
                s->contact=s->last_ui_stroke;s->pointer_source=e.source;s->pointer_id=e.pointer_id;
            }
            if (s->cancel_contact) {if(e.action==SOLAR_OS_INPUT_POINTER_RELEASE)s->cancel_contact=false;continue;}
            if (s->contact && e.source==s->pointer_source && e.pointer_id==s->pointer_id) {
                apply(s,s->layout->pointer(s->layout_state,e.action,e.x,e.y));
                if (e.action!=SOLAR_OS_INPUT_POINTER_MOVE) s->dirty=true;
                if (e.action==SOLAR_OS_INPUT_POINTER_RELEASE) s->contact=false;
            }
        } else {
            const solar_os_input_gesture_event_t *e=&event.data.gesture;
            if (s->route.edge_enabled && e->gesture==SOLAR_OS_INPUT_GESTURE_EDGE_SWIPE &&
                e->direction==s->route.edge_direction) toggle(s);
            else if (s->visible && s->last_ui_stroke && e->source==s->last_touch_source &&
                e->gesture==SOLAR_OS_INPUT_GESTURE_SWIPE && e->direction==SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH)
                solar_os_screen_keyboard_hide();
        }
    }
    if (s->visible && s->dirty) {
        s->dirty=false;s->last_error=draw(s);
        if (s->last_error!=ESP_OK) solar_os_screen_keyboard_hide();
    }
}

static esp_err_t stop_on_owner(void *context)
{
    (void)context;
    screen_keyboard_t *s=keyboard;
    if (!s) return ESP_OK;
    solar_os_screen_keyboard_hide();
    solar_os_input_route_release(s);
    solar_os_input_source_close(s->route.excluded_source);
    portENTER_CRITICAL(&keyboard_lock);keyboard=NULL;portEXIT_CRITICAL(&keyboard_lock);
    solar_os_memory_free(s->layout_state);solar_os_memory_free(s);
    return ESP_OK;
}

static void stop(solar_os_context_t *ctx)
{
    (void)ctx;
    (void)solar_os_sessions_run_on_owner(stop_on_owner,NULL);
}

static esp_err_t start(solar_os_context_t *ctx,int argc,char **argv)
{
    (void)ctx;
    screen_keyboard_t *s=solar_os_memory_calloc(1,sizeof(*s),SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,SCREEN_KEYBOARD_NAME);
    if (!s) return ESP_ERR_NO_MEM;
    const char *layout="grid",*source=NULL,*touch=NULL;
    bool have_trigger=false,visible=false;
    s->route.trigger_key=' ';s->route.trigger_modifiers=SOLAR_OS_INPUT_MOD_LEFT_CTRL;
    s->route.edge_enabled=true;s->route.edge_direction=SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH;
    esp_err_t err=ESP_OK;
    for (int i=1;i<argc && err==ESP_OK;i++) {
        const char *arg=argv[i],*value=strchr(arg,'=');
        if (!value || !value[1]) {err=ESP_ERR_INVALID_ARG;break;}
        value++;
        if (!strncmp(arg,"mode=",5)) {
            if (strcmp(value,"full") && strcmp(value,"symbols")) err=ESP_ERR_INVALID_ARG;
            s->symbols=!strcmp(value,"symbols");
        } else if (!strncmp(arg,"layout=",7)) layout=value;
        else if (!strncmp(arg,"display=",8)) {
            if (strlen(value)>=sizeof(s->route.target)) err=ESP_ERR_INVALID_ARG;
            else strlcpy(s->route.target,value,sizeof(s->route.target));
        } else if (!strncmp(arg,"trigger=",8)) {
            have_trigger=true;
            if (!strcmp(value,"none")) s->route.trigger_key=s->route.trigger_modifiers=0;
            else if (!solar_os_input_parse_key_chord(value,&s->route.trigger_key,&s->route.trigger_modifiers)) err=ESP_ERR_INVALID_ARG;
        } else if (!strncmp(arg,"source=",7)) source=value;
        else if (!strncmp(arg,"touch-source=",13)) touch=value;
        else if (!strncmp(arg,"key-id=",7)) {
            char *end=NULL;unsigned long id=strtoul(value,&end,0);
            if (!id || id>UINT16_MAX || !end || *end) err=ESP_ERR_INVALID_ARG;
            else s->route.trigger_physical_key=id;
        } else if (!strncmp(arg,"gesture=",8)) {
            if (strcmp(value,"none") && strcmp(value,"edge-swipe")) err=ESP_ERR_INVALID_ARG;
            s->route.edge_enabled=!strcmp(value,"edge-swipe");
        } else if (!strncmp(arg,"direction=",10)) {
            if (!solar_os_input_parse_gesture_direction(value,&s->route.edge_direction) ||
                s->route.edge_direction<SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST ||
                s->route.edge_direction>SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH) err=ESP_ERR_INVALID_ARG;
        } else if (!strncmp(arg,"auto-hide=",10) || !strncmp(arg,"visible=",8)) {
            if (strcmp(value,"on") && strcmp(value,"off")) err=ESP_ERR_INVALID_ARG;
            if (arg[0]=='a') s->auto_hide=!strcmp(value,"on");else visible=!strcmp(value,"on");
        } else err=ESP_ERR_INVALID_ARG;
    }
    solar_os_input_source_info_t info;
    if (err==ESP_OK && source) {
        if (!solar_os_input_source_find(source,&info) || !(info.capabilities&SOLAR_OS_INPUT_CAP_KEY_EVENTS)) err=ESP_ERR_NOT_FOUND;
        else {
            s->route.trigger_source=info.source;
            strlcpy(s->trigger_source_name,source,sizeof(s->trigger_source_name));
        }
    }
    if (err==ESP_OK && touch) {
        if (!solar_os_input_source_find(touch,&info) || info.source_class!=SOLAR_OS_INPUT_SOURCE_TOUCH) err=ESP_ERR_NOT_FOUND;
        else {
            s->route.touch_source=info.source;
            strlcpy(s->touch_source_name,touch,sizeof(s->touch_source_name));
        }
    }
    if (s->route.trigger_physical_key) {
        if (!source) err=ESP_ERR_INVALID_ARG;
        if (!have_trigger) s->route.trigger_key=s->route.trigger_modifiers=0;
    }
    if (!s->route.target[0] && !solar_os_sessions_input_focus(s->route.target,sizeof(s->route.target)))
        strlcpy(s->route.target,SOLAR_OS_DISPLAY_PRIMARY_TARGET,sizeof(s->route.target));
    solar_os_display_target_t target;
    solar_os_terminal_profile_t profile;
    if (err==ESP_OK && (!solar_os_display_find_target(s->route.target,&target) || !target.ready || !target.u8g2 ||
        solar_os_display_get_terminal_profile(s->route.target,&profile)!=ESP_OK)) err=ESP_ERR_NOT_FOUND;
    s->layout=solar_os_screen_keyboard_layout_find(layout);
    if (!s->layout) err=ESP_ERR_INVALID_ARG;
    if (err==ESP_OK) {
        s->route.native_width=target.native_width?target.native_width:target.width;
        s->route.native_height=target.native_height?target.native_height:target.height;
        s->route.orientation=profile.orientation_degrees;s->route.scale=profile.scale?profile.scale:1;
        s->layout_state=solar_os_memory_calloc(1,s->layout->state_size,SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"screen-keyboard.layout");
        if (!s->layout_state) err=ESP_ERR_NO_MEM;
        else s->layout->init(s->layout_state,s->symbols);
    }
    if (err==ESP_OK) err=solar_os_input_keyboard_source_open(SCREEN_KEYBOARD_NAME,true,&s->route.excluded_source);
    if (err==ESP_OK) err=solar_os_input_route_claim(s,&s->route);
    if (err!=ESP_OK) {
        if (s->route.excluded_source) solar_os_input_source_close(s->route.excluded_source);
        solar_os_memory_free(s->layout_state);solar_os_memory_free(s);return err;
    }
    s->routed=true;
    s->request=visible?REQUEST_SHOW:REQUEST_NONE;
    portENTER_CRITICAL(&keyboard_lock);keyboard=s;portEXIT_CRITICAL(&keyboard_lock);
    (void)solar_os_jobs_note_resource(SCREEN_KEYBOARD_NAME,SOLAR_OS_JOB_RESOURCE_CUSTOM,s->route.target,"OS overlay and virtual keyboard");
    return ESP_OK;
}
static bool event(solar_os_context_t *ctx,const solar_os_event_t *event)
{ (void)ctx;if(keyboard && event->type==SOLAR_OS_EVENT_TICK)pump(keyboard);return false; }
static void detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io=solar_os_context_shell_io(ctx);
    char target[SOLAR_OS_INPUT_POINTER_TARGET_MAX];
    const char *layout=NULL;
    bool symbols=false,visible=false;
    uint32_t emitted=0;
    esp_err_t error=ESP_OK;
    portENTER_CRITICAL(&keyboard_lock);
    screen_keyboard_t *s=keyboard;
    if (s) {
        strlcpy(target,s->route.target,sizeof(target));layout=s->layout->name;
        symbols=s->symbols;visible=s->visible;emitted=s->emitted;error=s->last_error;
    }
    portEXIT_CRITICAL(&keyboard_lock);
    if (io && !layout) solar_os_shell_io_writeln(io,"screen-keyboard: stopped");
    if (io && layout) solar_os_shell_io_printf(io,"  display=%s layout=%s mode=%s visible=%s emitted=%lu last_error=%s\n",
        target,layout,symbols?"symbols":"full",visible?"on":"off",
        (unsigned long)emitted,esp_err_to_name(error));
}
void solar_os_shell_cmd_screen_keyboard(solar_os_context_t *ctx,int argc,char **argv)
{
    solar_os_shell_io_t *io=solar_os_context_shell_io(ctx);
    if (argc==1 || (argc==2 && !strcmp(argv[1],"status"))) {detail(ctx);return;}
    unsigned request=REQUEST_NONE;
    if (argc==2) {
        if (!strcmp(argv[1],"show")) request=REQUEST_SHOW;
        else if (!strcmp(argv[1],"hide")) request=REQUEST_HIDE;
        else if (!strcmp(argv[1],"toggle")) request=REQUEST_TOGGLE;
    } else if (argc==3 && !strcmp(argv[1],"mode")) {
        if (!strcmp(argv[2],"full")) request=REQUEST_FULL;
        else if (!strcmp(argv[2],"symbols")) request=REQUEST_SYMBOLS;
    }
    if (!request) {solar_os_shell_io_writeln(io,"usage: screen-keyboard status|show|hide|toggle|mode full|symbols");return;}
    portENTER_CRITICAL(&keyboard_lock);
    bool running=keyboard!=NULL;
    if (running) keyboard->request=request;
    portEXIT_CRITICAL(&keyboard_lock);
    solar_os_shell_io_writeln(io,running?"screen-keyboard: request queued":"screen-keyboard: start the job first");
}
const solar_os_job_t solar_os_screen_keyboard_job={
    .name=SCREEN_KEYBOARD_NAME,.summary="touch and arrow-controlled screen keyboard",
    .kind=SOLAR_OS_JOB_KIND_BACKGROUND,.start=start,.stop=stop,.event=event,.detail=detail,
    .tick_interval_ms=20,.tick_deadline_ms=50,
};
