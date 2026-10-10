/* Compile the actual binding and its core against Lua; no replacement adapter. */
#define main native_image_suite
#include "tensor_image_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
static bool image_valid = true;
static struct { solar_os_raster_image_t *images[1]; } solua;
static solar_os_inference_model_info_t model;
static const uint8_t pixels[] = {255,0,0,0,255,0,0,0,255,10,32,13,124,116,104,255,255,255};
esp_err_t solar_os_raster_image_pixels(const solar_os_raster_image_t *image, solar_os_raster_image_pixels_t *out)
{
    assert(image == solua.images[0]); *out = (solar_os_raster_image_pixels_t){pixels,sizeof(pixels),9,3,2};
    return ESP_OK;
}
static int solua_check_esp(lua_State *L, esp_err_t error)
{ return error == ESP_OK ? 0 : luaL_error(L,"native error %d",error); }
static bool solua_should_cancel(void *user) { (void)user; return false; }
static size_t solua_image_slot(lua_State *L, int index)
{ if (!image_valid || luaL_checkinteger(L,index) != 1) luaL_error(L,"invalid image"); return 0; }
static const solar_os_inference_model_info_t *solua_inference_info(lua_State *L)
{ if (luaL_checkinteger(L,1) != 7) luaL_error(L,"invalid model"); return &model; }
static void solua_inference_field(lua_State *L, int index, const char *key)
{ lua_pushstring(L,key); lua_rawget(L,index); }
static const char *solua_inference_name(lua_State *L, int index)
{
    size_t n; const char *s = luaL_checklstring(L,index,&n);
    if (memchr(s,0,n)) luaL_error(L,"embedded NUL");
    return s;
}
static uint32_t solua_inference_uint(lua_State *L, int index)
{
    lua_Integer n = luaL_checkinteger(L,index);
    if (n < 0 || (uint64_t)n > UINT32_MAX) luaL_error(L,"integer out of range");
    return n;
}
static void solua_set_int(lua_State *L, int index, const char *key, lua_Integer n)
{ index = lua_absindex(L,index); lua_pushinteger(L,n); lua_setfield(L,index,key); }
#define SOLAR_OS_PACKAGE_SERVICE_IMAGE 1
#include "solar_os_lua_tensor_image.inc"
static void run(lua_State *L, const char *source)
{
    int status = luaL_dostring(L,source);
    if (status != LUA_OK) fprintf(stderr,"Lua: %s\n",lua_tostring(L,-1));
    assert(status == LUA_OK); lua_settop(L,0); assert(!allocations);
}
int main(void)
{
    assert(native_image_suite() == 0);
    int32_t exponent=0;
    model.input_count=1;
    model.inputs[0]=(solar_os_inference_tensor_t){.name="pixels",.rank=4,.shape={1,2,3,3},
        .dtype=SOLAR_OS_TENSOR_INT8,.bytes=18,.exponent_count=1,.exponents=&exponent};
    solua.images[0]=(solar_os_raster_image_t *)1;
    lua_State *L=luaL_newstate(); assert(L);
    luaL_requiref(L,"_G",luaopen_base,1); lua_pop(L,1);
    luaL_requiref(L,LUA_STRLIBNAME,luaopen_string,1); lua_pop(L,1);
    lua_pushcfunction(L,solua_inference_prepare_image); lua_setglobal(L,"prepare");
    run(L,"retained=prepare(7,'pixels',1); assert(#retained.data==18); "
        "assert(retained.transform.source_width==3 and retained.transform.resized_height==2); "
        "assert(string.byte(retained.data,1)==127); "
        "assert(not pcall(prepare,99,'pixels',1)); assert(not pcall(prepare,7,'missing',1)); "
        "assert(not pcall(prepare,7,'pixels',99)); assert(not pcall(prepare,7,'pixels',1,'bad')); "
        "assert(not pcall(prepare,7,'pixels',1,{mean={}})); "
        "assert(not pcall(prepare,7,'pixels',1,{mean={1,2}})); "
        "assert(not pcall(prepare,7,'pixels',1,{mean={true}})); "
        "assert(not pcall(prepare,7,'pixels',1,{std={0}})); "
        "assert(not pcall(prepare,7,'pixels',1,{std={0/0}})); "
        "assert(not pcall(prepare,7,'pixels',1,{pad={1.5}})); "
        "assert(not pcall(prepare,7,'pixels',1,{pad={256}})); "
        "assert(not pcall(prepare,7,'pixels',1,{x=-1})); "
        "assert(not pcall(prepare,7,'pixels',1,{extra=1})); "
        "assert(not pcall(prepare,7,'pixels',1,{color='BGR'..string.char(0)})); "
        "local r=prepare(7,'pixels',1,{resize='letterbox',pad={4},width=1,height=1}); "
        "assert(r.transform.resized_width==2 and r.transform.pad_left==0)");
    fail_alloc=true; run(L,"assert(not pcall(prepare,7,'pixels',1))"); fail_alloc=false;
    run(L,"assert(#prepare(7,'pixels',1,{mean={128},std={1}}).data==18)");
    image_valid=false;
    run(L,"assert(#retained.data==18); assert(not pcall(prepare,7,'pixels',1))");
    lua_close(L); assert(!allocations);
    puts("actual Lua image tensor validation, result ownership and allocation retry passed");
}
