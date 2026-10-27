static int solua_jet_scene_instance(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");
    q.values[16]=solua_jet_id(L,2);
    solua_jet_options(L,3,jet_instance_fields,JET_INST_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_instance_add(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_ADD,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>4)return luaL_error(L,"too many Jet arguments");
    q.values[16]=solua_jet_id(L,3);
    solua_jet_options(L,4,jet_instance_fields,JET_INST_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.count+1);return 1;
}
static int solua_jet_scene_instance_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>4)return luaL_error(L,"too many Jet arguments");
    luaL_checkinteger(L,3);q.values[22]=solua_jet_integer(L,3,1,SOLAR_OS_JET_MAX_INSTANCES,1)-1;
    solua_jet_options(L,4,jet_instance_fields,JET_INST_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_prototype(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PROTOTYPE,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,3,jet_prototype_fields,1,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_prototype_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PROTOTYPE_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_newtable(L);
    solua_set_int(L,-1,"id",r.handle);solua_set_int(L,-1,"vertices",r.count);solua_set_int(L,-1,"triangles",r.handles[0]);
    lua_pushstring(L,r.name);lua_setfield(L,-2,"name");lua_newtable(L);
    for(unsigned i=0;i<6;++i) {lua_pushinteger(L,(int32_t)r.handles[i+1]);lua_rawseti(L,-2,i+1);}
    lua_setfield(L,-2,"bounds");return 1;
}
static int solua_jet_scene_instance_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    luaL_checkinteger(L,3);q.values[22]=solua_jet_integer(L,3,1,SOLAR_OS_JET_MAX_INSTANCES,1)-1;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_instance_fields,JET_INST_COUNT);
    solua_set_int(L,-1,"index",q.values[22]+1);solua_set_int(L,-1,"prototype",r.values[8]);
    lua_pushboolean(L,r.values[10]);lua_setfield(L,-2,"baked");lua_newtable(L);
    for(unsigned i=0;i<9;++i) {lua_pushnumber(L,r.values[11+i]);lua_rawseti(L,-2,i+1);}lua_setfield(L,-2,"basis");
    unsigned count=r.values[9];lua_newtable(L);
    for(unsigned i=0;i<count;++i) {
        if(i&&i%32==0) {q.values[23]=i/32;SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));}
        lua_pushinteger(L,r.handles[i%32]);lua_rawseti(L,-2,i+1);
    }
    lua_setfield(L,-2,"materials");return 1;
}
static int solua_jet_scene_instance_remove(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_REMOVE,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    luaL_checkinteger(L,3);q.values[22]=solua_jet_integer(L,3,1,SOLAR_OS_JET_MAX_INSTANCES,1)-1;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_instance_clear(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_CLEAR,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
