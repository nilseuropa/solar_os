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
    assert(!solar_os_tui_box(&tui, 2, 1, 3, 4, SOLAR_OS_TUI_ATTR_NORMAL));
    solar_os_tui_refresh(&tui);
    assert(strstr(wire,"test"));
    assert(strstr(wire, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90"));
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
    assert(!solar_os_tui_box(&tui, 1, 1, 3, 8, SOLAR_OS_TUI_ATTR_NORMAL));
    assert(!solar_os_tui_addstr(&tui, 2, 2, "\xc3\xa5.txt", SOLAR_OS_TUI_ATTR_NORMAL));
    const size_t last_row=solar_os_tui_rows(&tui)-1,last_col=solar_os_tui_cols(&tui)-1;
    assert(!solar_os_tui_putch(&tui,last_row,last_col,'Z',SOLAR_OS_TUI_ATTR_INVERSE));
    assert(!terminal->lines[0][0] && !frames);
    solar_os_tui_refresh(&tui);
    assert(terminal->lines[0][0]=='L' && frames && allocations==before+4 && live_allocations==live_before+4);
    assert(terminal->lines[last_row][last_col]=='Z');
    assert(terminal->lines[1][1] == 0x250c && terminal->lines[1][8] == 0x2510);
    assert(terminal->lines[3][1] == 0x2514 && terminal->lines[3][8] == 0x2518);
    assert(terminal->lines[1][2] == 0x2500 && terminal->lines[2][1] == 0x2502);
    assert(terminal->lines[2][2] == 0x00e5 && terminal->lines[2][3] == '.');

    solar_os_tui_rect_t popup;
    assert(!solar_os_tui_text_popup(&tui, NULL, "Copy", "Overwrite?", &popup));
    solar_os_tui_refresh(&tui);
    assert(terminal->lines[popup.row][popup.col] == 0x250c);
    assert(terminal->lines[popup.row][popup.col + 1] == 0x2500);
    assert(terminal->lines[popup.row + 1][popup.col] == 0x2502);
    solar_os_tui_end(&tui);assert(live_allocations==live_before && !attached_tui);
    solar_os_terminal_deinit(terminal);free(terminal);assert(!live_allocations);
    assert(attaches==2 && detaches==2);
    return 0;
}
