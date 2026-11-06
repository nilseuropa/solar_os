#define main controllers_registry_suite
#include "controllers_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
static int solua_check_esp(lua_State *L, esp_err_t error)
{ return error == ESP_OK ? 0 : luaL_error(L, "controller error %d", error); }
static void solua_set_str(lua_State *L, int table, const char *key, const char *value)
{ table = lua_absindex(L, table); lua_pushstring(L, value); lua_setfield(L, table, key); }
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{ table = lua_absindex(L, table); lua_pushinteger(L, value); lua_setfield(L, table, key); }
static void solua_set_num(lua_State *L, int table, const char *key, lua_Number value)
{ table = lua_absindex(L, table); lua_pushnumber(L, value); lua_setfield(L, table, key); }
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{ table = lua_absindex(L, table); lua_pushboolean(L, value); lua_setfield(L, table, key); }
#define SOLAR_OS_PACKAGE_SERVICE_CONTROLLERS 1
#include "solar_os_lua_controllers.inc"
int main(void)
{
    register_output();
    lua_State *L = luaL_newstate(); assert(L); luaL_openlibs(L);
    const luaL_Reg api[] = {
        {"list", solua_controllers_list},
        {"status", solua_controllers_status},
        {"create", solua_controllers_create},
        {"configure", solua_controllers_configure},
        {"bind", solua_controllers_bind},
        {"feed", solua_controllers_feed},
        {"step", solua_controllers_step},
        {"manual", solua_controllers_manual},
        {"auto", solua_controllers_auto},
        {"reset", solua_controllers_reset},
        {"start", solua_controllers_start},
        {"tune", solua_controllers_tune},
        {"stop", solua_controllers_stop},
        {"delete", solua_controllers_delete},
        {NULL, NULL}
    };
    luaL_newlib(L, api); lua_setglobal(L, "controllers");
    const char *script =
        "local c = controllers.create('loop'); assert(c.kind == 'pid' and c.ki > 0)\n"
        "assert(#controllers.list() == 1)\n"
        "controllers.configure('loop', 'setpoint', 1)\n"
        "local u = controllers.step('loop', 0.5, 0.02); assert(u > 0.5 and u < 0.51)\n"
        "controllers.manual('loop', 0.4); controllers.auto('loop')\n"
        "controllers.bind('loop', 'manual', 'parameter:plant.drive')\n"
        "controllers.feed('loop', 0.3); controllers.start('loop')\n"
        "assert(controllers.status('loop').enabled)\n"
        "assert(not pcall(controllers.step, 'loop', 0, 0.02))\n"
        "controllers.stop('loop'); controllers.reset('loop')\n"
        "local t = controllers.tune('loop', .01, 3, 2000)\n"
        "assert(t.tune_state == 'running' and t.tune_required_cycles == 3 and t.tune_kp == 0)\n"
        "assert(not pcall(controllers.configure, 'loop', 'setpoint', 2))\n"
        "controllers.stop('loop'); assert(controllers.status('loop').tune_state == 'aborted')\n"
        "assert(not pcall(controllers.tune, 'loop', .01, 3.5, 2000))\n"
        "assert(not pcall(controllers.tune, 'loop', .01, 3, 0/0))\n"
        "controllers.reset('loop'); controllers.delete('loop')\n"
        "controllers.create('relay', 'hysteresis'); assert(not pcall(controllers.tune, 'relay')); controllers.delete('relay')\n"
        "assert(#controllers.list() == 0)\n"
        "assert(not pcall(controllers.create, 'bad', 'unknown'))\n"
        "assert(not pcall(controllers.status, 'missing'))\n";
    int result = luaL_dostring(L, script);
    if (result != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(result == LUA_OK); lua_close(L);
    puts("Lua controller bindings passed");
    return 0;
}
