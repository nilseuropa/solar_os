static mp_obj_t solaros_jet_scene_instance(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE};solar_os_jet_reply_t r;
    q.values[16]=python_u32_from_obj(args[1]);
    python_jet_options(count-2,args+2,kwargs,jet_instance_fields,JET_INST_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_instance_obj,2,solaros_jet_scene_instance);
static mp_obj_t solaros_jet_scene_instance_add(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_ADD,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    q.values[16]=python_u32_from_obj(args[2]);
    python_jet_options(count-3,args+3,kwargs,jet_instance_fields,JET_INST_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.count);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_instance_add_obj,3,solaros_jet_scene_instance_add);
static mp_obj_t solaros_jet_scene_instance_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    q.values[22]=python_jet_integer(args[2],0,SOLAR_OS_JET_MAX_INSTANCES-1,0);
    python_jet_options(count-3,args+3,kwargs,jet_instance_fields,JET_INST_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_instance_set_obj,3,solaros_jet_scene_instance_set);
static mp_obj_t solaros_jet_scene_prototype(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PROTOTYPE,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_prototype_fields,1,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_prototype_obj,2,solaros_jet_scene_prototype);
static mp_obj_t solaros_jet_scene_prototype_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PROTOTYPE_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));mp_obj_t info=mp_obj_new_dict(0);
    python_dict_store_uint(info,"id",r.handle);python_dict_store_uint(info,"vertices",r.count);python_dict_store_uint(info,"triangles",r.handles[0]);
    mp_obj_dict_store(info,mp_obj_new_str("name",4),mp_obj_new_str(r.name,strlen(r.name)));
    mp_obj_t bounds[6];for(unsigned i=0;i<6;++i)bounds[i]=mp_obj_new_int((int32_t)r.handles[i+1]);
    mp_obj_dict_store(info,MP_OBJ_NEW_QSTR(MP_QSTR_bounds),mp_obj_new_tuple(6,bounds));return info;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_prototype_info_obj,solaros_jet_scene_prototype_info);
static mp_obj_t solaros_jet_scene_instance_info(mp_obj_t self,mp_obj_t handle,mp_obj_t index) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    q.values[22]=python_jet_integer(index,0,SOLAR_OS_JET_MAX_INSTANCES-1,0);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));
    mp_obj_t info=python_jet_info_dict(&r,jet_instance_fields,JET_INST_COUNT),materials=mp_obj_new_list(0,NULL);
    python_dict_store_uint(info,"index",q.values[22]);python_dict_store_uint(info,"prototype",r.values[8]);
    python_dict_store_bool(info,"baked",r.values[10]);
    mp_obj_t basis[9];for(unsigned i=0;i<9;++i)basis[i]=mp_obj_new_float(r.values[11+i]);
    mp_obj_dict_store(info,mp_obj_new_str("basis",5),mp_obj_new_tuple(9,basis));
    unsigned count=r.values[9];
    for(unsigned i=0;i<count;++i) {
        if(i&&i%32==0) {q.values[23]=i/32;PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));}
        mp_obj_list_append(materials,mp_obj_new_int_from_uint(r.handles[i%32]));
    }
    mp_obj_dict_store(info,mp_obj_new_str("materials",9),materials);return info;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_jet_scene_instance_info_obj,solaros_jet_scene_instance_info);
static mp_obj_t solaros_jet_scene_instance_remove(mp_obj_t self,mp_obj_t handle,mp_obj_t index) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_REMOVE,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    q.values[22]=python_jet_integer(index,0,SOLAR_OS_JET_MAX_INSTANCES-1,0);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_jet_scene_instance_remove_obj,solaros_jet_scene_instance_remove);
static mp_obj_t solaros_jet_scene_instance_clear(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INSTANCE_CLEAR,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_instance_clear_obj,solaros_jet_scene_instance_clear);
