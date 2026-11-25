#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_memory.h"
#include "solar_os_script_media.h"
#include "solar_os_task.h"

struct solar_os_gfx { bool mono; };
struct test_task { pthread_t thread; TaskFunction_t fn; void *arg; bool internal; };
static _Thread_local bool internal_stack;
static atomic_uint allocations, opens, closes, acquired, released, direct_frames, indexed_frames;
static atomic_bool block_capture, in_capture, block_write, fail_write, busy_source, deny_save;
static unsigned finishes;
static bool literal_key_hint;
static int canvas_width=291,canvas_height=291;
static unsigned headers,shutters;
static solar_os_gfx_font_t font;
static solar_os_gfx_color_t color;
static char directory[160];
static ssize_t photo_test_write(int fd,const void *data,size_t size);
size_t strlcpy(char *to,const char *from,size_t size) {
    size_t n=strlen(from);if(size) { size_t bytes=n<size-1?n:size-1;memcpy(to,from,bytes);to[bytes]=0; }return n;
}
#define write photo_test_write
#include "../../src/apps/solar_os_photo.c"
#undef write

struct solar_os_script_media { bool open, leased, remote; uint8_t jpeg[8]; };
static ssize_t photo_test_write(int fd,const void *data,size_t size) {
    assert(internal_stack); /* Never enter a flash-backed VFS on the preview stack. */
    while (atomic_load(&block_write) && !stopping()) usleep(1000);
    if (atomic_load(&fail_write)) { errno=ENOSPC;return -1; }
    return write(fd,data,size);
}
int64_t esp_timer_get_time(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
void vTaskDelay(TickType_t ticks) { usleep(ticks*1000); }
void vTaskSuspend(TaskHandle_t task) { assert(!task);pthread_exit(NULL); }
static void *run_task(void *arg) {
    struct test_task *task=arg;internal_stack=task->internal;task->fn(task->arg);return NULL;
}
static BaseType_t create_task(TaskFunction_t fn,void *arg,TaskHandle_t *out,bool internal) {
    *out=calloc(1,sizeof(**out));assert(*out);(*out)->fn=fn;(*out)->arg=arg;(*out)->internal=internal;
    assert(!pthread_create(&(*out)->thread,NULL,run_task,*out));return pdPASS;
}
BaseType_t solar_os_task_create_pinned_external(TaskFunction_t fn,const char *name,uint32_t stack,
    void *arg,UBaseType_t priority,TaskHandle_t *out,BaseType_t core,solar_os_task_role_t role) {
    (void)priority;(void)core;assert(!strcmp(name,"photo") && stack==PHOTO_STACK && role==SOLAR_OS_TASK_ROLE_FOREGROUND);
    return create_task(fn,arg,out,false);
}
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t fn,const char *name,uint32_t stack,
    void *arg,UBaseType_t priority,TaskHandle_t *out,BaseType_t core,solar_os_task_role_t role) {
    (void)priority;(void)core;assert(!strcmp(name,"photo-save") && stack==PHOTO_SAVE_STACK && role==SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (atomic_load(&deny_save)) { *out=NULL;return 0; }
    return create_task(fn,arg,out,true);
}
void solar_os_task_delete_external(TaskHandle_t task) {
    /* FreeRTOS interprets NULL as deleting the caller, never as a no-op. */
    assert(task);assert(!pthread_join(task->thread,NULL));free(task);
}
void solar_os_task_delete_internal(TaskHandle_t task) { solar_os_task_delete_external(task); }
bool solar_os_task_wait_done(TaskHandle_t task,volatile bool *done,uint32_t ms) {
    (void)ms;if (!task) return true;
    for (unsigned n=0;n<20;n++) { if (__atomic_load_n(done,__ATOMIC_ACQUIRE)) return true;usleep(1000); }
    return false;
}
void *solar_os_memory_alloc(size_t size,solar_os_memory_class_t kind,const char *tag) {
    (void)tag;assert(kind==SOLAR_OS_MEMORY_EXTERNAL_REQUIRED);void *p=malloc(size);if (p) allocations++;return p;
}
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag) {
    void *p=solar_os_memory_alloc(n*size,kind,tag);if (p) memset(p,0,n*size);return p;
}
void solar_os_memory_free(void *p) { if (p) { assert(allocations);allocations--;free(p); } }
void solar_os_stb_image_free(void *p) { solar_os_memory_free(p); }
static esp_err_t decode(const uint8_t *data,size_t size,uint32_t max,uint8_t **p,uint32_t *w,uint32_t *h,unsigned bpp) {
    assert(data && size==8 && max==PHOTO_MAX_PIXELS);*w=*h=2;
    *p=solar_os_memory_alloc(4*bpp,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"test");memset(*p,data[0],4*bpp);return ESP_OK;
}
esp_err_t solar_os_stb_jpeg_decode_gray(const uint8_t *d,size_t n,uint32_t max,uint8_t **p,uint32_t *w,uint32_t *h)
{ return decode(d,n,max,p,w,h,1); }
esp_err_t solar_os_stb_decode_jpeg_rgb_scaled(const uint8_t *d,size_t n,uint32_t max,uint32_t mw,uint32_t mh,
    uint8_t **p,uint32_t *w,uint32_t *h) { assert(mw && mh);return decode(d,n,max,p,w,h,3); }
esp_err_t solar_os_stb_decode_jpeg_rgb565_scaled(const uint8_t *d,size_t n,uint32_t max,uint32_t mw,uint32_t mh,
    uint8_t **p,uint32_t *w,uint32_t *h) { assert(mw && mh);return decode(d,n,max,p,w,h,2); }
esp_err_t solar_os_script_media_create(const char *owner,solar_os_script_media_cancel_fn cancel,void *user,
    solar_os_script_media_t **out) { assert(!strcmp(owner,"app:photo") && cancel && !user);*out=calloc(1,sizeof(**out));return ESP_OK; }
void solar_os_script_media_destroy(solar_os_script_media_t *s) {
    if (!s) return;assert(!s->leased);if (s->open) closes++;free(s);
}
esp_err_t solar_os_script_media_open(solar_os_script_media_t *s,const char *id,
    const solar_os_stream_open_options_t *opt,uint32_t *handle) {
    assert(!strcmp(id,"camera0") && opt->requested_video.codec==SOLAR_OS_STREAM_VIDEO_JPEG);
    if (atomic_load(&busy_source)) return ESP_ERR_INVALID_STATE;
    s->open=true;opens++;*handle=1;return ESP_OK;
}
esp_err_t solar_os_script_media_acquire(solar_os_script_media_t *s,uint32_t id,uint32_t *lease) {
    assert(s->open && !s->leased && id==1);
    in_capture=true;
    while (atomic_load(&block_capture)) usleep(1000);
    in_capture=false;
    s->leased=true;memset(s->jpeg,(uint8_t)++acquired,sizeof(s->jpeg));*lease=2;return ESP_OK;
}
esp_err_t solar_os_script_media_frame(solar_os_script_media_t *s,uint32_t id,solar_os_script_media_frame_t *frame) {
    assert(id==2 && s->leased);*frame=(solar_os_script_media_frame_t){.jpeg={.data=s->jpeg,.length=8,.width=640,.height=480}};return ESP_OK;
}
esp_err_t solar_os_script_media_release(solar_os_script_media_t *s,uint32_t id) {
    assert(id==2 && s->leased);s->leased=false;released++;return ESP_OK;
}
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
esp_err_t solar_os_script_media_rtsp_open(solar_os_script_media_t *s,const char *url,bool video,bool audio,uint32_t *handle) {
    assert(!strcmp(url,"rtsp://example/media") && video && !audio);s->open=s->remote=true;opens++;*handle=1;return ESP_OK;
}
esp_err_t solar_os_script_media_rtsp_read(solar_os_script_media_t *s,uint32_t handle,uint32_t ms,uint32_t *frame)
{ assert(s->remote && ms==50);return solar_os_script_media_acquire(s,handle,frame); }
esp_err_t solar_os_script_media_rtsp_status(solar_os_script_media_t *s,uint32_t id,solar_os_rtsp_client_status_t *status,bool *ended)
{ (void)s;(void)id;*status=(solar_os_rtsp_client_status_t){.video=true};*ended=false;return ESP_OK; }
esp_err_t solar_os_rtsp_url_normalize(const char *in,char *out,size_t size)
{ assert(strlen(in)<size);strcpy(out,in);return ESP_OK; }
esp_err_t solar_os_rtsp_url_redact(const char *in,char *out,size_t size)
{ return solar_os_rtsp_url_normalize(in,out,size); }
#endif
size_t solar_os_stream_count(void) { return 3; }
bool solar_os_stream_get(size_t i,solar_os_stream_info_t *info) {
    if (i>=3) return false;memset(info,0,sizeof(*info));
    strcpy(info->id,i==0?"camera0":i==1?"mic0":"video-sink");
    info->type=i==1?SOLAR_OS_STREAM_TYPE_AUDIO:SOLAR_OS_STREAM_TYPE_VIDEO;
    info->direction=i==2?SOLAR_OS_STREAM_DIRECTION_SINK:SOLAR_OS_STREAM_DIRECTION_SOURCE;
    info->video=(solar_os_stream_video_format_t){.codec=SOLAR_OS_STREAM_VIDEO_JPEG,.width=240,.height=240};return true;
}
esp_err_t solar_os_stream_get_info(const char *id,solar_os_stream_info_t *out) {
    for (size_t i=0;solar_os_stream_get(i,out);i++) if (!strcmp(id,out->id)) return ESP_OK;return ESP_ERR_NOT_FOUND;
}
esp_err_t solar_os_storage_resolve_path(const char *arg,char *out,size_t size) {
    const char *p=!strcmp(arg,".")?directory:arg;if (strlen(p)>=size) return ESP_ERR_INVALID_SIZE;strcpy(out,p);return ESP_OK;
}
void solar_os_storage_notify_changed(void) {}
int solar_os_context_argc(const solar_os_context_t *ctx) { return ctx->argc; }
const char *solar_os_context_argv(const solar_os_context_t *ctx,int i) { return ctx->argv[i]; }
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
void solar_os_context_set_graphics_active(solar_os_context_t *ctx,bool active) { (void)ctx;(void)active; }
void solar_os_context_finish(solar_os_context_t *ctx,int code,const char *message) { (void)ctx;(void)code;(void)message;finishes++; }
solar_os_display_rect_t solar_os_gfx_canvas_bounds(const solar_os_gfx_t *gfx) { (void)gfx;return (solar_os_display_rect_t){0,0,canvas_width,canvas_height}; }
solar_os_display_rect_t solar_os_gfx_panel_bounds(const solar_os_gfx_t *gfx) { (void)gfx;return (solar_os_display_rect_t){-60,-60,412,412}; }
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { (void)gfx;return canvas_width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { (void)gfx;return canvas_height; }
solar_os_display_format_t solar_os_gfx_format(const solar_os_gfx_t *gfx)
{ return gfx->mono?SOLAR_OS_DISPLAY_FORMAT_MONO1:SOLAR_OS_DISPLAY_FORMAT_INDEX8; }
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *gfx,solar_os_display_format_t format)
{ assert(format==SOLAR_OS_DISPLAY_FORMAT_RGB565);return !gfx->mono; }
bool solar_os_gfx_display_target_name(const solar_os_gfx_t *gfx,char *out,size_t size)
{ (void)gfx;assert(size>8);strcpy(out,"display0");return true; }
esp_err_t solar_os_display_set_high_refresh_override(const char *name,bool enabled,uint16_t hz)
{ (void)enabled;(void)hz;assert(!strcmp(name,"display0"));return ESP_OK; }
void solar_os_gfx_clear(solar_os_gfx_t *gfx,solar_os_gfx_color_t c) { (void)gfx;(void)c; }
void solar_os_gfx_set_font(solar_os_gfx_t *gfx,solar_os_gfx_font_t f) { (void)gfx;font=f; }
void solar_os_gfx_set_color(solar_os_gfx_t *gfx,solar_os_gfx_color_t c) { (void)gfx;color=c; }
size_t solar_os_gfx_text_width(solar_os_gfx_t *gfx,const char *s) { (void)gfx;return strlen(s)*5; }
#include "gfx_text_metrics_fixture.h"
void solar_os_gfx_text(solar_os_gfx_t *gfx,int x,int y,const char *text)
{
    (void)gfx;if(strstr(text,"z: capture")) literal_key_hint=true;
    assert(x>=-60 && x<352 && y>=-60 && y<352);
    if (!photo.fullscreen) assert(x>=0 && x+(int)strlen(text)*5<=canvas_width && y<canvas_height);
    if (!strcmp(text,"Photo")) {
        assert(color==SOLAR_OS_GFX_COLOR_WHITE);
        assert(font==(canvas_height<180 || canvas_width<240?SOLAR_OS_GFX_FONT_BITMAP_8:SOLAR_OS_GFX_FONT_BOLD_16));
        headers++;
    }
}
void solar_os_gfx_rect(solar_os_gfx_t *gfx,int x,int y,int w,int h)
{ (void)gfx;assert(x>=0 && y>=0 && w>0 && h>0 && x+w<=canvas_width && y+h<=canvas_height); }
void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx,int x,int y,int w,int h)
{ solar_os_gfx_rect(gfx,x,y,w,h); }
void solar_os_gfx_line(solar_os_gfx_t *gfx,int x,int y,int right,int bottom)
{ (void)gfx;assert(x>=0 && right<canvas_width && y>=0 && bottom<canvas_height); }
void solar_os_gfx_fill_circle(solar_os_gfx_t *gfx,int x,int y,int radius)
{ (void)gfx;assert(x-radius>=0 && x+radius<canvas_width && y-radius>=0 && y+radius<canvas_height);shutters++; }
void solar_os_gfx_fill_polygon(solar_os_gfx_t *gfx,const solar_os_gfx_point_t *points,size_t count)
{ (void)gfx;(void)points;(void)count;assert(false); }
void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx; }
esp_err_t solar_os_gfx_present_frame_at(solar_os_gfx_t *gfx,const solar_os_display_raster_t *r,int x,int y) {
    (void)gfx;assert(r->format==SOLAR_OS_DISPLAY_FORMAT_RGB565 && r->source_stride==4);
    assert(x>=-60 && y>=-60 && x+r->width<=352 && y+r->height<=352);direct_frames++;return ESP_OK;
}
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx,const solar_os_gfx_raster_t *r,int x,int y,int w,int h,const solar_os_gfx_clip_t *clip)
{ (void)gfx;(void)r;(void)clip;assert(x>=-60 && y>=-60 && x+w<=352 && y+h<=352);indexed_frames++;return ESP_OK; }
const char *esp_err_to_name(esp_err_t error) { (void)error;return "test-error"; }

static void tick(solar_os_context_t *ctx) { const solar_os_event_t e={.type=SOLAR_OS_EVENT_TICK};event(ctx,&e);usleep(3000); }
static void key(solar_os_context_t *ctx,uint8_t ch) { const solar_os_event_t e={.type=SOLAR_OS_EVENT_CHAR,.data.ch=ch};event(ctx,&e); }
static bool click(solar_os_context_t *ctx,int x,int y,solar_os_input_pointer_action_t action,uint8_t buttons) {
    const solar_os_event_t e={.type=SOLAR_OS_EVENT_POINTER,.data.pointer={
        .mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE,.action=action,.buttons=buttons,.x=x,.y=y}};
    return event(ctx,&e);
}
static void await_preview(solar_os_context_t *ctx) {
    for (unsigned i=0;i<1500 && !photo.current;i++) tick(ctx);assert(photo.current);
}
static void await_save(solar_os_context_t *ctx) {
    for (unsigned i=0;i<1500 && !__atomic_load_n(&photo.save_done,__ATOMIC_ACQUIRE);i++) tick(ctx);
    assert(photo.save_done);
}
static void close_app(solar_os_context_t *ctx) {
    stop(ctx);for(unsigned i=0;i<2000 && !release_ready();i++) usleep(1000);
    assert(release_ready());cleanup();assert(!allocations);
}
static char candidates[32][64];static unsigned candidate_count;
static void emit(const char *text,void *user) { (void)user;assert(candidate_count<32);strcpy(candidates[candidate_count++],text); }
static bool offered(const char *name) { for(unsigned i=0;i<candidate_count;i++) if(!strcmp(candidates[i],name)) return true;return false; }
static void set_args(solar_os_context_t *ctx,int argc,const char **argv) {
    ctx->argc=argc;for(int i=0;i<argc;i++) strlcpy(ctx->argv[i],argv[i],sizeof(ctx->argv[i]));
}
static void check_capture(solar_os_context_t *ctx,uint8_t shutter) {
    uint8_t expected[8],saved[8];memcpy(expected,photo.current->jpeg,8);
    if (shutter) key(ctx,shutter);
    else assert(click(ctx,canvas_width/2,canvas_height-5,SOLAR_OS_INPUT_POINTER_PRESS,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    await_save(ctx);assert(strstr(photo.message,"Saved photo-000002.jpg"));
    char path[200];snprintf(path,sizeof(path),"%s/photo-000002.jpg",directory);
    FILE *f=fopen(path,"rb");assert(f && fread(saved,1,8,f)==8 && !memcmp(saved,expected,8));
    fclose(f);unlink(path);
}
int main(void) {
    char tmp[]="/tmp/solaros-photo-XXXXXX";assert(mkdtemp(tmp));strcpy(directory,tmp);
    photo_state_t state;photo_state=&state;solar_os_gfx_t gfx={0};
    const char *args[]={"photo"};solar_os_context_t ctx={.gfx=&gfx};set_args(&ctx,1,args);
    candidate_count=0;solar_os_photo_complete(args,1,emit,NULL);
    assert(offered("camera0") && !offered("mic0") && !offered("video-sink") && offered("--key") && offered("--fullscreen"));
    const char *full[]={"photo","--fullscreen"};candidate_count=0;solar_os_photo_complete(full,2,emit,NULL);
    assert(offered("camera0") && offered("--key") && !offered("--fullscreen"));
    const char *keys[]={"photo","camera0","--key"};candidate_count=0;solar_os_photo_complete(keys,3,emit,NULL);
    assert(offered("SPACE") && !offered("camera0"));
    const char *sizes[]={"photo","--size"};candidate_count=0;solar_os_photo_complete(sizes,2,emit,NULL);assert(offered("240x240"));
    char path[200];snprintf(path,sizeof(path),"%s/photo-000001.jpg",directory);
    FILE *f=fopen(path,"wb");assert(f);fputs("existing",f);fclose(f);
    fixture_media_unavailable=true;
    assert(start(&ctx)==ESP_ERR_NOT_SUPPORTED && !photo.mutex && !photo.worker);
    fixture_media_unavailable=false;
    assert(start(&ctx)==ESP_OK);
    const unsigned media_queries=fixture_media_queries, ui_queries=fixture_ui_queries;
    await_preview(&ctx);assert(direct_frames && photo.key==SOLAR_OS_KEY_ENTER);
    assert(fixture_media_queries==media_queries && fixture_ui_queries==ui_queries);
    assert(headers && shutters && (solar_os_photo_app.flags & SOLAR_OS_APP_FLAG_POINTER_EVENTS));
    solar_os_media_player_layout_t ui;
    solar_os_media_player_layout(canvas_width,canvas_height,false,&ui);
    solar_os_display_rect_t view=viewport(&gfx);
    assert(view.x==ui.view_x && view.y==ui.view_y && view.width==ui.view_width && view.height==ui.view_height);
    assert(!click(&ctx,0,0,SOLAR_OS_INPUT_POINTER_PRESS,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    assert(!click(&ctx,canvas_width/2,canvas_height-5,SOLAR_OS_INPUT_POINTER_MOVE,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    assert(!click(&ctx,canvas_width/2,canvas_height-5,SOLAR_OS_INPUT_POINTER_RELEASE,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    assert(!click(&ctx,canvas_width/2,canvas_height-5,SOLAR_OS_INPUT_POINTER_PRESS,0));
    assert(!click(&ctx,ui.button_gap*2+ui.button_width-1,canvas_height-5,SOLAR_OS_INPUT_POINTER_PRESS,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    assert(!photo.capture_busy && photo.sequence==1);
    memset(photo.label,'L',sizeof(photo.label)-1);photo.label[sizeof(photo.label)-1]=0;
    memset(photo.message,'M',sizeof(photo.message)-1);photo.message[sizeof(photo.message)-1]=0;
    photo.dirty=true;render(&ctx); /* Long source names and save errors are clipped. */
    strcpy(photo.label,"camera0");photo.message[0]=0;
    uint8_t expected[8];memcpy(expected,photo.current->jpeg,8);
    block_write=true;assert(click(&ctx,canvas_width/2,canvas_height-5,SOLAR_OS_INPUT_POINTER_PRESS,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    assert(photo.capture_busy);key(&ctx,SOLAR_OS_KEY_ENTER);
    assert(click(&ctx,canvas_width/2,canvas_height-5,SOLAR_OS_INPUT_POINTER_PRESS,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    block_write=false;await_save(&ctx);assert(strstr(photo.message,"Saved photo-000002.jpg"));
    snprintf(path,sizeof(path),"%s/photo-000002.jpg",directory);f=fopen(path,"rb");assert(f);
    uint8_t saved[8];assert(fread(saved,1,8,f)==8 && !memcmp(saved,expected,8));fclose(f);unlink(path);
    fail_write=true;key(&ctx,SOLAR_OS_KEY_ENTER);await_save(&ctx);assert(strstr(photo.message,"Save failed"));fail_write=false;
    snprintf(path,sizeof(path),"%s/photo-000003.jpg",directory);assert(access(path,F_OK)!=0);
    unsigned before=headers;
    key(&ctx,'f');assert(photo.fullscreen);tick(&ctx);assert(headers==before && viewport(&gfx).width==412);
    assert(!click(&ctx,canvas_width/2,canvas_height-5,SOLAR_OS_INPUT_POINTER_PRESS,SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY));
    unsigned previous=opens;suspend(&ctx);
    for(unsigned i=0;i<1000 && closes<previous;i++) usleep(1000);assert(closes==previous);
    resume(&ctx);for(unsigned i=0;i<1000 && opens==previous;i++) tick(&ctx);assert(opens==previous+1);
    before=finishes;key(&ctx,SOLAR_OS_KEY_ESCAPE);assert(finishes==before+1);
    close_app(&ctx);assert(acquired==released && opens==closes);
    gfx.mono=true;const char *custom[]={"photo","--key","SPACE"};set_args(&ctx,3,custom);
    assert(start(&ctx)==ESP_OK);await_preview(&ctx);assert(indexed_frames);
    key(&ctx,SOLAR_OS_KEY_ENTER);assert(!photo.capture_busy);
    photo.save_done=false;key(&ctx,' ');assert(!photo.capture_busy && !photo.capture);photo.save_done=true;
    deny_save=true;key(&ctx,' ');assert(!photo.capture_busy && strstr(photo.message,"Not enough memory"));deny_save=false;
    check_capture(&ctx,' ');
    close_app(&ctx);gfx.mono=false;set_args(&ctx,1,args);
    const char *letter[]={"photo","--key","z"};set_args(&ctx,3,letter);
    assert(start(&ctx)==ESP_OK && literal_key_hint && photo.key=='z');close_app(&ctx);set_args(&ctx,1,args);
    /* The same widgets and hit testing adapt to compact monochrome canvases. */
    const int dimensions[][2]={{240,135},{128,64},{80,48}};
    gfx.mono=true;
    for (unsigned i=0;i<sizeof(dimensions)/sizeof(dimensions[0]);i++) {
        canvas_width=dimensions[i][0];canvas_height=dimensions[i][1];before=shutters;
        assert(start(&ctx)==ESP_OK);await_preview(&ctx);assert(shutters>before);
        solar_os_media_player_layout(canvas_width,canvas_height,false,&ui);view=viewport(&gfx);
        assert(view.y==ui.view_y && view.height==ui.view_height);
        check_capture(&ctx,0);close_app(&ctx);
    }
    canvas_width=canvas_height=291;gfx.mono=false;
    const char *full_options[]={"photo","--dir",directory,"--key","SPACE","--fullscreen","--size","640x480"};
    set_args(&ctx,8,full_options);before=headers;
    assert(start(&ctx)==ESP_OK && photo.fullscreen && photo.output_width==412 && photo.output_height==412);
    await_preview(&ctx);assert(headers==before && viewport(&gfx).x==-60);
    key(&ctx,'f');assert(!photo.fullscreen && headers>before);
    key(&ctx,'f');assert(photo.fullscreen);close_app(&ctx);set_args(&ctx,1,args);
    assert(start(&ctx)==ESP_OK);await_preview(&ctx);block_write=true;
    const solar_os_event_t relative={.type=SOLAR_OS_EVENT_POINTER,.data.pointer={
        .mode=SOLAR_OS_INPUT_POINTER_RELATIVE,.action=SOLAR_OS_INPUT_POINTER_PRESS,
        .buttons=SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY,.delta_x=145,.delta_y=286}};
    assert(event(&ctx,&relative) && photo.capture_busy);block_write=false;await_save(&ctx);
    assert(strstr(photo.message,"Saved photo-000002.jpg"));
    snprintf(path,sizeof(path),"%s/photo-000002.jpg",directory);unlink(path);close_app(&ctx);
    busy_source=true;assert(start(&ctx)==ESP_OK);
    for(unsigned i=0;i<1000 && !photo.done;i++) tick(&ctx);assert(photo.done && photo.error==ESP_ERR_INVALID_STATE);
    close_app(&ctx);busy_source=false;
    const char *bad[]={"photo","--key","ESCAPE"};set_args(&ctx,3,bad);
    assert(start(&ctx)==ESP_ERR_INVALID_ARG);close_app(&ctx);
    const char *duplicate[]={"photo","--fullscreen","--fullscreen"};set_args(&ctx,3,duplicate);
    assert(start(&ctx)==ESP_ERR_INVALID_ARG);close_app(&ctx);
    const char *remote[]={"photo","--fullscreen","rtsp://example/media"};set_args(&ctx,3,remote);
    candidate_count=0;solar_os_photo_complete(remote,3,emit,NULL);
    assert(offered("--dir") && !offered("--fullscreen") && !offered("--size"));
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    assert(start(&ctx)==ESP_OK);await_preview(&ctx);assert(photo.remote && photo.fullscreen);
    check_capture(&ctx,SOLAR_OS_KEY_ENTER);close_app(&ctx);
#else
    assert(start(&ctx)==ESP_ERR_NOT_SUPPORTED);close_app(&ctx);
#endif
    /* A blocked provider outlives stop's wait budget; state is retained until
     * the worker releases its frame and source, rather than force-deleting it. */
    set_args(&ctx,1,args);block_capture=true;assert(start(&ctx)==ESP_OK);
    for(unsigned i=0;i<1000 && !in_capture;i++) usleep(1000);assert(in_capture);
    stop(&ctx);assert(!release_ready());block_capture=false;
    for(unsigned i=0;i<1000 && !release_ready();i++) usleep(1000);
    assert(release_ready());cleanup();assert(!allocations && acquired==released && opens==closes);
    snprintf(path,sizeof(path),"%s/photo-000001.jpg",directory);f=fopen(path,"rb");assert(f);
    assert(fread(saved,1,8,f)==8 && !memcmp(saved,"existing",8));fclose(f);unlink(path);rmdir(directory);
    puts("photo: source routing, exact-frame capture, non-overwrite, failure cleanup, controls and lifecycle passed");
}
