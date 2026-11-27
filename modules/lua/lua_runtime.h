#pragma once

#include "lauxlib.h"
#include "solar_os_native_client.h"

typedef struct {
    const solar_os_native_host_api_v1_t *host;
    const solar_os_native_foreground_api_v1_t *foreground;
    const solar_os_native_memory_api_v1_t *memory;
    const solar_os_native_libc_api_v1_t *libc;
    const solar_os_native_fs_api_v1_t *fs;
    const solar_os_native_gfx_api_v1_t *graphics;
    const solar_os_native_display_api_v1_t *display;
    const solar_os_native_audio_api_v1_t *audio;
    const solar_os_native_jet_api_v1_t *jet;
    const solar_os_native_voice_api_v1_t *voice;
    solar_os_gfx_t *surface;
    solar_os_native_handle_t scene, synth;
    uint32_t tone;
    bool exiting;
    int status;
} solar_os_lua_runtime_t;

extern solar_os_lua_runtime_t lua_runtime;

void solar_os_lua_write(const char *text, size_t length);
int solar_os_lua_read_key(uint32_t timeout_ms);
bool solar_os_lua_stopping(void);
int solar_os_lua_check(lua_State *L, int result);
lua_Integer solar_os_lua_integer(lua_State *L, int index, lua_Integer minimum,
                                lua_Integer maximum, lua_Integer fallback);
void solar_os_lua_set_number(lua_State *L, const char *name, lua_Integer value);
void solar_os_lua_graphics(lua_State *L);
void solar_os_lua_audio(lua_State *L);
void solar_os_lua_jet(lua_State *L);
