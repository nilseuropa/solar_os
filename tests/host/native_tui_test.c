#include <pthread.h>
#include "tui_fixture.h"
#include "solar_os_native_tui.h"
#include "solar_os_native_tui_abi.h"
#include "solar_os_key_codes.h"
#include "solar_os_native_handles.h"

static const solar_os_native_tui_api_v1_t *api;
static const solar_os_app_t app,other;
static solar_os_native_handle_t active;
static bool start_failure;
static esp_err_t start(solar_os_context_t *ctx)
{
    assert(ctx && solar_os_application_descriptor()==&app);
    const int result=api->screen_begin(&active);
    return result ? ESP_FAIL : start_failure ? ESP_FAIL : ESP_OK;
}
static const solar_os_app_t app={.name="ui-test",.app_class=SOLAR_OS_APP_CLASS_TUI,.start=start};
static const solar_os_app_t other={.name="other",.app_class=SOLAR_OS_APP_CLASS_TUI};
static unsigned rows_called;
static void form_row(size_t index,char *text,size_t capacity,void *user)
{
    assert(user==&rows_called && api->end(active)==SOLAR_OS_NATIVE_ERROR_BUSY);
    ++rows_called;snprintf(text,capacity,"Row %zu",index+1);
}
static void *foreign(void *unused)
{
    (void)unused;assert(!api->rows(active));
    assert(api->clear(active)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    return NULL;
}
int main(void)
{
    api=solar_os_native_tui_get_service("tui",1,8);assert(api && api->struct_size==sizeof(*api));
    assert(!solar_os_native_tui_get_service("tui",2,8));
    assert(!solar_os_native_tui_get_service("tui",1,sizeof(*api)+1));
    assert(!solar_os_native_tui_get_service("missing",1,8));
    active=999;assert(api->begin(&active)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !active);
    solar_os_tui_screen_layout_t layout;
    assert(api->layout_compute(20,40,1,1,1,&layout) && layout.body.width==40);
    solar_os_tui_viewport_t viewport={.cursor=999,.top=999};
    api->viewport_reconcile(&viewport,3,2);assert(viewport.cursor==2 && viewport.top==1);
    char input[20]="ab";solar_os_tui_input_state_t input_state={1,0};
    assert(api->input_text(input,sizeof(input),&input_state,"\xc3\xb6",2,8)==SOLAR_OS_TUI_INPUT_CHANGED);
    assert(!strcmp(input,"a\xc3\xb6" "b") && input_state.cursor==3);
    assert(api->input_key(input,sizeof(input),&input_state,SOLAR_OS_KEY_LEFT,8)==SOLAR_OS_TUI_INPUT_NONE);
    assert(input_state.cursor==1);
    assert(api->input_key(input,sizeof(input),&input_state,SOLAR_OS_KEY_DELETE,8)==SOLAR_OS_TUI_INPUT_CHANGED);
    assert(!strcmp(input,"ab") && !live_allocations);
    const solar_os_port_handle_t port={.index=0,.token=1};
    solar_os_shell_io_t io;solar_os_shell_io_init_port(&io,&port,20,10);
    solar_os_shell_io_set_terminal_profile(&io,SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI);
    solar_os_context_t ctx={.shell_io=&io};
    /* Object allocation plus each core diff allocation can fail independently. */
    for(unsigned stage=0;stage<5;++stage) {
        fail_at=allocations+stage;
        assert(solar_os_app_start(&app,&ctx)==ESP_FAIL && !live_allocations && !solar_os_native_handle_count() && !attached_tui);
    }
    fail_at=UINT_MAX;
    assert(!solar_os_app_start(&app,&ctx) && active && attached_tui && live_allocations==5);
    solar_os_application_scope_t scope;
    solar_os_application_scope_enter(&scope,&app,&ctx);
    assert(api->rows(active)==10 && api->cols(active)==20 && api->screen_fullscreen(active));
    pthread_t task;assert(!pthread_create(&task,NULL,foreign,NULL) && !pthread_join(task,NULL));
    solar_os_application_scope_t child;solar_os_application_scope_enter(&child,&other,&ctx);
    assert(api->clear(active)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !api->rows(active));
    assert(api->end(active)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_application_scope_leave(&child);
    assert(!api->clear(active) && !api->draw_title(active,"Example","ABI"));
    solar_os_tui_form_t form={.bounds={.row=2,.col=0,.height=4,.width=20},.viewport=&viewport,
        .count=3,.row=form_row,.user=&rows_called};
    unsigned before=allocations;
    assert(!api->draw_form(active,&form) && rows_called);
    assert(!api->refresh(active) && strstr(wire,"Example") && allocations==before);
    assert(!api->write_cell(active,5,0,20,"\xc3\xa9",SOLAR_OS_TUI_ATTR_BOLD));
    assert(!api->refresh(active) && strstr(wire,"\xc3\xa9") && allocations==before);
    const solar_os_native_handle_t old=active;
    solar_os_application_scope_leave(&scope);
    solar_os_app_suspend(&app,&ctx);solar_os_app_resume(&app,&ctx);
    assert(live_allocations==5 && solar_os_native_handle_count()==1);
    assert(solar_os_app_stop_checked(&app,&ctx) && !live_allocations && !attached_tui && !solar_os_native_handle_count());
    start_failure=true;
    assert(solar_os_app_start(&app,&ctx)==ESP_FAIL && !live_allocations && !solar_os_native_handle_count());start_failure=false;
    assert(!solar_os_app_start(&app,&ctx) && active!=old);
    solar_os_application_scope_enter(&scope,&app,&ctx);
    assert(!api->rows(old) && api->end(old)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->end(active) && !live_allocations);solar_os_application_scope_leave(&scope);
    assert(solar_os_app_stop_checked(&app,&ctx));
    /* A real local terminal commits a frame only at refresh and restores its
     * status bar when the owning app stops without calling end. */
    uint8_t buffer[52*52*8];u8g2_t u={0};
    u8g2_SetupDisplay(&u,display,u8x8_dummy_cb,u8x8_dummy_cb,u8x8_dummy_cb);
    u8g2_SetupBuffer(&u,buffer,52,u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
    solar_os_display_geometry_t geometry;solar_os_display_geometry_init(&geometry,412,412,false);
    const solar_os_display_rect_t compact={0,0,412,100};assert(solar_os_display_geometry_set_canvas(&geometry,&compact));
    u8g2_SetUserPtr(&u,&geometry);
    solar_os_terminal_t *terminal=calloc(1,sizeof(*terminal));assert(terminal);solar_os_terminal_init(terminal,&u);
    const unsigned baseline=live_allocations;
    solar_os_shell_io_init_terminal(&io,terminal);ctx.terminal=terminal;
    assert(!solar_os_app_start(&app,&ctx) && !solar_os_terminal_status_bar_visible(terminal));
    solar_os_application_scope_enter(&scope,&app,&ctx);before=allocations;
    assert(!api->addstr(active,0,0,"Local ABI",SOLAR_OS_TUI_ATTR_NORMAL) && !terminal->lines[0][0]);
    assert(!api->refresh(active) && terminal->lines[0][0]=='L' && frames && allocations==before);
    solar_os_application_scope_leave(&scope);
    assert(solar_os_app_stop_checked(&app,&ctx) && solar_os_terminal_status_bar_visible(terminal));
    assert(live_allocations==baseline && !attached_tui && !solar_os_native_handle_count());
    solar_os_terminal_deinit(terminal);free(terminal);assert(!live_allocations);
    return 0;
}
