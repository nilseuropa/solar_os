/* Shared command schemas keep Python and Lua option contracts identical. */
static int solua_jet_scene_primitive(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PRIMITIVE};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_primitive_fields,JET_PRIM_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_object_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_OBJECT_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,3,jet_object_fields,JET_OBJ_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_object_action(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_OBJECT_ACTION,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,3,jet_object_action_fields,JET_OACT_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_camera_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_CAMERA_SET};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_camera_fields,JET_CAM_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_camera_action(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_CAMERA_ACTION};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_camera_action_fields,JET_CACT_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_point_light(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POINT_LIGHT};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_point_fields,JET_POINT_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_point_light_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POINT_LIGHT_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,3,jet_point_fields,JET_POINT_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_settings(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SCENE_SET};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_scene_fields,JET_SCENE_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_object_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_OBJECT_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_object_fields,JET_OBJ_COUNT);
    solua_set_int(L,-1,"vertices",r.count);solua_set_int(L,-1,"triangles",r.handles[0]);
    solua_set_int(L,-1,"lod_levels",r.handles[7]);
    solua_set_int(L,-1,"instances",r.handles[8]);
    lua_newtable(L);for(unsigned i=0;i<6;++i) {lua_pushinteger(L,(int32_t)r.handles[i+1]);lua_rawseti(L,-2,i+1);}
    lua_setfield(L,-2,"bounds");
    return 1;
}
static int solua_jet_scene_camera_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_CAMERA_INFO};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_camera_fields,JET_CAM_COUNT);
    return 1;
}
static int solua_jet_scene_point_light_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POINT_LIGHT_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_point_fields,JET_POINT_COUNT);
    return 1;
}
static int solua_jet_scene_settings_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SCENE_INFO};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_scene_fields,JET_SCENE_COUNT);
    return 1;
}
static int solua_jet_list_command(lua_State *L,bool lod) {
    solar_os_jet_t *scene=solua_jet_scene(L);uint32_t object=lod ? solua_jet_id(L,2) : 0;
    int index=lod ? 3 : 2;size_t n=0;
    if(!lua_isnoneornil(L,index)) {luaL_checktype(L,index,LUA_TTABLE);n=lua_rawlen(L,index);}
    if(n>(lod ? 8U : 1024U)) return luaL_error(L,"too many Jet entries");
    uint32_t *data=lua_newuserdatauv(L,n*sizeof(*data),0);
    for(size_t i=0;i<n;++i) {
        lua_rawgeti(L,index,i+1);luaL_checkinteger(L,-1);
        data[i]=lod ? solua_jet_id(L,-1) : (uint32_t)solua_jet_integer(L,-1,0,0xffffff,0);lua_pop(L,1);
    }
    solar_os_jet_command_t q={.opcode=lod ? SOLAR_OS_JET_LOD : SOLAR_OS_JET_GRADIENT,
        .handle=object,.data=data,.bytes=n*sizeof(*data)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_gradient(lua_State *L) {return solua_jet_list_command(L,false);}
static int solua_jet_scene_lod(lua_State *L) {return solua_jet_list_command(L,true);}
static int solua_jet_scene_mesh_data(lua_State *L) {return solua_jet_mesh_data(L,0);}
static int solua_jet_scene_mesh_update(lua_State *L) {
    uint32_t handle=solua_jet_id(L,2);lua_remove(L,2);return solua_jet_mesh_data(L,handle);
}
