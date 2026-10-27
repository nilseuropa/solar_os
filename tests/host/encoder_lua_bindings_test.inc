#define main encoder_registry_suite
#include "encoder_service_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "encoder_binding_expansion_test.inc"

static int solua_check_esp(lua_State *L, esp_err_t error)
{ return error == ESP_OK ? 0 : luaL_error(L, "encoder error %d", error); }
static void solua_set_str(lua_State *L, int table, const char *key, const char *value)
{ table = lua_absindex(L, table); lua_pushstring(L, value); lua_setfield(L, table, key); }
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{ table = lua_absindex(L, table); lua_pushinteger(L, value); lua_setfield(L, table, key); }
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{ table = lua_absindex(L, table); lua_pushboolean(L, value); lua_setfield(L, table, key); }

/* ENCODER_BINDINGS */

int main(void)
{
    fake_t a = {.value = INT32_MIN, .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    fake_t b = {.value = INT32_MAX, .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const solar_os_encoder_ops_t full = {.read = read_count, .reset = reset_count};
    const solar_os_encoder_ops_t basic = {.read = read_count};
    const solar_os_encoder_registration_t one = {.name = "left", .driver = "pcnt", .mode = 4, .ops = &full, .ctx = &a};
    const solar_os_encoder_registration_t two = {.name = "right", .driver = "external", .mode = 2, .ops = &basic, .ctx = &b};
    assert(solar_os_encoder_register(&one) == ESP_OK && solar_os_encoder_register(&two) == ESP_OK);
    lua_State *L = luaL_newstate(); assert(L); luaL_openlibs(L);
    const luaL_Reg api[] = {{"list", solua_encoder_list}, {"status", solua_encoder_status},
        {"read", solua_encoder_read}, {"reset", solua_encoder_reset}, {"attach", solua_expansion_attach}, {NULL, NULL}};
    luaL_newlib(L, api); lua_setglobal(L, "encoder");
    const char *script =
        "local es = encoder.list(); assert(#es == 2 and es[1].mode == 4 and not es[2].can_reset)\n"
        "assert(encoder.read('left') == -2147483648 and encoder.read('right') == 2147483647)\n"
        "local s = encoder.status('left'); assert(s.count == -2147483648 and s.driver == 'pcnt')\n"
        "local n = 0; for k,v in pairs(s) do n = n + 1 end; assert(n == 5)\n"
        "encoder.reset('left'); assert(encoder.read('left') == 0)\n"
        "assert(not pcall(encoder.reset, 'right'))\n"
        "assert(not pcall(encoder.read, 'missing'))\n"
        "assert(not pcall(encoder.status, {}))\n"
        "encoder.attach('pcnt-encoder', 'wheel', {a=1, b=2, mode=4, invert=1, pull=2, filter_ns=1000})\n"
        "assert(not pcall(encoder.attach, 'pcnt-encoder', 'wheel', {a=1, b=2, mode=4, typo=1}))\n"
        "assert(not pcall(encoder.attach, 'pcnt-encoder', 'wheel', {a=1, b=2, mode='x4'}))\n"
        "assert(not pcall(encoder.attach, 'pcnt-encoder', 'wheel', {a=1, b=2, mode=4294967300}))\n";
    const int ret = luaL_dostring(L, script);
    if (ret != LUA_OK) fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1));
    assert(ret == LUA_OK); check_attachment();
    assert(solar_os_encoder_unregister("left") == ESP_OK && solar_os_encoder_unregister("right") == ESP_OK);
    assert(luaL_dostring(L, "assert(#encoder.list() == 0)") == LUA_OK);
    lua_close(L); assert(!allocations);
    puts("Lua encoder bindings: real interpreter, signed bounds, errors and expansion encoding PASS");
    return 0;
}
