static void python_jet_asset_data(mp_obj_t data,solar_os_jet_command_t *q) {
    if(mp_obj_is_str(data))q->data=mp_obj_str_get_data(data,&q->bytes);
    else {mp_buffer_info_t buffer;mp_get_buffer_raise(data,&buffer,MP_BUFFER_READ);q->data=buffer.buf;q->bytes=buffer.len;}
}
static mp_obj_t solaros_jet_scene_load_obj(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_LOAD_OBJ};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_obj_load_fields,JET_LOAD_COUNT,&q);python_jet_asset_data(args[1],&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_load_obj_obj,2,solaros_jet_scene_load_obj);
static mp_obj_t solaros_jet_scene_load_bmp(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_LOAD_BMP};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_texture_fields,JET_TEX_COUNT,&q);python_jet_asset_data(args[1],&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_load_bmp_obj,2,solaros_jet_scene_load_bmp);
static mp_obj_t solaros_jet_scene_load_mtl(mp_obj_t self,mp_obj_t data) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_LOAD_MTL};solar_os_jet_reply_t r;
    python_jet_asset_data(data,&q);PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));
    mp_obj_t list=mp_obj_new_list(0,NULL);for(unsigned i=0;i<r.count;++i)mp_obj_list_append(list,mp_obj_new_int_from_uint(r.handles[i]));return list;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_load_mtl_obj,solaros_jet_scene_load_mtl);
