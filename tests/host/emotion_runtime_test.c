#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_emotion_abi.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_NOT_SUPPORTED 5
#define ESP_ERR_TIMEOUT 6
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16
#define SOLAR_OS_DISPLAY_TARGET_OWNER_MAX 32
#define SOLAR_OS_DISPLAY_FORMAT_INDEX2 2
#define SOLAR_OS_DISPLAY_FORMAT_MONO1 0
#define SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT 8
#define SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT 2
#define SOLAR_OS_MEMORY_INTERNAL_CRITICAL 0
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 1
#define SOLAR_OS_TASK_ROLE_BACKGROUND 0
#define SOLAR_OS_TASK_STOP_WAIT_MS 2000
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY 0
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef void *TaskHandle_t;
typedef struct { const char *name; } solar_os_gfx_t;
typedef uint32_t solar_os_gfx_color_t;
typedef struct { uint16_t preferred_stream_fps, presentation_stack_bytes; uint32_t frame_formats; char owner[32]; } solar_os_display_target_t;
typedef struct { bool alive; } solar_os_frame_presenter_t;
typedef struct {
    solar_os_gfx_t *gfx;
    int format;
    uint16_t width, height, stride, preferred_fps;
    const uint16_t *palette_rgb565;
    size_t palette_size;
    bool allow_mono_fallback, literal_direct_palette, literal_mono_palette, clear_background_on_resume;
    unsigned background_index;
} solar_os_frame_presenter_config_t;
typedef struct { esp_err_t last_error; } solar_os_frame_presenter_stats_t;
typedef struct { unsigned count, size, head; bool caps; unsigned char data[8][64]; } queue_t;
typedef queue_t *QueueHandle_t;
#include "emotion_runtime_header.inc"
static unsigned allocations, claims, leases, submitted, audio_opened, audio_closed;
static bool join_ok = true, present_stop_ok = true, acquire_ok = true;
static bool presenter_start_ok = true, output_busy, software_target;
static unsigned canvas_rows;
static uint32_t terminal_foreground, terminal_background = 0xffffff;
static uint32_t expected_canvas_background, expected_canvas_foreground = 0xffffff;
static uint64_t clock_ms = 1000;
static unsigned delay_count;
static solar_os_gfx_t gfx = {"oled0"};
static void (*saved_worker)(void *);
static void *saved_arg;
static void fake_delay(unsigned ms);
#define vTaskDelay fake_delay
static int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
#define strlcpy test_strlcpy
static size_t strlcpy(char *out, const char *in, size_t n)
{ size_t len = strlen(in); if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(out,in,k);out[k]=0; }return len; }
static void *solar_os_memory_alloc(size_t n, int kind, const char *tag)
{ (void)kind;(void)tag;void *p=malloc(n);if(p)allocations++;return p; }
static void *solar_os_memory_calloc(size_t n,size_t size,int kind,const char *tag)
{ void *p=solar_os_memory_alloc(n*size,kind,tag);if(p)memset(p,0,n*size);return p; }
static void solar_os_memory_free(void *p) { if(p){allocations--;free(p);} }
static QueueHandle_t xQueueCreate(unsigned n, unsigned size)
{ assert(n==8 && size<=64); queue_t *q=calloc(1,sizeof(*q));q->size=size;return q; }
static QueueHandle_t solar_os_queue_create(unsigned n, unsigned size)
{ queue_t *q=xQueueCreate(n,size);q->caps=true;return q; }
static int xQueueSend(queue_t *q,const void *item,unsigned wait)
{(void)wait;if(q->count==8)return 0;memcpy(q->data[(q->head+q->count)%8],item,q->size);q->count++;return 1;}
static int xQueueReceive(queue_t *q,void *item,unsigned wait)
{(void)wait;if(!q->count)return 0;memcpy(item,q->data[q->head],q->size);q->head=(q->head+1)%8;q->count--;return 1;}
static void solar_os_queue_delete(queue_t *q) { assert(q->caps);free(q); }
static bool solar_os_gfx_display_target_name(solar_os_gfx_t *g,char *out,size_t n)
{ strlcpy(out,g->name,n);return true; }
static unsigned solar_os_gfx_width(solar_os_gfx_t *g) {(void)g;return 128;}
static unsigned solar_os_gfx_height(solar_os_gfx_t *g) {(void)g;return 64;}
static unsigned solar_os_gfx_format(solar_os_gfx_t *g) {(void)g;return SOLAR_OS_DISPLAY_FORMAT_MONO1;}
static bool solar_os_gfx_palette_inverted(solar_os_gfx_t *g) {(void)g;return true;}
static bool solar_os_display_find_target(const char *name,solar_os_display_target_t *t)
{ if(strcmp(name,"oled0"))return false;*t=(solar_os_display_target_t){.preferred_stream_fps=25,.frame_formats=software_target?0:8};return true; }
static int solar_os_display_get_colors(uint32_t *foreground, uint32_t *background)
{ *foreground=terminal_foreground;*background=terminal_background;return ESP_OK; }
static solar_os_gfx_color_t solar_os_gfx_rgb(unsigned r,unsigned g,unsigned b)
{return (r<<16)|(g<<8)|b;}
static void solar_os_gfx_prepare_surface(solar_os_gfx_t *g) {assert(g);}
static void solar_os_gfx_bitmap_2bpp(solar_os_gfx_t *g,int x,int y,int w,int h,const uint8_t *pixels,size_t bytes,const solar_os_gfx_color_t *palette)
{assert(g&&x==0&&y>=0&&y<64&&w==128&&h==1&&pixels&&bytes==32&&palette);
 assert(palette[0]==expected_canvas_background && palette[3]==expected_canvas_foreground);
 canvas_rows++;}
static void solar_os_gfx_present(solar_os_gfx_t *g) {assert(g);submitted++;}
static int solar_os_display_open_gfx(const char *name,const char *owner,solar_os_gfx_t **out,char *detail,size_t n)
{(void)name;(void)owner;(void)detail;(void)n;if(claims)return ESP_ERR_INVALID_STATE;claims++;*out=&gfx;return 0;}
static int solar_os_display_release(const char *name,const char *owner)
{(void)name;(void)owner;assert(claims);claims--;return 0;}
static int solar_os_display_claim(const char *name,const char *owner,char *detail,size_t n)
{solar_os_gfx_t *g=NULL;return solar_os_display_open_gfx(name,owner,&g,detail,n);}
const solar_os_emotion_api_v1_t *solar_os_emotion_engine_api_v1(void);
static int solar_os_native_library_acquire(const char *name,unsigned abi,size_t n,uint32_t *lease,const void **out)
{assert(!strcmp(name,"emotion")&&abi==1&&n==sizeof(solar_os_emotion_api_v1_t));if(!acquire_ok)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;leases++;*lease=42;*out=solar_os_emotion_engine_api_v1();return 0;}
static int solar_os_native_library_release(uint32_t lease)
{assert(lease==42&&leases);leases--;return 0;}
static int solar_os_frame_presenter_init(solar_os_frame_presenter_t **out,const solar_os_frame_presenter_config_t *c)
{assert(c->width==128&&c->height==64&&c->literal_direct_palette&&c->literal_mono_palette);*out=calloc(1,sizeof(**out));return presenter_start_ok?0:ESP_ERR_NO_MEM;}
static int solar_os_frame_presenter_suspend_checked(solar_os_frame_presenter_t *p)
{return !p||present_stop_ok?0:ESP_ERR_TIMEOUT;}
static int solar_os_frame_presenter_deinit(solar_os_frame_presenter_t *p)
{assert(!p||present_stop_ok);free(p);return 0;}
static int solar_os_frame_presenter_resume(solar_os_frame_presenter_t *p)
{(void)p;return 0;}
static bool solar_os_frame_presenter_redraw(solar_os_frame_presenter_t *p)
{(void)p;return true;}
static bool solar_os_frame_presenter_submit(solar_os_frame_presenter_t *p,const uint8_t *bytes,size_t size)
{assert(p&&bytes&&size==2048);submitted++;return true;}
static void solar_os_frame_presenter_take_stats(solar_os_frame_presenter_t *p,solar_os_frame_presenter_stats_t *stats)
{(void)p;stats->last_error=0;}
static int solar_os_task_create_pinned_internal(void (*fn)(void *),const char *name,unsigned size,void *arg,
    unsigned priority,void **task,int core,int role)
{(void)name;(void)size;(void)priority;(void)core;(void)role;saved_worker=fn;saved_arg=arg;*task=(void *)1;return pdPASS;}
static bool solar_os_task_wait_done(void *task,volatile bool *done,unsigned timeout)
{(void)task;(void)done;(void)timeout;return join_ok;}
static void solar_os_task_delete_internal(void *task) {(void)task;}
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#define SOLAR_OS_STREAM_AUDIO_S16_LE 1
typedef struct {int unused;} solar_os_audio_stream_t;
typedef struct {unsigned sample_format,bits_per_sample,channels,sample_rate;} solar_os_audio_stream_format_t;
static solar_os_audio_stream_t audio;
static bool solar_os_audio_output_available(void) {return true;}
static int solar_os_audio_stream_open(const char *owner,unsigned timeout,solar_os_audio_stream_t **out,solar_os_audio_stream_format_t *f)
{(void)owner;assert(timeout==0);if(output_busy)return ESP_ERR_TIMEOUT;*out=&audio;*f=(solar_os_audio_stream_format_t){1,16,2,16000};audio_opened++;return 0;}
static void solar_os_audio_stream_close(solar_os_audio_stream_t *a) {assert(a==&audio);audio_closed++;}
static int solar_os_audio_stream_write(solar_os_audio_stream_t *a,const void *samples,size_t size)
{assert(a==&audio&&samples&&size<=1024);clock_ms+=size/64;return 0;}
#endif
#include "emotion_runtime.inc"
static void fake_delay(unsigned ms)
{clock_ms+=ms;if(++delay_count>=8)atomic_store(&((solar_os_emotion_t *)saved_arg)->stop,true);}
static void test_terminal_colors(void)
{
    solar_os_emotion_options_t o={.display="oled0",.motion=0};
    solar_os_emotion_t *s=NULL;
    terminal_foreground=0xff0000;terminal_background=0x0000ff;
    assert(!solar_os_emotion_open(&o,NULL,&s,NULL,0));
    assert(s->palette[0]==0x001f && s->palette[3]==0xf800);
    assert(s->palette[1]==0x5015 && s->palette[2]==0xa80a);
    assert(!solar_os_emotion_close(&s));
    terminal_foreground=0xffffff;terminal_background=0;
    assert(!solar_os_emotion_open(&o,NULL,&s,NULL,0));
    assert(s->palette[0]==0 && s->palette[3]==0xffff);
    assert(!solar_os_emotion_close(&s));
    terminal_foreground=0xff0000;terminal_background=0x0000ff;
    /* Either endpoint can be overridden, including an explicit zero. */
    o.foreground_set=true;o.foreground_rgb888=0;
    assert(!solar_os_emotion_open(&o,NULL,&s,NULL,0));
    assert(s->palette[0]==0x001f && s->palette[3]==0);
    assert(!solar_os_emotion_close(&s));
    o.background_set=true;o.background_rgb888=0x00ff00;
    assert(!solar_os_emotion_open(&o,NULL,&s,NULL,0));
    assert(s->palette[0]==0x07e0 && s->palette[3]==0);
    assert(!solar_os_emotion_close(&s));
    o.foreground_set=false;
    assert(!solar_os_emotion_open(&o,NULL,&s,NULL,0));
    assert(s->palette[0]==0x07e0 && s->palette[3]==0xf800);
    assert(!solar_os_emotion_close(&s));
    o.background_rgb888=0x1000000;
    assert(solar_os_emotion_open(&o,NULL,&s,NULL,0)==ESP_ERR_INVALID_ARG && !s);
    o.background_set=false;o.foreground_set=true;o.foreground_rgb888=0x1000000;
    assert(solar_os_emotion_open(&o,NULL,&s,NULL,0)==ESP_ERR_INVALID_ARG && !s);
    o.foreground_set=false;
    /* The software monochrome path compensates canvas polarity once. */
    software_target=true;
    expected_canvas_background=0xffff00;expected_canvas_foreground=0x00ffff;
    assert(!solar_os_emotion_open(&o,NULL,&s,NULL,0));
    delay_count=0;saved_worker(saved_arg);
    assert(canvas_rows==64);
    assert(!solar_os_emotion_close(&s));
    o.foreground_set=true;o.foreground_rgb888=0;
    o.background_set=true;o.background_rgb888=0xffffff;
    expected_canvas_background=0;expected_canvas_foreground=0xffffff;
    assert(!solar_os_emotion_open(&o,NULL,&s,NULL,0));
    delay_count=0;saved_worker(saved_arg);
    assert(canvas_rows==128);
    assert(!solar_os_emotion_close(&s));
    software_target=false;canvas_rows=0;submitted=0;
    terminal_foreground=0;terminal_background=0xffffff;
    expected_canvas_background=0;expected_canvas_foreground=0xffffff;
    assert(!claims&&!leases&&!allocations);
}
int main(void)
{
    test_terminal_colors();
    /* A software OLED target can advertise animation rates without a
     * hardware frame callback. Explicit reduced motion still caps at 2 fps. */
    software_target=true;
    solar_os_emotion_options_t automatic={.display="oled0",.motion=-1};
    solar_os_emotion_t *face=NULL;
    assert(!solar_os_emotion_open(&automatic,NULL,&face,NULL,0));
    assert(face->software && face->motion && face->fps==25);
    assert(!solar_os_emotion_close(&face));
    automatic.motion=0;
    assert(!solar_os_emotion_open(&automatic,NULL,&face,NULL,0));
    assert(face->software && !face->motion && face->fps==2);
    assert(!solar_os_emotion_close(&face));
    software_target=false;
    solar_os_emotion_options_t o={.display="oled0",.sound=true,.motion=0};
    solar_os_emotion_t *s=NULL,*second=NULL;
    char detail[96];
    acquire_ok=false;
    assert(solar_os_emotion_open(&o,NULL,&s,detail,sizeof(detail))==ESP_ERR_NOT_FOUND);
    assert(!s&&!claims&&!leases&&!allocations);
    acquire_ok=true;
    assert(!solar_os_emotion_open(&o,NULL,&s,detail,sizeof(detail)));
    assert(claims==1&&leases==1);
    assert(solar_os_emotion_open(&o,NULL,&second,detail,sizeof(detail))==ESP_ERR_INVALID_STATE&&!second);
    assert(solar_os_emotion_set(s,"invalid",0)==ESP_ERR_INVALID_ARG);
    for(unsigned i=0;i<8;i++)assert(!solar_os_emotion_set(s,"happy",0));
    assert(solar_os_emotion_set(s,"neutral",0)==ESP_ERR_TIMEOUT);
    saved_worker(saved_arg);
    assert(submitted==1); /* still poses do not repeatedly refresh an unchanged display */
    join_ok=false;
    assert(solar_os_emotion_close(&s)==ESP_ERR_TIMEOUT&&s&&claims==1&&leases==1);
    join_ok=true;present_stop_ok=false;
    assert(solar_os_emotion_close(&s)==ESP_ERR_TIMEOUT&&s&&claims==1&&leases==1);
    present_stop_ok=true;
    assert(!solar_os_emotion_close(&s)&&!s&&!claims&&!leases&&!allocations);
    /* Failed admission must retain a failed presenter for caller cleanup. */
    presenter_start_ok=false;present_stop_ok=false;
    assert(solar_os_emotion_open(&o,NULL,&s,detail,sizeof(detail))==ESP_ERR_NO_MEM&&s);
    assert(claims&&leases);
    present_stop_ok=true;
    assert(!solar_os_emotion_close(&s)&&!s&&!claims&&!leases&&!allocations);
    presenter_start_ok=true;
    /* Foreground borrowers also retain the display until their workers stop. */
    o.display=NULL;
    assert(!solar_os_emotion_open(&o,&gfx,&s,detail,sizeof(detail))&&claims==1);
    assert(!solar_os_emotion_suspend(s));
    assert(!claims);
    assert(!solar_os_emotion_set(s,"happy",200)); /* bounded commands survive suspension */
    uint64_t paused=clock_ms;clock_ms+=5000;
    assert(!solar_os_emotion_resume(s)&&s->clock_offset==clock_ms-paused);
    assert(!solar_os_emotion_react(s,"surprised",700,120,"chirp"));
    delay_count=0;saved_worker(saved_arg);
    assert(!solar_os_emotion_close(&s)&&!leases&&!allocations);
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    assert(audio_opened==1&&audio_closed==1);
    output_busy=true;o.display="oled0";
    assert(!solar_os_emotion_open(&o,NULL,&s,detail,sizeof(detail)));
    assert(!solar_os_emotion_react(s,"surprised",700,120,"chirp"));
    delay_count=0;saved_worker(saved_arg);
    solar_os_emotion_status_t stats;solar_os_emotion_status(s,&stats);
    assert(stats.audio_skipped==1&&stats.audio_error==ESP_ERR_TIMEOUT);
    assert(!solar_os_emotion_close(&s)&&!claims&&!leases&&!allocations);
#else
    (void)audio_opened;(void)audio_closed;(void)output_busy;
#endif
    software_target=true;o.display="oled0";
    assert(!solar_os_emotion_open(&o,NULL,&s,detail,sizeof(detail))&&!s->presenter);
    delay_count=0;saved_worker(saved_arg);
    assert(canvas_rows==64);
    assert(!solar_os_emotion_close(&s)&&!claims&&!leases&&!allocations);
    puts("emotion runtime passed");
    return 0;
}
