#include "script_hid_abi_fixture.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
static void solua_set_bool(lua_State *L,int table,const char *key,bool value)
{ table=lua_absindex(L,table);lua_pushboolean(L,value);lua_setfield(L,table,key); }
#include "../../src/apps/solar_os_lua_hid.inc"
static lua_State *new_vm(void)
{
    lua_State *L=luaL_newstate();assert(L);luaL_openlibs(L);
    const luaL_Reg root[]={{"status",solua_hid_status},{NULL,NULL}};
    const luaL_Reg keyboard[]={{"press",solua_hid_keyboard_press},{"release",solua_hid_keyboard_release},
        {"release_all",solua_hid_keyboard_release_all},{NULL,NULL}};
    const luaL_Reg mouse[]={{"move",solua_hid_mouse_move},{"button",solua_hid_mouse_button},{NULL,NULL}};
    const luaL_Reg gamepad[]={{"axis",solua_hid_gamepad_axis},{"button",solua_hid_gamepad_button},
        {"hat",solua_hid_gamepad_hat},{"send",solua_hid_gamepad_send},{NULL,NULL}};
    lua_newtable(L);luaL_setfuncs(L,root,0);
    lua_newtable(L);luaL_setfuncs(L,keyboard,0);lua_setfield(L,-2,"keyboard");
    lua_newtable(L);luaL_setfuncs(L,mouse,0);lua_setfield(L,-2,"mouse");
    lua_newtable(L);luaL_setfuncs(L,gamepad,0);lua_setfield(L,-2,"gamepad");
    lua_setglobal(L,"hid");return L;
}
static void run(lua_State *L,const char *source)
{
    if(luaL_dostring(L,source)!=LUA_OK){fprintf(stderr,"Lua HID: %s\n",lua_tostring(L,-1));assert(false);}
}
int main(void)
{
    script_hid_begin();lua_State *L=new_vm();
    run(L,"local s=hid.status(); assert(s.initialized and s.connected); "
          "hid.keyboard.press(257,4); assert(not pcall(hid.keyboard.press,65536)); "
          "assert(not pcall(hid.mouse.move,4294967296,0)); "
          "assert(not pcall(hid.gamepad.axis,4294967296,0)); "
          "assert(not pcall(hid.gamepad.hat,4294967296)); "
          "assert(not pcall(hid.gamepad.hat,-1)); "
          "assert(not pcall(hid.gamepad.button,256,true))");
    assert(hid_api_lookups==1 && hid_send_keyboard() && sent_keyboard.modifier==1 && sent_keyboard.keycode[0]==4);
    run(L,"hid.mouse.move(17,-18); hid.mouse.button(1,true); "
          "hid.gamepad.axis(5,32767); hid.gamepad.button(32,true); hid.gamepad.hat(8); hid.gamepad.send()");
    assert(hid_send_mouse() && sent_mouse.x==17 && sent_mouse.y==-18 && sent_mouse.buttons==1);
    assert(hid_send_gamepad() && sent_gamepad.ry==127 && sent_gamepad.buttons==(UINT32_C(1)<<31) && sent_gamepad.hat==8);
    run(L,"hid.keyboard.release(257,4); hid.keyboard.press(5)");
    lua_close(L);solua_hid_cleanup();script_hid_check_queued_cleanup();
    L=new_vm();hid_api_available=false;
    run(L,"local ok,err=pcall(hid.status); assert(not ok and string.find(err,'unavailable'))");
    lua_close(L);solua_hid_cleanup();
    hid_api_available=true;L=new_vm();run(L,"assert(hid.status().connected)");
    const unsigned cached=hid_api_lookups;hid_api_available=false;
    run(L,"hid.keyboard.release_all(); assert(hid.status().initialized)");
    assert(hid_api_lookups==cached);lua_close(L);solua_hid_cleanup();
    hid_test_shutdown();
    puts("Lua HID: real VM/service, ABI controls, wide integer bounds, cached lookup and post-VM neutral cleanup passed");
    return 0;
}
