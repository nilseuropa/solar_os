#include "solar_os_aogmaneo.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
extern int aon_test_leases, aon_test_missing, aon_test_release_failures, aon_test_destroy_failures;
extern void aon_test_clean(void);
static struct { solar_os_aogmaneo_context_t aogmaneo; } runtime;
static __typeof__(runtime) *solua_runtime_state = &runtime;
static bool cancelled;
static bool solua_should_cancel(void *user) { (void)user; return cancelled; }
static void test_delay(unsigned ticks) { (void)ticks; }
#define vTaskDelay test_delay
#define pdMS_TO_TICKS(n) (n)
#define SOLAR_OS_PACKAGE_SERVICE_AOGMANEO 1
static uint32_t solua_optional_u32(lua_State *L, int index, uint32_t fallback)
{
    if (lua_isnoneornil(L, index)) return fallback;
    lua_Number n = luaL_checknumber(L, index);
    if (!isfinite(n) || n < 0 || n > UINT32_MAX || floor(n) != n) luaL_error(L, "invalid uint32");
    return (uint32_t)n;
}
#include "solar_os_lua_aogmaneo.inc"
static void run(lua_State *L, const char *script)
{
    int result = luaL_dostring(L, script);
    if (result != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(result == LUA_OK);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    lua_State *L = luaL_newstate(); assert(L); luaL_openlibs(L);
    const luaL_Reg functions[] = {
        {"create", solua_aogmaneo_create}, {"destroy", solua_aogmaneo_destroy},
        {"step", solua_aogmaneo_step}, {"prediction", solua_aogmaneo_prediction},
        {"activations", solua_aogmaneo_activations}, {"values", solua_aogmaneo_values},
        {"hidden", solua_aogmaneo_hidden}, {"clear", solua_aogmaneo_clear},
        {"image_create", solua_aogmaneo_image_create}, {"image_step", solua_aogmaneo_image_step},
        {"image_reconstruct", solua_aogmaneo_image_reconstruct}, {NULL, NULL},
    };
    luaL_newlib(L, functions); lua_setglobal(L, "a");
    lua_newtable(L); lua_getglobal(L, "a"); lua_setfield(L, -2, "aogmaneo"); lua_setglobal(L, "solaros");
    aon_test_missing = 1;
    run(L, "assert(not pcall(a.create, {{size={1,1,4}}}, {{size={2,2,8}}}))");
    assert(!aon_test_leases); aon_test_missing = 0;
    run(L,
        "ios={{size={1,1,4}}, {size={1,1,4},type=2,value_size=8,history_capacity=4}}; "
        "layers={{size={2,2,8}},{size={1,2,8},recurrent_radius=-1}}; m=a.create(ios,layers,12345); "
        "assert(not pcall(a.prediction,m)); "
        "for i=0,39 do a.step(m,{{i%4},{i%4}},true,0.5,0.2) end; "
        "assert(#a.prediction(m)==1 and #a.activations(m)==4 and #a.hidden(m)==4); "
        "assert(#a.values(m,1)==1 and not pcall(a.values,m,0)); "
        "assert(not pcall(a.step,m,{{4},{0}})); assert(not pcall(a.step,m,{{0}})); "
        "assert(not pcall(a.step,m,{{0},{0}},true,0,2)); "
        "assert(not pcall(a.hidden,m,9)); a.clear(m); assert(not pcall(a.prediction,m)); "
        "a.step(m,{{0},{0}},false); assert(#a.prediction(m)==1); "
        "assert(not pcall(a.create,{{size={1,1,4},typo=2}},layers)); "
        "assert(not pcall(a.create,{{size={1,1,4},dendrites=3}},layers)); "
        "assert(not pcall(a.create,{{size={2147483647,1,4}}},layers)); "
        "assert(not pcall(a.create,ios,layers,-1)); "
        "im=a.image_create({2,2,8},{{size={4,4,1},radius=1},{size={2,2,3},radius=1}}); "
        "data={string.rep(string.char(0,128,255,64),4),string.rep(string.char(0,128,255),4)}; "
        "for i=1,8 do codes=a.image_step(im,data,true,true) end; "
        "assert(#codes==4 and #a.image_reconstruct(im,codes)==16 and #a.image_reconstruct(im,codes,1)==12); "
        "assert(not pcall(a.clear,im)); assert(not pcall(a.image_step,m,data)); "
        "assert(not pcall(a.image_step,im,{'short',data[2]})); "
        "assert(not pcall(a.image_reconstruct,im,{-1,0,0,0})); "
        "a.destroy(im); assert(not pcall(a.image_step,im,data)); "
        "old=m; a.destroy(m); m=a.create(ios,layers); assert(m~=old and not pcall(a.clear,old));");
    assert(aon_test_leases == 1);
    aon_test_destroy_failures = 1;
    run(L, "assert(not pcall(a.destroy,m))"); assert(aon_test_leases == 1);
    aon_test_release_failures = 1;
    run(L, "assert(not pcall(a.destroy,m))"); assert(aon_test_leases == 1);
    run(L, "a.destroy(m); m=a.create(ios,layers)");
    assert(!solua_aogmaneo_active());
    cancelled = true;
    run(L, "assert(not pcall(a.step,m,{{0},{0}}))");
    cancelled = false;
    aon_test_destroy_failures = 1;
    solua_aogmaneo_cleanup(); aon_test_clean();
    assert(luaL_dostring(L, "m=a.create(ios,layers); error('script failed')") != LUA_OK);
    solua_aogmaneo_cleanup(); aon_test_clean();
    int example = luaL_dofile(L, argv[1]);
    if (example != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(example == LUA_OK); aon_test_clean();
    lua_close(L);
    puts("Lua AOgmaNeo bindings with real interpreter/engine and failure cleanup passed");
    return 0;
}
