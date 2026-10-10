/* Actual Lua binding with the native service; fake numerical backend. */
#define main native_inference_suite
#include "inference_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "solar_os_storage.h"
static bool fail_lua, fail_result;
static int fail_lua_after = -1;
static bool solua_should_cancel(void *user) { (void)user; return false; }
static int solua_check_esp(lua_State *L, esp_err_t err)
{
    if (err) return luaL_error(L, "inference error %d", err);
    if (fail_result && atomic_load(&backend_entered)) fail_lua = true;
    return 0;
}
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer v)
{ table = lua_absindex(L, table); lua_pushinteger(L, v); lua_setfield(L, table, key); }
static void solua_set_str(lua_State *L, int table, const char *key, const char *v)
{ table = lua_absindex(L, table); lua_pushstring(L, v); lua_setfield(L, table, key); }
static void solua_resolve_path(lua_State *L, int index, char *out, size_t size)
{ snprintf(out, size, "%s", luaL_checkstring(L, index)); }
#define SOLAR_OS_PACKAGE_SERVICE_INFERENCE 1
#include "solar_os_lua_inference.inc"
static void *lua_alloc(void *user, void *ptr, size_t old, size_t size)
{
    (void)user; (void)old;
    if (!size) { free(ptr); return NULL; }
    if (fail_lua_after == 0) return NULL;
    if (fail_lua_after > 0) --fail_lua_after;
    return fail_lua ? NULL : realloc(ptr, size);
}
static void run(lua_State *L, const char *script)
{
    int status = luaL_dostring(L, script);
    if (status) fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK); lua_settop(L, 0);
}
#if SOLAR_OS_PACKAGE_SERVICE_JSON
static void *json_alloc(size_t size)
{ return solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.json"); }
static void json_results(lua_State *L)
{
    cJSON_Hooks hooks = {json_alloc, solar_os_memory_free}; cJSON_InitHooks(&hooks);
    const char *payload = "{\"kind\":\"classification\",\"classes\":[{\"id\":281,\"label\":\"tabby\",\"score\":0.5}]}";
    const char *transforms = "{\"pixels\":{\"source_width\":300,\"source_height\":200}}";
    for (int failure = -1; failure < 80; ++failure) {
        lua_gc(L, LUA_GCCOLLECT); int baseline = allocations;
        solar_os_inference_result_t *result = solar_os_memory_calloc(1,sizeof(*result),SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"test.result");
        assert(result);
        result->result_json = solar_os_memory_alloc(strlen(payload)+1,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"test.result_json");
        result->transforms_json = solar_os_memory_alloc(strlen(transforms)+1,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"test.transforms");
        assert(result->result_json && result->transforms_json);
        strcpy(result->result_json,payload); strcpy(result->transforms_json,transforms);
        result->preprocess_us = 12; result->postprocess_us = 1500;
        lua_pushcfunction(L,solua_inference_result); lua_pushlightuserdata(L,result);
        fail_lua_after = failure;
        int status = lua_pcall(L,1,1,0); fail_lua_after = -1;
        if (status == LUA_OK) {
            lua_setglobal(L,"bundle_result");
            run(L,"assert(bundle_result.result.classes[1].label=='tabby'); "
                "assert(bundle_result.result.classes[1].id==281); "
                "assert(bundle_result.transforms.pixels.source_width==300); "
                "assert(bundle_result.postprocess_us==1500 and bundle_result.preprocess_us==12)");
        } else { assert(status==LUA_ERRMEM); lua_settop(L,0); }
        solar_os_inference_result_free(result); assert(allocations==baseline);
    }
}
#endif
int main(void)
{
    assert(native_inference_suite() == 0);
    lua_State *L = lua_newstate(lua_alloc, NULL); assert(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_Reg api[] = {{"load", solua_inference_load}, {"info", solua_inference_info_api},
        {"inputs", solua_inference_inputs}, {"outputs", solua_inference_outputs},
        {"run", solua_inference_run}, {"reset", solua_inference_reset}, {"set_mode", solua_inference_set_mode},
        {"list", solua_inference_list}, {"find", solua_inference_find}, {"close", solua_inference_close}, {"close_all", solua_inference_close_all}, {NULL, NULL}};
    luaL_newlib(L, api); lua_setglobal(L, "infer");
    run(L, "h = infer.load('/model.espdl'); "
        "a = string.pack('bbbbbbbb',1,-2,3,4,-5,6,7,-8); "
        "b = string.pack('bbbbbbbb',2,3,-4,1,6,-2,0,4); "
        "assert(infer.info(h).backend == 'espdl'); assert(infer.inputs(h).a.bytes == 8); "
        "assert(infer.outputs(h).sum.dtype == 'int8'); "
        "assert(infer.info(h).mode == 'single'); infer.set_mode(h,'dual'); "
        "assert(infer.info(h).mode == 'dual'); infer.set_mode(h,'auto'); "
        "assert(infer.info(h).mode == 'auto'); infer.set_mode(h,'single'); "
        "assert(not pcall(infer.set_mode,h,'bad')); "
        "assert(not pcall(infer.set_mode,h,'dual' .. string.char(0) .. 'suffix')); "
        "typed = {data=a,dtype='int8',shape={2,4},bytes=8,exponents={0}}; "
        "r = infer.run(h,{a=typed,b=b}); assert(string.byte(r.outputs.sum.data,1) == 3); "
        "typed.dtype='uint8'; assert(not pcall(infer.run,h,{a=typed,b=b})); typed.dtype='int8'; "
        "typed.dtype='int8' .. string.char(0) .. 'suffix'; assert(not pcall(infer.run,h,{a=typed,b=b})); typed.dtype='int8'; "
        "assert(not pcall(infer.run,h,{['a' .. string.char(0) .. 'suffix']=a,b=b})); "
        "typed.shape={1,8}; assert(not pcall(infer.run,h,{a=typed,b=b})); typed.shape={2,4}; "
        "typed.exponents={1}; assert(not pcall(infer.run,h,{a=typed,b=b})); typed.exponents={0}; "
        "typed.byte=8; assert(not pcall(infer.run,h,{a=typed,b=b})); typed.byte=nil; "
        "assert(not pcall(infer.run,h,{a=a})); assert(not pcall(infer.run,h,{a=a,b='short'})); "
        "assert(not pcall(infer.run,h,{a=a,unknown=b})); "
        "assert(not pcall(infer.load,'/missing')); assert(not pcall(infer.run,h,{a=a,b=b},0)); "
        "infer.reset(h)");
    int baseline = allocations;
    lua_getglobal(L, "infer"); lua_getfield(L, -1, "run"); lua_remove(L, -2);
    lua_getglobal(L, "h"); lua_newtable(L);
    lua_getglobal(L, "a"); lua_setfield(L, -2, "a"); lua_getglobal(L, "b"); lua_setfield(L, -2, "b");
    /* Toggle allocator failure only when native execution returns successfully. */
    atomic_store(&backend_entered, 0); fail_result = true;
    int status = lua_pcall(L, 2, 1, 0);
    fail_result = fail_lua = false;
    assert(status == LUA_ERRMEM && allocations == baseline && !workers);
    lua_settop(L, 0);
    run(L, "retained = infer.run(h,{b=b,a=a}).outputs.sum.data; "
        "old=h; infer.close(h); assert(string.byte(retained,1)==3); "
        "assert(not pcall(infer.run,old,{a=a,b=b})); h=infer.load('/model.espdl'); "
        "assert(h ~= old); infer.close_all(); assert(not pcall(infer.info,h)); "
        "h=infer.load('/model.espdl')");
    assert(solua_inference_active());
    solua_inference_destroy(); assert(!workers && !solua_inference_active());
    run(L, "assert(infer.find('/model.espdl') == h); assert(#infer.list() == 1); "
        "assert(infer.run(h,{a=a,b=b}).outputs.sum.data == retained); infer.close_all()");
    solua_inference_destroy(); assert(!allocations && !workers && !solua_inference_active());
#if SOLAR_OS_PACKAGE_SERVICE_JSON
    json_results(L); assert(!allocations);
#endif
    lua_close(L);
    puts("actual Lua inference metadata, typed buffers, result-OOM, client teardown and resident model ownership passed");
    return 0;
}
