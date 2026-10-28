from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayProfileRotationTest(unittest.TestCase):
    def run_native(self, source):
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory, 'rotation.c'), Path(directory, 'rotation')
            path.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'tests/host'),
                            '-I', str(ROOT / 'src/services'), '-I', str(ROOT / 'src'),
                            '-idirafter', str(ROOT / 'include'), str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_shell_redraw_preserves_typed_command_and_history(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct {bool watch_active,log_follow_active;void *foreground_app;
    char input[80],history_draft[80];size_t input_len,input_cursor,input_row,input_col;
    int history_index;} solar_os_shell_session_t;
typedef struct {size_t row,col;unsigned writes,clears,renders;char screen[80];} solar_os_shell_io_t;
typedef struct {solar_os_shell_session_t *session;solar_os_shell_io_t *io;} solar_os_context_t;
static solar_os_shell_io_t *shell_io(solar_os_context_t *ctx) {return ctx->io;}
static size_t solar_os_shell_io_cursor_row(solar_os_shell_io_t *io) {return io->row;}
static void solar_os_shell_io_clear_line_from(solar_os_shell_io_t *io,size_t row,size_t col) {
    assert(row==io->row && col==0);io->clears++;io->screen[0]=0;
}
static void solar_os_shell_io_set_cursor(solar_os_shell_io_t *io,size_t row,size_t col) {io->row=row;io->col=col;}
static void shell_write_prompt(solar_os_context_t *ctx) {
    assert(ctx->io->col==0);strcpy(ctx->io->screen,"user:/ ");ctx->io->writes++;
    ctx->session->input_row=ctx->io->row;ctx->session->input_col=7;
}
static void shell_render_input(solar_os_context_t *ctx) {
    strcat(ctx->io->screen,ctx->session->input);ctx->io->renders++;
}
'''
        source += production_functions((ROOT / 'src/apps/solar_os_shell.c').read_text(),
                                       ('solar_os_shell_session_redraw_prompt',))
        source += r'''
int main(void) {
    solar_os_shell_session_t session={.input="echo unfinished",.history_draft="draft",
        .input_len=15,.input_cursor=5,.input_row=17,.input_col=13,.history_index=3};
    solar_os_shell_io_t io={.row=4,.col=8};solar_os_context_t ctx={&session,&io};
    solar_os_shell_session_redraw_prompt(&ctx,&session);
    assert(!strcmp(io.screen,"user:/ echo unfinished"));
    assert(session.input_len==15 && session.input_cursor==5 && session.history_index==3);
    assert(session.input_row==4 && session.input_col==7 && !strcmp(session.history_draft,"draft"));
    session.watch_active=true;solar_os_shell_session_redraw_prompt(&ctx,&session);
    session.watch_active=false;session.log_follow_active=true;solar_os_shell_session_redraw_prompt(&ctx,&session);
    session.log_follow_active=false;session.foreground_app=&ctx;solar_os_shell_session_redraw_prompt(&ctx,&session);
    assert(io.writes==1 && io.clears==1 && io.renders==1);return 0;
}
'''
        self.run_native(source)

    def test_profile_notifies_shell_gui_and_tui_without_touching_other_displays(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define SOLAR_OS_SESSION_MAX 5
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 20
typedef struct {unsigned orientation_degrees,scale,font,text_size;bool status_bar_visible;} solar_os_terminal_profile_t;
typedef struct {solar_os_terminal_profile_t profile;} solar_os_terminal_t;
typedef struct {bool used,started,suspended,graphics_active,terminal_redraw_requested,snapshot;
    solar_os_terminal_t *terminal;const char *target;} solar_os_session_entry_t;
static struct {solar_os_session_entry_t sessions[5];solar_os_terminal_t *shell_terminal;} session_state;
static solar_os_terminal_profile_t registered={.scale=2};
static bool session_terminal_target_name(const solar_os_session_entry_t *s,const solar_os_terminal_t *t,char *out,size_t size) {
    (void)t;if(!s)return false;assert(strlen(s->target)<size);strcpy(out,s->target);return true;
}
static esp_err_t solar_os_display_get_terminal_profile(const char *name,solar_os_terminal_profile_t *p) {
    if(strcmp(name,"display0"))return ESP_ERR_NOT_FOUND;
    *p=registered;return ESP_OK;
}
static esp_err_t solar_os_display_set_terminal_profile(const char *name,const solar_os_terminal_profile_t *p) {
    if(strcmp(name,"display0"))return ESP_ERR_NOT_FOUND;
    registered=*p;return ESP_OK;
}
static void solar_os_terminal_get_profile(solar_os_terminal_t *t,solar_os_terminal_profile_t *p) {*p=t->profile;}
static esp_err_t solar_os_terminal_apply_profile_transient(solar_os_terminal_t *t,const solar_os_terminal_profile_t *p) {t->profile=*p;return ESP_OK;}
static void session_discard_graphics_snapshot(solar_os_session_entry_t *s) {s->snapshot=false;}
'''
        source += production_functions((ROOT / 'src/services/solar_os_sessions.c').read_text(),
                                       ('session_terminal_layout_changed', 'session_set_display_terminal_profile_internal'))
        source += r'''
int main(void) {
    solar_os_terminal_t terminals[5];
    for(int i=0;i<5;i++) {
        terminals[i].profile=registered;
        session_state.sessions[i]=(solar_os_session_entry_t){.used=true,.started=true,.terminal=&terminals[i],
            .target=i==4?"other0":"display0",.graphics_active=i==1||i==3,.suspended=i==3,.snapshot=true};
    }
    session_state.shell_terminal=&terminals[0];
    solar_os_terminal_profile_t changed=registered;changed.orientation_degrees=90;
    assert(session_set_display_terminal_profile_internal("display0",&changed)==ESP_OK);
    for(int i=0;i<5;i++) {
        assert(terminals[i].profile.orientation_degrees==(i==4?0U:90U));
        assert(session_state.sessions[i].terminal_redraw_requested==(i<3));
        assert(session_state.sessions[i].snapshot==(i==4));
    }
    changed.scale=3;
    assert(session_set_display_terminal_profile_internal("display0",&changed)==ESP_ERR_INVALID_STATE);
    assert(registered.scale==2);return 0;
}
'''
        self.run_native(source)

    def test_background_profile_updates_run_on_scheduler(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define pdTRUE 1
#define portMAX_DELAY 99
#define pdMS_TO_TICKS(n) (n)
#define SOLAR_OS_SESSION_OPERATION_SET_DISPLAY_PROFILE 3
static int ui,bg,semaphore,queue;
static void *current_task=&bg,*session_scheduler_task=&ui,*session_operation_queue=&queue;
typedef struct {unsigned orientation_degrees;} solar_os_terminal_profile_t;
typedef struct {int type;const char *target_name;solar_os_terminal_profile_t display_profile;
    void *complete;esp_err_t result;} solar_os_session_operation_request_t;
static solar_os_session_operation_request_t *pending;
static solar_os_terminal_profile_t *caller_profile;
static unsigned applied,deleted;
static bool queue_fail;
static void *xTaskGetCurrentTaskHandle(void) {return current_task;}
static bool solar_os_shutdown_requested(void) {return false;}
static void *xSemaphoreCreateBinary(void) {return &semaphore;}
static void vSemaphoreDelete(void *handle) {assert(handle==&semaphore);deleted++;}
static esp_err_t session_set_display_terminal_profile_internal(const char *name,const solar_os_terminal_profile_t *p) {
    assert(current_task==&ui && !strcmp(name,"display0") && p->orientation_degrees==90);
    applied++;return ESP_OK;
}
static int xQueueSend(void *handle,void *item,unsigned timeout) {
    assert(handle==&queue && timeout==1000 && !applied);
    if(queue_fail)return 0;
    pending=*(solar_os_session_operation_request_t **)item;
    caller_profile->orientation_degrees=180;return pdTRUE;
}
static int xSemaphoreTake(void *handle,unsigned timeout) {
    assert(handle==&semaphore && timeout==portMAX_DELAY && pending);
    current_task=&ui;
    assert(pending->type==SOLAR_OS_SESSION_OPERATION_SET_DISPLAY_PROFILE);
    pending->result=session_set_display_terminal_profile_internal(pending->target_name,&pending->display_profile);
    current_task=&bg;return pdTRUE;
}
'''
        source += production_functions((ROOT / 'src/services/solar_os_sessions.c').read_text(),
                                       ('solar_os_sessions_set_display_terminal_profile',))
        source += r'''
int main(void) {
    solar_os_terminal_profile_t profile={90};caller_profile=&profile;
    assert(solar_os_sessions_set_display_terminal_profile("display0",&profile)==ESP_OK);
    assert(applied==1 && deleted==1 && profile.orientation_degrees==180);
    profile.orientation_degrees=90;current_task=&ui;
    assert(solar_os_sessions_set_display_terminal_profile("display0",&profile)==ESP_OK && applied==2);
    current_task=&bg;applied=0;queue_fail=true;
    assert(solar_os_sessions_set_display_terminal_profile("display0",&profile)==ESP_ERR_TIMEOUT);
    assert(!applied && deleted==2);return 0;
}
'''
        self.run_native(source)

    def test_indexed_surface_rotation_reuses_pixels_and_invalidates_dirty_hashes(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_display_surface.h"
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 20
typedef struct {uint16_t width,height;solar_os_display_rotation_t rotation;} u8g2_t;
typedef struct {solar_os_display_surface_t surface;} index_t;
typedef struct {u8g2_t *u8g2;index_t *index8;bool dirty;} solar_os_gfx_t;
typedef struct {uint32_t surface_formats;} solar_os_display_target_t;
static unsigned marks,allocations,releases;
static bool gfx_ready(solar_os_gfx_t *g) {return g && g->u8g2;}
static bool gfx_uses_index8(solar_os_gfx_t *g) {return g->index8!=NULL;}
static uint16_t u8g2_GetDisplayWidth(u8g2_t *u) {return u->width;}
static uint16_t u8g2_GetDisplayHeight(u8g2_t *u) {return u->height;}
static solar_os_display_rotation_t gfx_rotation(u8g2_t *u) {return u->rotation;}
static void gfx_mark_index8_all_dirty(solar_os_gfx_t *g) {(void)g;marks++;}
static void solar_os_gfx_release_surface(solar_os_gfx_t *g) {g->index8=NULL;releases++;}
static esp_err_t solar_os_gfx_enable_index8(solar_os_gfx_t *g) {(void)g;allocations++;return ESP_OK;}
static bool solar_os_display_target_name_for_u8g2(u8g2_t *u,char *out,size_t size) {(void)u;(void)out;(void)size;return false;}
static bool solar_os_display_find_target(const char *name,solar_os_display_target_t *target) {(void)name;(void)target;return false;}
'''
        source += production_functions((ROOT / 'src/services/solar_os_gfx.c').read_text(),
                                       ('solar_os_gfx_prepare_surface',))
        source += r'''
int main(void) {
    uint8_t pixels[24];uint32_t hashes[3];u8g2_t display={6,4,SOLAR_OS_DISPLAY_ROTATION_0};
    index_t index={.surface={.width=6,.height=4,.stride=6,.data=pixels,.data_size=24,
        .presented_hashes=hashes,.presented_hash_count=3}};
    solar_os_gfx_t gfx={.u8g2=&display,.index8=&index};
    const solar_os_display_rotation_t rotations[]={SOLAR_OS_DISPLAY_ROTATION_90,SOLAR_OS_DISPLAY_ROTATION_180,
        SOLAR_OS_DISPLAY_ROTATION_270,SOLAR_OS_DISPLAY_ROTATION_0};
    for(unsigned i=0;i<4;i++) {
        display.width=(i%2)?6:4;display.height=(i%2)?4:6;display.rotation=rotations[i];
        memset(hashes,0xff,sizeof(hashes));gfx.dirty=false;solar_os_gfx_prepare_surface(&gfx);
        assert(index.surface.width==display.width && index.surface.height==display.height);
        assert(index.surface.stride==display.width && index.surface.rotation==display.rotation);
        assert(gfx.index8==&index && index.surface.data==pixels && gfx.dirty);
        assert(!hashes[0]&&!hashes[1]&&!hashes[2]);
    }
    assert(marks==4 && !allocations && !releases);return 0;
}
'''
        self.run_native(source)
