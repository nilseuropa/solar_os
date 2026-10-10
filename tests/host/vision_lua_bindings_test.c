/* Compile the actual vision binding section, alongside the native service and
 * JPEG/PNG/ownership suite. Only the enclosing interpreter context is stubbed. */
#define main native_vision_suite
#include "vision_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static bool fail_lua, fail_result;
static struct { solar_os_raster_image_t *images[1]; } solua;
static size_t solua_image_slot(lua_State *L, int index)
{
    if (luaL_checkinteger(L, index) != 1 || !solua.images[0]) luaL_error(L, "invalid image");
    return 0;
}
static bool solua_should_cancel(void *user) { (void)user; return false; }
static int solua_check_esp(lua_State *L, esp_err_t err)
{
    if (err != ESP_OK) return luaL_error(L, "vision error %d", err);
    if (fail_result) fail_lua = true;
    return 0;
}
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{ table = lua_absindex(L, table); lua_pushinteger(L, value); lua_setfield(L, table, key); }
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{ table = lua_absindex(L, table); lua_pushboolean(L, value); lua_setfield(L, table, key); }
static uint32_t solua_media_option(lua_State *L, const char *key, uint32_t fallback, uint32_t max)
{
    if (lua_isnoneornil(L, 2)) return fallback;
    lua_getfield(L, 2, key);
    lua_Integer n = lua_isnil(L, -1) ? fallback : luaL_checkinteger(L, -1);
    if (n < 0 || (uint64_t)n > max) luaL_error(L, "option out of range");
    lua_pop(L, 1); return n;
}
static uint16_t solua_check_u16_size(lua_State *L, int index)
{
    lua_Integer n = luaL_checkinteger(L, index);
    if (n < 0 || n > UINT16_MAX) luaL_error(L, "dimension out of range");
    return n;
}
#include "solar_os_lua_image_rgb.inc"
#define SOLAR_OS_PACKAGE_SERVICE_VISION 1
#include "solar_os_lua_vision.inc"

static void *lua_alloc(void *user, void *ptr, size_t old, size_t size)
{
    (void)user; (void)old;
    if (!size) { free(ptr); return NULL; }
    if (fail_lua) return NULL;
    return realloc(ptr, size);
}
static void run(lua_State *L, const char *script)
{
    int status = luaL_dostring(L, script);
    if (status != LUA_OK) fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK); lua_settop(L, 0);
}
int main(void)
{
    assert(native_vision_suite() == 0);
    solua.images[0] = open_fixture("multiple.png");
    int baseline = allocations;
    lua_State *L = lua_newstate(lua_alloc, NULL); assert(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    lua_pushcfunction(L, solua_vision_qrcodes); lua_setglobal(L, "qrcodes");
    lua_pushcfunction(L, solua_image_to_rgb); lua_setglobal(L, "to_rgb");
    run(L, "retained_rgb = to_rgb(1,2,1); assert(#retained_rgb == 6); "
        "assert(#to_rgb(1) == 416*198*3); "
        "assert(not pcall(to_rgb,99)); assert(not pcall(to_rgb,1,2)); "
        "assert(not pcall(to_rgb,1,0,1)); assert(not pcall(to_rgb,1,-1,1)); "
        "assert(not pcall(to_rgb,1,65536,1)); "
        "assert(not pcall(to_rgb,1,2048,2048)); "
        "assert(not pcall(to_rgb,1,1,1,1))");
    solar_os_raster_image_pixels_t pixels;
    assert(solar_os_raster_image_pixels(solua.images[0], &pixels) == ESP_OK);
    lua_getglobal(L, "retained_rgb");
    size_t rgb_size;
    const char *rgb = lua_tolstring(L, -1, &rgb_size);
    assert(rgb_size == 6 && !memcmp(rgb, pixels.data, 3) &&
        !memcmp(rgb + 3, pixels.data + (416 / 2) * 3, 3));
    lua_pop(L, 1);
    run(L, "local r = qrcodes(1); assert(#r.codes == 2); "
        "assert(r.width == 416 and r.processed_height == 198 and r.elapsed_us > 0); "
        "assert(r.codes[1].payload == 'SolarOS QR' .. string.char(0) .. 'binary' .. string.char(255)); "
        "assert(#r.codes[1].corners == 4 and #r.codes[1].corners[1] == 2); "
        "r = qrcodes(1,{x=218,width=198,height=198,output_width=99,output_height=99}); "
        "assert(#r.codes == 1 and r.codes[1].corners[1][1] >= 218); "
        "assert(not pcall(qrcodes,99)); assert(not pcall(qrcodes,1,{x=-1})); "
        "assert(not pcall(qrcodes,1,{output_width=641})); "
        "assert(not pcall(qrcodes,1,{output_widht=99})); "
        "assert(not pcall(qrcodes,1,{[2]=99})); "
        "assert(not pcall(qrcodes,1,{x=4294967295})); "
        "assert(not pcall(qrcodes,1,'invalid')); retained = r.codes[1].payload");
    assert(allocations == baseline && !workers);
    lua_pushcfunction(L, solua_vision_qrcodes); lua_pushinteger(L, 1);
    fail_result = true;
    int status = lua_pcall(L, 1, 1, 0);
    fail_result = fail_lua = false;
    assert(status == LUA_ERRMEM && allocations == baseline && !workers);
    lua_settop(L, 0);
    lua_pushcfunction(L, solua_image_to_rgb);
    lua_pushinteger(L, 1); lua_pushinteger(L, 224); lua_pushinteger(L, 224);
    fail_result = true;
    status = lua_pcall(L, 3, 1, 0);
    fail_result = fail_lua = false;
    assert(status == LUA_ERRMEM && allocations == baseline && !workers);
    lua_settop(L, 0);
    solar_os_raster_image_release(solua.images[0]); solua.images[0] = NULL;
    assert(!allocations);
    run(L, "assert(#retained == 18); assert(#retained_rgb == 6); "
        "assert(not pcall(qrcodes,1)); assert(not pcall(to_rgb,1))");
    lua_close(L);
    puts("actual Lua QR and RGB copy/resize/ownership/result-OOM cleanup tests passed");
    return 0;
}
