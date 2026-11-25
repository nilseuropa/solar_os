#include "tui_fixture.h"

int main(void)
{
    const solar_os_port_handle_t port={.index=0,.token=1};
    solar_os_shell_io_t io;solar_os_shell_io_init_port(&io,&port,12,8);
    solar_os_context_t ctx={.shell_io=&io};solar_os_tui_t tui;
    solar_os_shell_io_set_terminal_profile(&io,SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB);
    io.bold=true;io.cursor_visible=false;
    assert(solar_os_tui_screen_begin(&tui,&ctx)==ESP_ERR_NOT_SUPPORTED);
    assert(io.bold && !io.cursor_visible && !live_allocations && !attaches);
    solar_os_shell_io_set_terminal_profile(&io,SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI);
    for(unsigned stage=0;stage<4;++stage) {
        fail_at=allocations+stage;
        assert(solar_os_tui_screen_begin(&tui,&ctx)==ESP_ERR_NO_MEM);
        assert(!live_allocations && !attached_tui && io.bold && !io.cursor_visible);
    }
    fail_at=UINT_MAX;
    assert(!solar_os_tui_screen_begin(&tui,&ctx) && attached_tui==&tui && live_allocations==4);
    assert(!solar_os_tui_addstr(&tui,0,0,"test",SOLAR_OS_TUI_ATTR_BOLD));
    solar_os_tui_refresh(&tui);
    assert(strstr(wire,"test"));
    solar_os_tui_end(&tui);assert(!attached_tui && !live_allocations && io.bold && !io.cursor_visible);

    uint8_t buffer[52*52*8];u8g2_t u={0};
    u8g2_SetupDisplay(&u,display,u8x8_dummy_cb,u8x8_dummy_cb,u8x8_dummy_cb);
    u8g2_SetupBuffer(&u,buffer,52,u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
    solar_os_display_geometry_t geometry;solar_os_display_geometry_init(&geometry,412,412,false);u8g2_SetUserPtr(&u,&geometry);
    const solar_os_display_rect_t compact={0,0,412,100};
    assert(solar_os_display_geometry_set_canvas(&geometry,&compact));
    solar_os_terminal_t *terminal=calloc(1,sizeof(*terminal));assert(terminal);solar_os_terminal_init(terminal,&u);
    solar_os_shell_io_init_terminal(&io,terminal);ctx.terminal=terminal;
    const unsigned baseline=live_allocations;
    for(unsigned stage=0;stage<4;++stage) {
        fail_at=allocations+stage;
        assert(solar_os_tui_screen_begin(&tui,&ctx)==ESP_ERR_NO_MEM);
        assert(live_allocations==baseline && solar_os_terminal_status_bar_visible(terminal) && !attached_tui);
    }
    fail_at=UINT_MAX;frames=0;solar_os_terminal_clear(terminal);
    const unsigned before=allocations,live_before=live_allocations;
    /* Local frames stay private until refresh, just like remote frames. */
    assert(!solar_os_tui_screen_begin(&tui,&ctx));
    assert(allocations==before+4 && live_allocations==live_before+4);
    assert(!solar_os_terminal_status_bar_visible(terminal));
    fail_at=allocations;
    assert(!solar_os_tui_addstr(&tui,0,0,"Local",SOLAR_OS_TUI_ATTR_NORMAL));
    const size_t last_row=solar_os_tui_rows(&tui)-1,last_col=solar_os_tui_cols(&tui)-1;
    assert(!solar_os_tui_putch(&tui,last_row,last_col,'Z',SOLAR_OS_TUI_ATTR_INVERSE));
    assert(!terminal->lines[0][0] && !frames);
    solar_os_tui_refresh(&tui);
    assert(terminal->lines[0][0]=='L' && frames && allocations==before+4 && live_allocations==live_before+4);
    assert(terminal->lines[last_row][last_col]=='Z');
    solar_os_tui_end(&tui);assert(live_allocations==live_before && !attached_tui);
    solar_os_terminal_deinit(terminal);free(terminal);assert(!live_allocations);
    assert(attaches==2 && detaches==2);
    return 0;
}
