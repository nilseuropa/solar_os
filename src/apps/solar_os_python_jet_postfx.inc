static mp_obj_t solaros_jet_scene_postfx(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_postfx_fields,JET_FX_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_postfx_obj,1,solaros_jet_scene_postfx);
static mp_obj_t solaros_jet_scene_postfx_info(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX_INFO};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));mp_obj_t info=python_jet_info_dict(&r,jet_postfx_fields,JET_FX_COUNT);
    python_dict_store_uint(info,"scratch_bytes",r.values[16]);python_dict_store_bool(info,"history_valid",r.values[17]);return info;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_postfx_info_obj,solaros_jet_scene_postfx_info);
static mp_obj_t solaros_jet_scene_postfx_reset(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX_RESET};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_postfx_reset_obj,solaros_jet_scene_postfx_reset);
static mp_obj_t solaros_jet_scene_postfx_clear(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POSTFX_CLEAR};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_postfx_clear_obj,solaros_jet_scene_postfx_clear);
