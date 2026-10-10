#define IMLIB_TEST_ENTRY native_imlib_suite
#include "imlib_test.c"
#undef IMLIB_TEST_ENTRY
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#define SOLAR_OS_PACKAGE_SERVICE_IMLIB 1
#define SOLUA_RASTER_IMAGE_MAX 8
static struct { solar_os_raster_image_t *images[SOLUA_RASTER_IMAGE_MAX]; } solua;
static bool fail_lua_alloc;
static size_t solua_image_slot(lua_State *L, int index)
{
    lua_Integer n = luaL_checkinteger(L,index);
    if (n<1 || n>SOLUA_RASTER_IMAGE_MAX || !solua.images[n-1]) luaL_error(L,"invalid image");
    return n-1;
}
static bool solua_should_cancel(void *user) { (void)user; return false; }
static int solua_check_esp(lua_State *L, esp_err_t error)
{
    if (error) return luaL_error(L,"native error %d",error);
    return 0;
}
static void solua_set_int(lua_State *L,int table,const char *key,lua_Integer value)
{ table=lua_absindex(L,table);lua_pushinteger(L,value);lua_setfield(L,table,key); }
static void solua_set_str(lua_State *L,int table,const char *key,const char *value)
{ table=lua_absindex(L,table);lua_pushstring(L,value);lua_setfield(L,table,key); }
static void solua_set_bool(lua_State *L,int table,const char *key,bool value)
{ table=lua_absindex(L,table);lua_pushboolean(L,value);lua_setfield(L,table,key); }
#include "solar_os_lua_imlib.inc"
static void *test_lua_alloc(void *user,void *p,size_t old,size_t size)
{
    (void)user;(void)old;
    if (!size) { free(p);return NULL; }
    return fail_lua_alloc ? NULL : realloc(p,size);
}
static void run_lua(lua_State *L,const char *script)
{
    int status=luaL_dostring(L,script);
    if (status) fprintf(stderr,"Lua: %s\n",lua_tostring(L,-1));
    assert(status==LUA_OK);lua_settop(L,0);
}
int main(void)
{
    assert(native_imlib_suite()==0);
    uint8_t gray[16*12];memset(gray,20,sizeof(gray));
    for(unsigned y=3;y<8;++y)for(unsigned x=4;x<10;++x)gray[y*16+x]=200;
    solua.images[0]=gray_image(gray,16,12);int baseline=allocations;
    lua_State *L=lua_newstate(test_lua_alloc,NULL);assert(L);
    luaL_requiref(L,"_G",luaopen_base,1);lua_pop(L,1);
    luaL_requiref(L,LUA_MATHLIBNAME,luaopen_math,1);lua_pop(L,1);
    luaL_requiref(L,LUA_STRLIBNAME,luaopen_string,1);lua_pop(L,1);
#define REGISTER(name) lua_pushcfunction(L,solua_vision_##name);lua_setglobal(L,#name)
    REGISTER(process);REGISTER(statistics);REGISTER(histogram);REGISTER(binary);REGISTER(invert);
    REGISTER(mean);REGISTER(gaussian);REGISTER(median);REGISTER(erode);REGISTER(dilate);
    REGISTER(opening);REGISTER(closing);REGISTER(difference);REGISTER(blobs);
#undef REGISTER
    run_lua(L,"local s=statistics(1,{x=4,y=3,width=6,height=5}); assert(s.channels.gray.mean==200); "
        "local h=histogram(1); assert(#h.channels.gray==256); "
        "local b=blobs(1,{thresholds={{128,255}}}); assert(#b.blobs==1 and b.blobs[1].pixels==30); "
        "assert(b.blobs[1].x==4 and b.blobs[1].y==3 and b.coordinates=='source_pixels'); "
        "mask=binary(1,{thresholds={{128,255}}}); assert(mask==2); "
        "local r=process(mask,'opening'); assert(r.image==3 and r.workspace_peak_bytes>0); "
        "assert(r.elapsed_us>=r.process_us); "
        "diff=difference(1,1); assert(diff==4); "
        "assert(not pcall(process,1,'bogus')); assert(not pcall(statistics,99)); "
        "assert(not pcall(statistics,1,{bins=1})); assert(not pcall(mean,1,{ksize=4})); "
        "assert(not pcall(statistics,1,{width=-1})); assert(not pcall(binary,1)); "
        "assert(not pcall(binary,1,{thresholds={{0,300}}})); "
        "assert(not pcall(blobs,1,{thresholds={{0,255}},merge=1})); "
        "assert(not pcall(statistics,1,{[2]=99})); "
        "assert(not pcall(statistics,1,{output_widht=99})); "
        "assert(not pcall(statistics,1,{['bins'..string.char(0)]=16})); "
        "assert(not pcall(mean,1,{thresholds={{0,255}}})); "
        "assert(not pcall(process,1,'mean',{},1)); "
        "assert(not pcall(mean,1,{},1))");
    for(unsigned i=1;i<SOLUA_RASTER_IMAGE_MAX;++i){solar_os_raster_image_release(solua.images[i]);solua.images[i]=NULL;}
    assert(allocations==baseline);
    for(unsigned i=0;i<12;++i) {
        lua_pushcfunction(L,solua_vision_process);lua_pushinteger(L,1);lua_pushliteral(L,"gaussian");
        int before_attempts=attempts;
        fail_lua_alloc=true;
        int status=lua_pcall(L,2,1,0);fail_lua_alloc=false;
        assert(status!=LUA_OK);lua_settop(L,0);
        assert(attempts>before_attempts && allocations==baseline && !workers && !solua.images[1]);
        assert(!solua_imlib_active());
    }
    lua_close(L);solar_os_raster_image_release(solua.images[0]);assert(!allocations);
    puts("imlib Lua bindings, strict options, shared images, and result-allocation unwind: OK");
    return 0;
}
