#define main motor_registry_suite
#include "motor_service_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int solua_check_esp(lua_State *L, esp_err_t error)
{ return error == ESP_OK ? 0 : luaL_error(L, "motor error %d", error); }
static void solua_set_str(lua_State *L, int table, const char *key, const char *value)
{ table = lua_absindex(L, table); lua_pushstring(L, value); lua_setfield(L, table, key); }
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{ table = lua_absindex(L, table); lua_pushinteger(L, value); lua_setfield(L, table, key); }
static void solua_set_num(lua_State *L, int table, const char *key, lua_Number value)
{ table = lua_absindex(L, table); lua_pushnumber(L, value); lua_setfield(L, table, key); }
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{ table = lua_absindex(L, table); lua_pushboolean(L, value); lua_setfield(L, table, key); }

/* MOTOR_BINDINGS */

int main(void)
{
    fake_t a = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    fake_t b = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const solar_os_motor_ops_t full = {.status = status, .drive = drive, .coast = coast, .brake = brake, .clear = clear};
    const solar_os_motor_ops_t basic = {.status = status, .drive = drive, .coast = coast};
    const solar_os_motor_registration_t one = {.name = "left", .driver = "voltage-backend", .ops = &full, .ctx = &a};
    const solar_os_motor_registration_t two = {.name = "right", .driver = "pwm-backend", .ops = &basic, .ctx = &b};
    assert(solar_os_motor_register(&one) == ESP_OK && solar_os_motor_register(&two) == ESP_OK);
    lua_State *L = luaL_newstate(); assert(L); luaL_openlibs(L);
    const luaL_Reg api[] = {{"list", solua_motor_list}, {"status", solua_motor_status},
        {"drive", solua_motor_drive}, {"coast", solua_motor_coast},
        {"brake", solua_motor_brake}, {"clear", solua_motor_clear}, {NULL, NULL}};
    luaL_newlib(L, api); lua_setglobal(L, "motor");
    const char *script =
        "local motors = motor.list(); assert(#motors == 2)\n"
        "assert(motors[1].name == 'left' and motors[2].driver == 'pwm-backend')\n"
        "assert(motors[1].can_brake and not motors[2].can_clear)\n"
        "motor.drive('left', -0.5); local s = motor.status('left')\n"
        "assert(s.mode == 'reverse' and s.drive == -0.5 and s.drive_valid)\n"
        "assert(not s.voltage_valid and not s.raw_valid and s.blocking_faults == 0)\n"
        "local n = 0; for k, v in pairs(s) do n = n + 1 end; assert(n == 17)\n"
        "motor.brake('left'); motor.clear('left'); motor.coast('right')\n"
        "for _, x in ipairs({1.1, -1.1, math.huge, 0/0, 'bad', {}}) do\n"
        "  assert(not pcall(motor.drive, 'left', x))\n"
        "end\n"
        "assert(not pcall(motor.brake, 'right'))\n"
        "assert(not pcall(motor.status, 'missing'))\n";
    const int ret = luaL_dostring(L, script);
    if (ret != LUA_OK) fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1));
    assert(ret == LUA_OK && a.clears == 1 && a.brakes == 1 && a.value == 0 && b.value == 0);
    assert(solar_os_motor_unregister("left") == ESP_OK && solar_os_motor_unregister("right") == ESP_OK);
    assert(luaL_dostring(L, "assert(#motor.list() == 0)") == LUA_OK);
    lua_close(L);
    assert(allocations == 0);
    puts("Lua motor bindings: real interpreter, records, two providers, normalized controls and errors PASS");
}
