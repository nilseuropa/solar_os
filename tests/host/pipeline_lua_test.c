/* Actual Lua binding against the native pipeline and controlled host sources. */
#define main native_pipeline_suite
#include "pipeline_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "solar_os_storage.h"
static int lua_fail_after = -1;
static int solua_check_esp(lua_State *L, esp_err_t error)
{ if (error) return luaL_error(L, "pipeline error %d", error); return 0; }
static void solua_set_int(lua_State *L, int t, const char *key, lua_Integer n)
{ t = lua_absindex(L, t); lua_pushinteger(L, n); lua_setfield(L, t, key); }
static void solua_set_str(lua_State *L, int t, const char *key, const char *s)
{ t = lua_absindex(L, t); lua_pushstring(L, s); lua_setfield(L, t, key); }
static void solua_resolve_path(lua_State *L, int i, char *path, size_t n)
{ snprintf(path, n, "%s", luaL_checkstring(L, i)); }
#define SOLAR_OS_PACKAGE_SERVICE_PIPELINE 1
#include "solar_os_lua_pipeline.inc"
static void *lua_alloc(void *user, void *ptr, size_t old, size_t size)
{
    (void)user; (void)old;
    if (!size) { free(ptr); return NULL; }
    if (lua_fail_after == 0) return NULL;
    if (lua_fail_after > 0) --lua_fail_after;
    return realloc(ptr, size);
}
static lua_State *state(void)
{
    lua_State *L = lua_newstate(lua_alloc, NULL); assert(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    const luaL_Reg api[] = {{"start",solua_pipeline_start},{"list",solua_pipeline_list},
        {"status",solua_pipeline_status},{"result",solua_pipeline_result},
        {"stop",solua_pipeline_stop},{"destroy",solua_pipeline_destroy},{NULL,NULL}};
    luaL_newlib(L, api); lua_setglobal(L, "pipeline"); return L;
}
static void run(lua_State *L, const char *script)
{ int status = luaL_dostring(L, script); if (status) fprintf(stderr, "%s\n", lua_tostring(L,-1)); assert(status == LUA_OK); lua_settop(L,0); }
int main(void)
{
    lua_State *L = state();
    run(L,"p=pipeline.start('/test.png'); assert(type(p)=='number')");
    lua_getglobal(L,"p"); uint32_t id = lua_tointeger(L,-1); lua_settop(L,0);
    finish(id); lua_close(L);
    /* A new interpreter discovers and reads the still-owned native record. */
    L = state();
    run(L,"p=pipeline.list()[1]; local s=pipeline.status(p); assert(s.done and s.frames==1); "
        "local r=pipeline.result(p); assert(r.sequence==1 and r.result.codes[1].payload_hex=='00ff41'); "
        "assert(pipeline.result(p,r.sequence)==nil)");
    int baseline = allocations;
    for (int i = 0; i < 120; ++i) {
        lua_pushcfunction(L, solua_pipeline_result); lua_pushinteger(L,id);
        lua_fail_after = i;
        int status = lua_pcall(L,1,1,0);
        lua_fail_after = -1;
        assert(status == LUA_OK || status == LUA_ERRMEM || status == LUA_ERRRUN);
        lua_settop(L,0); assert(allocations == baseline);
    }
    run(L,"pipeline.stop(p); pipeline.destroy(p); assert(#pipeline.list()==0); "
        "assert(not pcall(pipeline.status,p)); assert(not pcall(pipeline.start,'/test.png','qr',-1)); "
        "assert(not pcall(pipeline.start,'/test.png','qr\\0invalid'))");
    lua_close(L); assert(!allocations && !workers && !references);
    puts("native pipeline Lua lifetime/OOM tests passed");
}
