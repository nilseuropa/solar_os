static mp_obj_t solaros_jet_scene_particles(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES};solar_os_jet_reply_t r;

    python_jet_options(count-1,args+1,kwargs,jet_particle_fields,JET_PART_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_particles_obj,1,solaros_jet_scene_particles);
static mp_obj_t solaros_jet_scene_particles_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;

    python_jet_options(count-2,args+2,kwargs,jet_particle_fields,JET_PART_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_particles_set_obj,2,solaros_jet_scene_particles_set);
static mp_obj_t solaros_jet_scene_particles_emit(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_EMIT,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;

    python_jet_options(count-2,args+2,kwargs,jet_emit_fields,JET_EMIT_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.count);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_particles_emit_obj,2,solaros_jet_scene_particles_emit);
static mp_obj_t solaros_jet_scene_flare_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;

    python_jet_options(count-2,args+2,kwargs,jet_flare_fields,JET_FLARE_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_flare_set_obj,2,solaros_jet_scene_flare_set);
static mp_obj_t solaros_jet_scene_flare_element_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_ELEMENT_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    q.values[22]=python_jet_integer(args[2],0,SOLAR_OS_JET_MAX_FLARE_ELEMENTS-1,0);
    python_jet_options(count-3,args+3,kwargs,jet_element_fields,JET_ELEM_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_flare_element_set_obj,3,solaros_jet_scene_flare_element_set);
static mp_obj_t solaros_jet_scene_flare(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_flare_fields,JET_FLARE_COUNT,&q);
    size_t n;mp_obj_t *items;mp_obj_get_array(args[1],&n,&items);
    if(!n||n>SOLAR_OS_JET_MAX_FLARE_ELEMENTS)mp_raise_ValueError(MP_ERROR_TEXT("invalid Jet flare elements"));
    solar_os_jet_element_t *elements=m_new(solar_os_jet_element_t,n);
    for(size_t i=0;i<n;++i) {
        if(!mp_obj_is_type(items[i],&mp_type_dict))mp_raise_TypeError(MP_ERROR_TEXT("expected Jet element options dictionary"));
        solar_os_jet_command_t element={};python_jet_options(0,NULL,mp_obj_dict_get_map(items[i]),jet_element_fields,JET_ELEM_COUNT,&element);
        elements[i].fields=element.fields;memcpy(elements[i].values,element.values,sizeof(element.values));
    }
    q.data=elements;q.bytes=n*sizeof(*elements);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));m_del(solar_os_jet_element_t,elements,n);return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_flare_obj,2,solaros_jet_scene_flare);
static mp_obj_t solaros_jet_scene_step(mp_obj_t self,mp_obj_t dt) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_STEP};solar_os_jet_reply_t r;q.values[0]=mp_obj_get_float(dt);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_step_obj,solaros_jet_scene_step);
static mp_obj_t solaros_jet_scene_particles_clear(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_CLEAR,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_particles_clear_obj,solaros_jet_scene_particles_clear);
static mp_obj_t solaros_jet_scene_particles_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PARTICLES_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;

    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));mp_obj_t info=python_jet_info_dict(&r,jet_particle_fields,JET_PART_COUNT);
    python_dict_store_uint(info,"active",r.values[16]);
    python_dict_store_uint(info,"triangles",r.values[17]);
    python_dict_store_uint(info,"capacity",r.values[18]);
    python_dict_store_uint(info,"random_state",r.values[19]);
    return info;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_particles_info_obj,solaros_jet_scene_particles_info);
static mp_obj_t solaros_jet_scene_flare_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;

    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));mp_obj_t info=python_jet_info_dict(&r,jet_flare_fields,JET_FLARE_COUNT);
    mp_obj_dict_store(info,mp_obj_new_str("visibility",10),mp_obj_new_float(r.values[16]));
    python_dict_store_bool(info,"in_front",r.values[17]);
    python_dict_store_bool(info,"on_screen",r.values[18]);
    mp_obj_dict_store(info,mp_obj_new_str("projected_x",11),mp_obj_new_int(r.values[19]));
    mp_obj_dict_store(info,mp_obj_new_str("projected_y",11),mp_obj_new_int(r.values[20]));
    python_dict_store_uint(info,"elements",r.values[21]);
    return info;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_flare_info_obj,solaros_jet_scene_flare_info);
static mp_obj_t solaros_jet_scene_flare_element_info(mp_obj_t self,mp_obj_t handle,mp_obj_t index) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_FLARE_ELEMENT_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    q.values[22]=python_jet_integer(index,0,SOLAR_OS_JET_MAX_FLARE_ELEMENTS-1,0);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));mp_obj_t info=python_jet_info_dict(&r,jet_element_fields,JET_ELEM_COUNT);
    return info;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_jet_scene_flare_element_info_obj,solaros_jet_scene_flare_element_info);
