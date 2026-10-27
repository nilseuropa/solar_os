static int solua_jet_scene_postfx(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2)return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_postfx_fields,JET_FX_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_postfx_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX_INFO};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_postfx_fields,JET_FX_COUNT);
    solua_set_int(L,-1,"scratch_bytes",r.values[16]);lua_pushboolean(L,r.values[17]);lua_setfield(L,-2,"history_valid");return 1;
}
static int solua_jet_scene_postfx_reset(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX_RESET};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_postfx_clear(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX_CLEAR};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
