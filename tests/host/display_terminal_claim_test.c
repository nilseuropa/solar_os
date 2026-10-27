#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16
#define SOLAR_OS_DISPLAY_PRESENT_TEXT 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef struct { int id; } u8g2_t;
typedef unsigned u8g2_uint_t;
typedef uint16_t solar_os_terminal_cell_t;
typedef struct {
    bool active,terminal_blocked;
    uint32_t terminal_epoch;
    unsigned claim_refs;
    struct { char owner[32]; u8g2_t *u8g2; } target;
    int gfx;
} display_target_slot_t;
static display_target_slot_t display_targets[2];
static int display_targets_lock;
static bool display_target_name_valid(const char *name,size_t max) {return name&&*name&&strlen(name)<max;}
static bool display_owner_valid(const char *owner) {return owner&&*owner&&strlen(owner)<32;}
static int display_find_slot_locked(const char *name) {
    if(!strcmp(name,"display0"))return 0;
    if(!strcmp(name,"display1"))return 1;
    return -1;
}
static int display_find_slot_by_u8g2_locked(const u8g2_t *u8g2) {
    for(int i=0;i<2;++i)if(display_targets[i].target.u8g2==u8g2)return i;
    return -1;
}
static void *solar_os_gfx_detach_surface_storage(int *gfx) {(void)gfx;return NULL;}
static void display_init_slot_gfx(display_target_slot_t *slot) {(void)slot;}
static void solar_os_memory_free(void *data) {assert(!data);}
#include "display_terminal_policy.inc"
typedef struct {
    u8g2_t *u8g2;
    bool dirty,render_valid,status_bar_visible,footer_enabled,cursor_visible;
    uint32_t rendered_display_epoch,rendered_profile_hash,rendered_status_hash,rendered_footer_hash;
    uint32_t rendered_row_hash[1],inverse[1][1];
    unsigned baseline_offset,line_height,char_width,cursor_row,cursor_col;
} solar_os_terminal_t;
static unsigned clears,status_paints,presents,fake_status_hash=1;
static void ignore(const void *first,...) {(void)first;}
#define terminal_apply_settings ignore
#define u8g2_SetFontMode ignore
#define u8g2_SetFontPosBaseline ignore
#define terminal_set_draw_color ignore
#define u8g2_DrawBox ignore
#define u8g2_DrawHLine ignore
#define terminal_clear_text_row ignore
#define terminal_draw_line ignore
#define terminal_draw_vrules ignore
#define terminal_draw_footer ignore
#define terminal_text_scale(t) ((void)(t),1)
#define terminal_render_profile_hash(t,u) ((void)(t),(void)(u),1U)
#define terminal_render_status_hash(t) ((void)(t),fake_status_hash)
#define terminal_render_footer_hash(t) ((void)(t),1U)
#define terminal_render_row_hash(t,r) ((void)(t),(void)(r),1U)
#define terminal_rows(t) ((void)(t),0U)
#define terminal_cols(t) ((void)(t),0U)
#define terminal_display_line(t,r,b,i,u,v) (ignore(t,r,b,i,u,v),(const uint16_t *)NULL)
#define solar_os_terminal_is_scrolled_back(t) ((void)(t),false)
#define cursor_x_position(t) ((void)(t),0)
#define terminal_cell_top_y(t,r) ((void)(t),(void)(r),0)
#define terminal_inverse_get(t,c) ((void)(t),(void)(c),false)
#define u8g2_GetDisplayWidth(u) ((void)(u),400)
#define u8g2_GetDisplayHeight(u) ((void)(u),300)
static void u8g2_ClearBuffer(u8g2_t *u) {(void)u;++clears;}
static void terminal_draw_status_bar(solar_os_terminal_t *t,u8g2_t *u) {(void)t;(void)u;++status_paints;}
static void solar_os_display_present(u8g2_t *u,int mode) {(void)u;(void)mode;++presents;}
static int solar_os_display_request_present_mode(u8g2_t *u,int mode) {(void)u;(void)mode;return 0;}
#include "terminal_draw_policy.inc"
int main(void) {
    u8g2_t screens[3]={{0},{1},{2}};
    for(unsigned i=0;i<2;++i)display_targets[i].target.u8g2=&screens[i];
    solar_os_terminal_t terminal={.u8g2=&screens[0],.dirty=true,.status_bar_visible=true};
    assert(solar_os_terminal_needs_draw(&terminal));solar_os_terminal_draw(&terminal);
    assert(clears==1&&status_paints==1&&presents==1&&!solar_os_terminal_needs_draw(&terminal));
    assert(solar_os_display_set_terminal_blocked("display0","jet",true)==ESP_ERR_INVALID_STATE);
    display_targets[0].claim_refs=2;strcpy(display_targets[0].target.owner,"jet");
    assert(solar_os_display_set_terminal_blocked("display0","other",true)==ESP_ERR_INVALID_STATE);
    assert(solar_os_display_set_terminal_blocked("display0","jet",true)==ESP_OK);
    assert(solar_os_display_terminal_draw_state(&screens[1],NULL));
    assert(solar_os_display_terminal_draw_state(&screens[2],NULL));
    assert(!solar_os_terminal_needs_draw(&terminal));
    terminal.dirty=true;++fake_status_hash;
    for(unsigned i=0;i<100;++i)solar_os_terminal_draw(&terminal);
    assert(clears==1&&status_paints==1&&presents==1&&terminal.dirty);
    assert(solar_os_display_release("display0","jet")==ESP_OK);
    assert(!solar_os_display_terminal_draw_state(&screens[0],NULL));
    assert(solar_os_display_release("display0","jet")==ESP_OK);
    assert(solar_os_terminal_needs_draw(&terminal));solar_os_terminal_draw(&terminal);
    assert(clears==2&&status_paints==2&&presents==2&&!terminal.dirty);
    display_targets[0].claim_refs=1;strcpy(display_targets[0].target.owner,"jet");
    assert(solar_os_display_set_terminal_blocked("display0","jet",true)==ESP_OK);
    assert(solar_os_display_set_terminal_blocked("display0","jet",false)==ESP_OK);
    /* Even a short stream with no terminal changes needs a complete repaint. */
    assert(!terminal.dirty&&solar_os_terminal_needs_draw(&terminal));
    solar_os_terminal_draw(&terminal);assert(clears==3&&presents==3);
    puts("Display claims exclude terminal/status paints and restore a full terminal frame");
}
