static int solua_jet_scene_load_obj(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_LOAD_OBJ};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");
    luaL_checktype(L,2,LUA_TSTRING);solua_jet_options(L,3,jet_obj_load_fields,JET_LOAD_COUNT,&q);
    q.data=lua_tolstring(L,2,&q.bytes);SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_load_bmp(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_LOAD_BMP};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");
    luaL_checktype(L,2,LUA_TSTRING);solua_jet_options(L,3,jet_texture_fields,JET_TEX_COUNT,&q);
    q.data=lua_tolstring(L,2,&q.bytes);SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_load_mtl(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_LOAD_MTL};solar_os_jet_reply_t r;
    luaL_checktype(L,2,LUA_TSTRING);q.data=lua_tolstring(L,2,&q.bytes);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_newtable(L);
    for(unsigned i=0;i<r.count;++i) {lua_pushinteger(L,r.handles[i]);lua_rawseti(L,-2,i+1);}return 1;
}
