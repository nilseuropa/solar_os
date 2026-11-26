#pragma once
#include <stdio.h>
#include "display_canvas_fixture.h"
#include "solar_os_terminal_internal.h"
#include "solar_os_native_terminal.h"
#include "solar_os_native_terminal_abi.h"
#include "nvs.h"

static unsigned frames, commits;
static bool fail_commit;
static struct { nvs_handle_t handle; char key[20]; uint16_t value; } preferences[16];
static unsigned preference_count;
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)mode;
    if (!strcmp(name, "terminal")) *handle = 1;
    else if (!strcmp(name, "display_scale")) *handle = 2;
    else { assert(!strcmp(name, "display_orient")); *handle = 3; }
    return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    assert(handle >= 1 && handle <= 3);
    for(unsigned i=0;i<preference_count;++i) if(handle == preferences[i].handle && !strcmp(key,preferences[i].key)) { *value=preferences[i].value; return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    assert(handle >= 1 && handle <= 3); unsigned i;
    for(i=0;i<preference_count;++i) if(handle == preferences[i].handle && !strcmp(key,preferences[i].key)) break;
    assert(i<16); if(i==preference_count) ++preference_count;
    preferences[i].handle = handle;
    snprintf(preferences[i].key,sizeof(preferences[i].key),"%s",key); preferences[i].value=value; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    uint16_t stored;
    esp_err_t result = nvs_get_u16(handle, key, &stored);
    if (result == ESP_OK) *value = (uint8_t)stored;
    return result;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ return nvs_set_u16(handle, key, value); }
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle >= 1 && handle <= 3); ++commits; return fail_commit ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle >= 1 && handle <= 3); }
int64_t esp_timer_get_time(void) { return 1000000; }
uint32_t solar_os_font_pack_generation(void) { return 0; }
bool solar_os_display_is_epaper(const u8g2_t *u) { (void)u; return true; }
bool solar_os_display_terminal_draw_state(const u8g2_t *u, uint32_t *epoch)
{ assert(u); *epoch=1; return true; }
void solar_os_display_present(u8g2_t *u, solar_os_display_present_mode_t mode)
{ assert(u && mode == SOLAR_OS_DISPLAY_PRESENT_TEXT); ++frames; }
void solar_os_display_present_regions(u8g2_t *u, solar_os_display_present_mode_t mode,
    const solar_os_display_rect_t *areas, size_t count)
{ assert((!count || areas)); solar_os_display_present(u,mode); }
size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t len=strlen(src); if(size) { size_t n=len<size?len:size-1; memcpy(dst,src,n);dst[n]=0; } return len; }
