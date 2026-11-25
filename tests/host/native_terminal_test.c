#include "terminal_fixture.h"

int main(void)
{
    const solar_os_native_terminal_api_v1_t *api=solar_os_native_terminal_get_service("terminal",1,8);
    assert(api && api->struct_size==sizeof(*api));
    assert(!solar_os_native_terminal_get_service("terminal",2,8));
    assert(!solar_os_native_terminal_get_service("terminal",1,sizeof(*api)+1));
    assert(!solar_os_native_terminal_get_service("missing",1,8));
    assert(api->write_utf8==solar_os_terminal_write_utf8 && api->draw==solar_os_terminal_draw);
    uint8_t buffer[52*52*8]; u8g2_t u={0};
    u8g2_SetupDisplay(&u,display,u8x8_dummy_cb,u8x8_dummy_cb,u8x8_dummy_cb);
    u8g2_SetupBuffer(&u,buffer,52,u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
    solar_os_display_geometry_t geometry;solar_os_display_geometry_init(&geometry,412,412,false);
    u8g2_SetUserPtr(&u,&geometry);
    solar_os_terminal_t *terminal=calloc(1,sizeof(*terminal));assert(terminal);
    solar_os_terminal_init(terminal,&u);
    assert(api->cols(terminal)>20 && api->rows(terminal)>10);
    const unsigned before=allocations;
    api->clear(terminal);api->set_status_bar_visible_transient(terminal,false);
    api->set_cursor_visible(terminal,false);
    api->write_utf8(terminal,"A\xc3\xa9");
    assert(terminal->lines[0][0]=='A' && terminal->lines[0][1]==0xe9);
    api->printf(terminal," %d %.1f %s",42,1.5,"ABI");
    assert(terminal->lines[0][3]=='4' && terminal->lines[0][4]=='2');
    api->set_cursor(terminal,1,2);api->printf_bold(terminal,"%s %d","B",7);
    assert(terminal->lines[1][2]=='B' && (terminal->bold[1][0]&(1U<<2)));
    assert(!api->bold(terminal));
    api->set_inverse(terminal,true);api->put_codepoint(terminal,'Z');
    assert(api->inverse(terminal));api->set_inverse(terminal,false);
    api->draw(terminal);assert(frames && !api->needs_draw(terminal));
    assert(memcmp(buffer,(uint8_t[sizeof(buffer)]){0},sizeof(buffer)));
    assert(allocations==before); /* Formatting/rendering adds no ABI allocations. */
    api->clear_line_from(terminal,0,0);assert(!terminal->lines[0][0]);
    for(unsigned i=0;i<api->rows(terminal)+3;++i) api->writeln(terminal,"scroll");
    api->page_up(terminal);assert(api->is_scrolled_back(terminal));
    api->scroll_to_live(terminal);assert(!api->is_scrolled_back(terminal));
    solar_os_terminal_profile_t profile;api->get_profile(terminal,&profile);
    profile.orientation_degrees=45;
    assert(api->apply_profile_transient(terminal,&profile)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    profile.orientation_degrees=90;
    assert(!api->apply_profile_transient(terminal,&profile) && api->orientation(terminal)==90 && !commits);
    assert(!api->set_font(terminal,SOLAR_OS_TERMINAL_FONT_MONO) && commits==1);
    assert(api->set_text_size(terminal,SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->set_cursor_preference(SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE,false));
    solar_os_terminal_cursor_shape_t shape;bool blink=true;
    api->get_cursor_preference(&shape,&blink);assert(shape==SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE && !blink);
    fail_commit=true;
    assert(api->set_cursor_preference(SOLAR_OS_TERMINAL_CURSOR_BLOCK,true)==SOLAR_OS_NATIVE_ERROR_FAILED);
    api->get_cursor_preference(&shape,&blink);assert(shape==SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE && !blink);
    solar_os_terminal_deinit(terminal);free(terminal);
    return 0;
}
