#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "solar_os_native_client.h"
#include "solar_os_native_dsp.h"
#include "solar_os_native_handles.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static bool available = true;
static unsigned lookups;
static const void *script_query_dsp(const char *name, uint32_t abi, uint32_t minimum) {
    ++lookups;
    return available ? solar_os_native_dsp_get_service(name, abi, minimum) : NULL;
}
#define SOLAR_OS_SCRIPT_SERVICE_QUERY(id) script_query_##id

static void solua_set_func(lua_State *L, int table, const char *name, lua_CFunction function) {
    lua_pushcfunction(L, function);
    lua_setfield(L, table, name);
}
static void solua_set_int(lua_State *L, int table, const char *name, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, table, name);
}
static void solua_set_str(lua_State *L, int table, const char *name, const char *value) {
    lua_pushstring(L, value);
    lua_setfield(L, table, name);
}
static size_t solua_check_size(lua_State *L, int index) {
    lua_Integer n = luaL_checkinteger(L, index);
    if (n < 0 || (uint64_t)n > SIZE_MAX)
        luaL_error(L, "bad size");
    return (size_t)n;
}
#include "../../src/apps/solar_os_lua_dsp.inc"
static void run(lua_State *L, const char *source) {
    if (luaL_dostring(L, source) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        assert(0);
    }
}
int main(void) {
    lua_State *L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    solua_dsp_register_type(L);
    lua_newtable(L);
    int table = lua_gettop(L);
#define EXPOSE(name, function) solua_set_func(L, table, name, function)
    EXPOSE("backend", solua_dsp_backend);
    EXPOSE("capabilities", solua_dsp_capabilities);
    EXPOSE("dot", solua_dsp_dot);
    EXPOSE("gain", solua_dsp_gain);
    EXPOSE("mix", solua_dsp_mix);
    EXPOSE("clip", solua_dsp_clip);
    EXPOSE("level", solua_dsp_level);
    EXPOSE("window", solua_dsp_window);
    EXPOSE("fir", solua_dsp_fir);
    EXPOSE("fft", solua_dsp_fft);
    lua_setglobal(L, "dsp");
    run(L, "assert(dsp.backend()=='portable'); assert(dsp.capabilities().supported>0); "
           "a=string.pack('<i2i2i2i2',-32768,-1000,1000,32767); "
           "b=string.pack('<i2i2i2i2',-32768,2000,-2000,32767); "
           "assert(dsp.dot(a,b)==32768*32768-4000000+32767*32767); "
           "assert(dsp.gain(a,0.5)==string.pack('<i2i2i2i2',-16384,-500,500,16383)); "
           "assert(dsp.mix(a,b,1,1)==string.pack('<i2i2i2i2',-32768,999,-1000,32767)); "
           "assert(dsp.clip(a,-900,900)==string.pack('<i2i2i2i2',-900,-900,900,900)); "
           "local peak,rms=dsp.level(string.pack('<i2i2',-3,4));assert(peak==4 and rms==3); "
           "assert(dsp.window(a,string.pack('<i2i2i2i2',32767,16384,0,-32768))==string.pack('<"
           "i2i2i2i2',-32767,-500,0,-32767)); "
           "f=dsp.fir(string.pack('<i2i2',16384,16384));collectgarbage('collect'); "
           "assert(f:process(string.pack('<i2i2i2i2',1000,2000,3000,4000))==string.pack('<i2i2i2i2'"
           ",500,1500,2500,3500)); "
           "f:reset(); assert(f:process(string.pack('<i2',1000))==string.pack('<i2',500)); "
           "g=dsp.fir(string.pack('<i2',32767),2); "
           "assert(g:process(string.pack('<i2i2i2',10,20,30))==string.pack('<i2i2',9,29)); "
           "assert(g:process(string.pack('<i2i2i2',40,50,60))==string.pack('<i2',49)); "
           "fft=dsp.fft(8);local "
           "spectrum,scale=fft:execute(string.pack('<i2i2i2i2i2i2i2i2',32767,0,0,0,0,0,0,0)); "
           "assert(scale==3 and #spectrum==32);for i=1,8 do local "
           "real,imag=string.unpack('<i2i2',spectrum,(i-1)*4+1);assert(real==4095 and imag==0)end");
    unsigned cached = lookups;
    available = false;
    run(L,
        "f:reset();f:process(string.pack('<i2',1000));g:reset();fft:execute(string.rep('\\0',16)); "
        "assert(not pcall(dsp.backend)); f:close();f:close();assert(not pcall(function() "
        "f:process(a)end));g:close();fft:close()");
    assert(lookups == cached + 1 && !solar_os_native_handle_count());
    available = true;
    run(L, "f=dsp.fir(string.pack('<i2',32767)); g=dsp.fft(8); assert(not pcall(dsp.fft,3)); "
           "assert(not pcall(dsp.fir,'x')); "
           "f=nil;g=nil;fft=nil;collectgarbage('collect')");
    assert(!solar_os_native_handle_count());
    lua_close(L);
    puts("Lua DSP bindings: real interpreter, ABI discovery, arithmetic, streaming, FFT, cached "
         "calls and GC passed");
    return 0;
}
