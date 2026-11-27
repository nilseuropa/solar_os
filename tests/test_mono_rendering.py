from pathlib import Path
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class MonoRenderingTest(unittest.TestCase):
    def test_working_format_preserves_transport_capabilities(self):
        functions = production_functions(
            (ROOT / "src/services/solar_os_display.c").read_text(),
            ("display_prefers_mono", "solar_os_display_open_gfx"))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK,ESP_ERR_INVALID_ARG,ESP_ERR_NOT_FOUND};
#define SOLAR_OS_BOARD_HAS_DISPLAY 1
#define SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT 2U
typedef int solar_os_gfx_t;
typedef struct board board_t;
struct board {struct {bool (*prefers_mono)(const board_t *);} *ops;};
typedef board_t solar_os_board_display_t;
typedef struct {
    uint32_t surface_formats,frame_formats;
    bool prefer_mono;
    bool (*prefers_mono)(const void *);
    void *render_context;
} solar_os_display_target_t;
typedef struct {
    solar_os_display_target_t target;
    solar_os_gfx_t gfx;
    board_t *board_display;
} display_target_slot_t;
static display_target_slot_t display_targets[1];
static int display_targets_lock;
static unsigned allocations,claims;
static bool mono;
static bool display_prefers_mono(const display_target_slot_t *);
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int display_find_slot_locked(const char *name) {return strcmp(name,"screen") ? -1 : 0;}
static esp_err_t solar_os_display_claim(const char *name,const char *owner,char *busy,size_t size)
{(void)name;(void)owner;(void)busy;(void)size;claims++;return ESP_OK;}
static esp_err_t solar_os_display_release(const char *name,const char *owner)
{(void)name;(void)owner;claims--;return ESP_OK;}
static bool solar_os_display_find_target(const char *name,solar_os_display_target_t *target)
{
    assert(!strcmp(name,"screen"));*target=display_targets[0].target;
    target->prefer_mono=display_prefers_mono(&display_targets[0]);return true;
}
static esp_err_t solar_os_gfx_enable_index8(solar_os_gfx_t *gfx)
{assert(gfx==&display_targets[0].gfx);allocations++;return ESP_OK;}
static bool preference(const void *context) {assert(context==&mono);return mono;}
static bool board_preference(const board_t *board) {assert(board);return mono;}
''' + functions + r'''
int main(void)
{
    display_target_slot_t *slot=&display_targets[0];
    slot->target.surface_formats=SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT;
    slot->target.frame_formats=15U; /* Capabilities remain intact. */
    solar_os_gfx_t *gfx=NULL;
    assert(!solar_os_display_open_gfx("screen","app",&gfx,NULL,0) && allocations==1);
    slot->target.prefers_mono=preference;slot->target.render_context=&mono;mono=true;
    assert(!solar_os_display_open_gfx("screen","app",&gfx,NULL,0) && allocations==1);
    assert(gfx==&slot->gfx && slot->target.frame_formats==15U && slot->target.surface_formats==2U);
    mono=false;
    assert(!solar_os_display_open_gfx("screen","app",&gfx,NULL,0) && allocations==2);
    slot->target.prefers_mono=NULL;slot->target.prefer_mono=true;
    assert(!solar_os_display_open_gfx("screen","app",&gfx,NULL,0) && allocations==2);
    board_t board={0};__typeof__(*board.ops) ops={.prefers_mono=board_preference};board.ops=&ops;
    slot->board_display=&board;mono=false;
    assert(!solar_os_display_open_gfx("screen","app",&gfx,NULL,0) && allocations==3);
    mono=true;
    assert(!solar_os_display_open_gfx("screen","app",&gfx,NULL,0) && allocations==3);
    assert(solar_os_display_open_gfx("screen","app",NULL,NULL,0)==ESP_ERR_INVALID_ARG);
    return 0;
}
''')

    def test_native_monochrome_video_uses_packed_frame_sizes(self):
        function = production_functions(
            (ROOT / "src/services/solar_os_native_foreground.c").read_text(),
            ("foreground_video_open_mono1",))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "solar_os_native_app_abi.h"
#include "solar_os_frame_presenter.h"
typedef struct {solar_os_gfx_t *gfx;solar_os_frame_presenter_t *presenter;size_t frame_size;} foreground_state_t;
static foreground_state_t state;
static bool caller=true;
static esp_err_t init_result;
static solar_os_frame_presenter_options_v1_t captured;
static foreground_state_t *foreground_current(void) {return &state;}
static bool foreground_caller(void) {return caller;}
static int foreground_result(esp_err_t result) {return result;}
esp_err_t solar_os_frame_presenter_init(solar_os_frame_presenter_t **out,solar_os_gfx_t *target,const solar_os_frame_presenter_options_v1_t *config)
{assert(target==state.gfx);captured=*config;if(init_result==ESP_OK)*out=(void *)&state;return init_result;}
esp_err_t solar_os_frame_presenter_resume(solar_os_frame_presenter_t *p) {assert(p);return ESP_OK;}
''' + function + r'''
int main(void)
{
    caller=false;assert(foreground_video_open_mono1(624,468,25)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    caller=true;
    assert(foreground_video_open_mono1(0,468,25)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(foreground_video_open_mono1(624,468,61)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!foreground_video_open_mono1(624,468,25));
    assert(state.frame_size==36504 && captured.stride==78 && captured.literal_direct_palette);
    assert(captured.format==SOLAR_OS_DISPLAY_FORMAT_MONO1 && !captured.palette_rgb565);
    assert(foreground_video_open_mono1(624,468,25)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    state.presenter=NULL;
    assert(!foreground_video_open_mono1(623,467,25) && state.frame_size==36426);
    state.presenter=NULL;init_result=ESP_ERR_NOT_SUPPORTED;
    assert(foreground_video_open_mono1(624,468,25)==ESP_ERR_NOT_SUPPORTED && !state.presenter);
    return 0;
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src"),
              "-idirafter", str(ROOT / "include"))
