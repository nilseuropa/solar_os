#define main robot_registry_suite
#include "robot_integration_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
static int solua_check_esp(lua_State *L,esp_err_t e) { return e==ESP_OK?0:luaL_error(L,"device error %d",e); }
static void solua_set_str(lua_State *L,int d,const char *k,const char *v) { d=lua_absindex(L,d);lua_pushstring(L,v);lua_setfield(L,d,k); }
static void solua_set_int(lua_State *L,int d,const char *k,lua_Integer v) { d=lua_absindex(L,d);lua_pushinteger(L,v);lua_setfield(L,d,k); }
static void solua_set_num(lua_State *L,int d,const char *k,lua_Number v) { d=lua_absindex(L,d);lua_pushnumber(L,v);lua_setfield(L,d,k); }
static uint32_t solua_optional_u32(lua_State *L,int n,uint32_t fallback) {
    if(lua_isnoneornil(L,n))return fallback;
    lua_Number x=luaL_checknumber(L,n);if(!isfinite(x)||x<0||x>UINT32_MAX||floor(x)!=x)luaL_error(L,"invalid uint32");return (uint32_t)x;
}
#define SOLAR_OS_PACKAGE_SERVICE_ROBOT 1
#define SOLAR_OS_LOGW(...) ((void)0)
#define TAG "test"
const char *esp_err_to_name(esp_err_t e) { (void)e;return "error"; }
#include "solar_os_lua_robot.inc"
int main(void)
{
    setup();lua_State *L=luaL_newstate();assert(L);luaL_openlibs(L);
    const luaL_Reg api[]={
        {"joints",solua_robot_joints},
        {"joint_create",solua_robot_joint_create},
        {"joint_configure",solua_robot_joint_configure},
        {"joint_bind_motor",solua_robot_joint_bind_motor},
        {"joint_bind_servo",solua_robot_joint_bind_servo},
        {"joint_command",solua_robot_joint_command},
        {"joint_enable",solua_robot_joint_enable},
        {"joint_disable",solua_robot_joint_disable},
        {"joint_status",solua_robot_joint_status},
        {"joint_close",solua_robot_joint_close},
        {NULL,NULL}};
    luaL_newlib(L,api);lua_setglobal(L,"robot");
    const char *script=
        "local s=robot.joint_create('wheel');assert(s.state=='ready')\n"
        "robot.joint_configure('wheel','ppr',70);robot.joint_configure('wheel','gear_ratio',30)\n"
        "assert(not pcall(robot.joint_configure,'wheel','encoder_counts_per_rev',2098))\n"
        "robot.joint_configure('wheel','motor_polarity',-1)\n"
        "robot.joint_configure('wheel','friction_bias_forward',0.15);robot.joint_configure('wheel','friction_bias_reverse',0.12)\n"
        "robot.joint_configure('wheel','velocity_deadband',0.05)\n"
        "robot.joint_bind_motor('wheel','left','left_enc');robot.joint_enable('wheel')\n"
        "assert(robot.joint_status('wheel').state=='waiting');assert(robot.joint_status('wheel').encoder_counts_per_rev==2100)\n"
        "robot.joint_command('wheel',1);assert(robot.joint_status('wheel').motor_polarity==-1)\n"
        "local c=robot.joint_status('wheel');assert(math.abs(c.friction_bias_forward-0.15)<1e-6 and math.abs(c.friction_bias_reverse-0.12)<1e-6)\n"
        "robot.joint_command('wheel',0.01);assert(robot.joint_status('wheel').target==0)\n"
        "assert(#robot.joints()==1);robot.joint_disable('wheel');robot.joint_close('wheel')\n"
        "robot.joint_create('pan','servo');robot.joint_bind_servo('pan','native',2,1000,2000,-1,1)\n"
        "assert(not pcall(robot.joint_command,'pan',2))\n";
    int ret=luaL_dostring(L,script);if(ret!=LUA_OK)fprintf(stderr,"%s\n",lua_tostring(L,-1));assert(ret==LUA_OK);
    solua_robot_destroy();assert(!solar_os_robot_joint_count());lua_close(L);teardown();
    puts("Lua robot API with real interpreter and cleanup passed");return 0;
}
