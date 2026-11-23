#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE };
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef int solar_os_board_display_t;
typedef struct {
    bool active;
    uint32_t generation, terminal_epoch;
    size_t refs;
    SemaphoreHandle_t present_mutex;
    struct {
        bool ready;
        esp_err_t (*set_colors)(void *,uint32_t,uint32_t);
        void *color_context;
    } target;
} display_target_slot_t;
#define SOLAR_OS_DISPLAY_TARGET_MAX 5
#define DISPLAY_NVS_FOREGROUND_KEY "foreground"
#define DISPLAY_NVS_BACKGROUND_KEY "background"
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define portENTER_CRITICAL(lock) assert(!pthread_mutex_lock(lock))
#define portEXIT_CRITICAL(lock) assert(!pthread_mutex_unlock(lock))
static pthread_mutex_t display_targets_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t present[3]={PTHREAD_MUTEX_INITIALIZER,PTHREAD_MUTEX_INITIALIZER,PTHREAD_MUTEX_INITIALIZER};
static display_target_slot_t display_targets[SOLAR_OS_DISPLAY_TARGET_MAX];
static atomic_bool waiting,finished;
static _Thread_local bool owns_present;
static uint32_t display_foreground_rgb888=0x123456,display_background_rgb888=0xffffff;
static unsigned saves;
static esp_err_t save_error;
static const char *saved_key;
static void display_load_colors(void) { }
static esp_err_t display_save_color(const char *key,uint32_t color)
{ assert(color<=0xffffff);saves++;saved_key=key;return save_error; }
static int xSemaphoreTake(SemaphoreHandle_t mutex,unsigned wait)
{
    assert(mutex && wait==portMAX_DELAY && !owns_present);
    atomic_store(&waiting,true);assert(!pthread_mutex_lock(mutex));owns_present=true;return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(owns_present);owns_present=false;assert(!pthread_mutex_unlock(mutex)); }
typedef struct { unsigned index,calls;uint32_t foreground,background;bool fail_next; } device_t;
static device_t devices[2]={{.index=0},{.index=1}};
static esp_err_t set_colors(void *context,uint32_t foreground,uint32_t background)
{
    device_t *d=context;
    assert(owns_present && display_targets[d->index].refs==1);
    d->calls++;
    if(d->fail_next){d->fail_next=false;return ESP_FAIL;}
    d->foreground=foreground;d->background=background;return ESP_OK;
}
#if SOLAR_OS_BOARD_HAS_DISPLAY
static solar_os_board_display_t board;
static uint32_t board_foreground,board_background;
static esp_err_t solar_os_board_display_set_colors(solar_os_board_display_t *handle,uint32_t fg,uint32_t bg)
{ assert(handle==&board);board_foreground=fg;board_background=bg;return ESP_OK; }
#endif
/* FUNCTIONS */
#if SOLAR_OS_BOARD_HAS_DISPLAY
static esp_err_t board_colors(void *context,uint32_t fg,uint32_t bg)
{ assert(owns_present && display_targets[2].refs==1);return solar_os_board_display_set_colors(context,fg,bg); }
#endif
static void *change_theme(void *unused)
{ (void)unused;assert(solar_os_display_set_foreground_color(0xff0000)==ESP_OK);atomic_store(&finished,true);return NULL; }
static void idle(void)
{ assert(!owns_present);for(unsigned i=0;i<SOLAR_OS_DISPLAY_TARGET_MAX;i++)assert(!display_targets[i].refs); }
int main(void)
{
    for(unsigned i=0;i<2;i++)display_targets[i]=(display_target_slot_t){
        .active=true,.generation=i+1,.present_mutex=&present[i],
        .target={.ready=true,.set_colors=set_colors,.color_context=&devices[i]}};
    display_targets[2].active=true;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    display_targets[2].target.ready=true;
    display_targets[2].target.set_colors=board_colors;
    display_targets[2].target.color_context=&board;
    display_targets[2].present_mutex=&present[2];
#endif
    display_targets[3].active=true; /* Monochrome target without a callback. */
    display_targets[4].target=display_targets[0].target; /* Detached target. */
    assert(!pthread_mutex_lock(&present[0]));
    pthread_t worker;assert(!pthread_create(&worker,NULL,change_theme,NULL));
    while(!atomic_load(&waiting) && !atomic_load(&finished))sched_yield();
    assert(atomic_load(&waiting));
    assert(!devices[0].calls && !devices[1].calls);
    assert(!pthread_mutex_unlock(&present[0]));assert(!pthread_join(worker,NULL));
    idle();assert(saves==1 && !strcmp(saved_key,"foreground"));
    for(unsigned i=0;i<2;i++)assert(devices[i].foreground==0xff0000 && devices[i].background==0xffffff);
    assert(display_targets[0].terminal_epoch==1 && display_targets[2].terminal_epoch==1);
    assert(display_targets[3].terminal_epoch==1 && !display_targets[4].terminal_epoch);
    assert(solar_os_display_set_background_color(0x0000ff)==ESP_OK);idle();
    for(unsigned i=0;i<2;i++)assert(devices[i].foreground==0xff0000 && devices[i].background==0x0000ff);
    assert(saves==2 && !strcmp(saved_key,"background") && display_targets[0].terminal_epoch==2);
    unsigned calls=devices[0].calls;
    assert(solar_os_display_set_foreground_color(0xff0000)==ESP_OK && saves==3);
    assert(devices[0].calls==calls && display_targets[0].terminal_epoch==2);
    assert(solar_os_display_set_foreground_color(0x1000000)==ESP_ERR_INVALID_ARG);
    assert(solar_os_display_set_background_color(UINT32_MAX)==ESP_ERR_INVALID_ARG && saves==3);
    devices[1].fail_next=true;
    assert(solar_os_display_set_foreground_color(0x00ff00)==ESP_FAIL);idle();
    assert(saves==3 && display_foreground_rgb888==0xff0000 && display_background_rgb888==0x0000ff);
    for(unsigned i=0;i<2;i++)assert(devices[i].foreground==0xff0000 && devices[i].background==0x0000ff);
    assert(display_targets[0].terminal_epoch==3);
    display_targets[0].present_mutex=NULL;
    assert(solar_os_display_set_foreground_color(0x00ff00)==ESP_ERR_INVALID_STATE);idle();
    assert(saves==3 && display_foreground_rgb888==0xff0000);
    display_targets[0].present_mutex=&present[0];
    display_targets[1].target.ready=false;calls=devices[1].calls;
    assert(solar_os_display_set_foreground_color(0x224466)==ESP_OK);idle();
    assert(devices[1].calls==calls && devices[0].foreground==0x224466);
#if SOLAR_OS_BOARD_HAS_DISPLAY
    assert(board_foreground==0x224466 && board_background==0x0000ff);
#endif
    display_targets[1].target.ready=true;save_error=ESP_FAIL;
    assert(solar_os_display_set_background_color(0x112233)==ESP_FAIL);idle();
    assert(display_background_rgb888==0x112233 && devices[1].background==0x112233);
    puts("Display colors: expansion and primary themes, presentation locking, redraw, rollback and persistence passed");
}
