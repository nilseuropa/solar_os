static int solua_jet_scene_particles(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2)return luaL_error(L,"too many Jet arguments");

    solua_jet_options(L,2,jet_particle_fields,JET_PART_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_particles_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");

    solua_jet_options(L,3,jet_particle_fields,JET_PART_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_particles_emit(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_EMIT,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");

    solua_jet_options(L,3,jet_emit_fields,JET_EMIT_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.count);return 1;
}
static int solua_jet_scene_flare_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");

    solua_jet_options(L,3,jet_flare_fields,JET_FLARE_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_flare_element_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_ELEMENT_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>4)return luaL_error(L,"too many Jet arguments");
    luaL_checkinteger(L,3);q.values[22]=solua_jet_integer(L,3,1,SOLAR_OS_JET_MAX_FLARE_ELEMENTS,1)-1;
    solua_jet_options(L,4,jet_element_fields,JET_ELEM_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_flare(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3)return luaL_error(L,"too many Jet arguments");
    luaL_checktype(L,2,LUA_TTABLE);size_t n=lua_rawlen(L,2);
    if(!n||n>SOLAR_OS_JET_MAX_FLARE_ELEMENTS)return luaL_error(L,"invalid Jet flare elements");
    solua_jet_options(L,3,jet_flare_fields,JET_FLARE_COUNT,&q);
    solar_os_jet_element_t *elements=lua_newuserdatauv(L,n*sizeof(*elements),0);
    for(size_t i=0;i<n;++i) {
        lua_rawgeti(L,2,i+1);solar_os_jet_command_t element={};
        luaL_checktype(L,-1,LUA_TTABLE);solua_jet_options(L,lua_gettop(L),jet_element_fields,JET_ELEM_COUNT,&element);
        elements[i].fields=element.fields;memcpy(elements[i].values,element.values,sizeof(element.values));lua_pop(L,1);
    }
    q.data=elements;q.bytes=n*sizeof(*elements);SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_step(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_STEP};solar_os_jet_reply_t r;q.values[0]=luaL_checknumber(L,2);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_particles_clear(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_CLEAR,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_particles_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;

    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_particle_fields,JET_PART_COUNT);
    solua_set_int(L,-1,"active",r.values[16]);
    solua_set_int(L,-1,"triangles",r.values[17]);
    solua_set_int(L,-1,"capacity",r.values[18]);
    solua_set_int(L,-1,"random_state",r.values[19]);
    return 1;
}
static int solua_jet_scene_flare_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;

    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_flare_fields,JET_FLARE_COUNT);
    lua_pushnumber(L,r.values[16]);lua_setfield(L,-2,"visibility");
    lua_pushboolean(L,r.values[17]);lua_setfield(L,-2,"in_front");
    lua_pushboolean(L,r.values[18]);lua_setfield(L,-2,"on_screen");
    solua_set_int(L,-1,"projected_x",r.values[19]);
    solua_set_int(L,-1,"projected_y",r.values[20]);
    solua_set_int(L,-1,"elements",r.values[21]);
    return 1;
}
static int solua_jet_scene_flare_element_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_ELEMENT_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    luaL_checkinteger(L,3);q.values[22]=solua_jet_integer(L,3,1,SOLAR_OS_JET_MAX_FLARE_ELEMENTS,1)-1;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_element_fields,JET_ELEM_COUNT);
    return 1;
}
