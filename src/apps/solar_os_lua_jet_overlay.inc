static int solua_jet_scene_sprite(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SPRITE};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2)return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_sprite_fields,JET_SPR_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_sprite_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SPRITE_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,3,jet_sprite_fields,JET_SPR_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_sprite_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SPRITE_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_sprite_fields,JET_SPR_COUNT);return 1;
}
static int solua_jet_scene_pick(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);size_t n=0;
    if(!lua_isnoneornil(L,2)) {luaL_checktype(L,2,LUA_TTABLE);n=lua_rawlen(L,2);}
    if(n>SOLAR_OS_JET_MAX_PICKS)return luaL_error(L,"too many Jet pick points");
    solar_os_jet_pick_t *data=lua_newuserdatauv(L,n*sizeof(*data),0);
    for(size_t i=0;i<n;++i) {
        lua_rawgeti(L,2,i+1);luaL_checktype(L,-1,LUA_TTABLE);
        if(lua_rawlen(L,-1)!=2)return luaL_error(L,"expected Jet pick x,y");
        lua_rawgeti(L,-1,1);luaL_checkinteger(L,-1);data[i].x=solua_jet_integer(L,-1,-1,1023,0);lua_pop(L,1);
        lua_rawgeti(L,-1,2);luaL_checkinteger(L,-1);data[i].y=solua_jet_integer(L,-1,-1,1023,0);lua_pop(L,2);
    }
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PICK,.data=data,.bytes=n*sizeof(*data)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_picks(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INFO};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));bool instancing=(r.handle&SOLAR_OS_JET_FEATURE_INSTANCING)!=0;
    q.opcode=SOLAR_OS_JET_PICK_RESULT;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));unsigned count=r.count;lua_newtable(L);
    for(unsigned i=0;i<count;++i) {
        if(i) {q.values[0]=i;SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));}
        lua_newtable(L);lua_pushboolean(L,r.values[0]);lua_setfield(L,-2,"hit");
        solua_set_int(L,-1,"object",r.handle);solua_set_int(L,-1,"triangle",r.values[1]+1);
        solua_set_int(L,-1,"mesh",instancing ? r.values[5] : 0);solua_set_int(L,-1,"instance",instancing ? r.values[6]+1 : 0);
        solua_set_int(L,-1,"batch",instancing ? r.values[7] : 0);
        solua_set_int(L,-1,"depth",r.values[2]);solua_set_int(L,-1,"x",r.values[3]);solua_set_int(L,-1,"y",r.values[4]);
        lua_rawseti(L,-2,i+1);
    }
    return 1;
}
