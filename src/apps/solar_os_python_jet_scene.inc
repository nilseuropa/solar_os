/* Shared command schemas keep Python and Lua option contracts identical. */
static mp_obj_t solaros_jet_scene_primitive(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PRIMITIVE};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_primitive_fields,JET_PRIM_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_primitive_obj,1,solaros_jet_scene_primitive);
static mp_obj_t solaros_jet_scene_object_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_OBJECT_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_object_fields,JET_OBJ_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_object_set_obj,2,solaros_jet_scene_object_set);
static mp_obj_t solaros_jet_scene_object_action(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_OBJECT_ACTION,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_object_action_fields,JET_OACT_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_object_action_obj,2,solaros_jet_scene_object_action);
static mp_obj_t solaros_jet_scene_camera_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_CAMERA_SET};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_camera_fields,JET_CAM_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_camera_set_obj,1,solaros_jet_scene_camera_set);
static mp_obj_t solaros_jet_scene_camera_action(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_CAMERA_ACTION};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_camera_action_fields,JET_CACT_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_camera_action_obj,1,solaros_jet_scene_camera_action);
static mp_obj_t solaros_jet_scene_point_light(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POINT_LIGHT};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_point_fields,JET_POINT_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_point_light_obj,1,solaros_jet_scene_point_light);
static mp_obj_t solaros_jet_scene_point_light_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POINT_LIGHT_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_point_fields,JET_POINT_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_point_light_set_obj,2,solaros_jet_scene_point_light_set);
static mp_obj_t solaros_jet_scene_settings(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SCENE_SET};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_scene_fields,JET_SCENE_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_settings_obj,1,solaros_jet_scene_settings);
static mp_obj_t solaros_jet_scene_object_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_OBJECT_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));
    mp_obj_t result=python_jet_info_dict(&r,jet_object_fields,JET_OBJ_COUNT);
    python_dict_store_uint(result,"vertices",r.count);python_dict_store_uint(result,"triangles",r.handles[0]);
    python_dict_store_uint(result,"lod_levels",r.handles[7]);
    python_dict_store_uint(result,"instances",r.handles[8]);
    mp_obj_t bounds[6];for(unsigned i=0;i<6;++i) bounds[i]=mp_obj_new_int((int32_t)r.handles[i+1]);
    mp_obj_dict_store(result,MP_OBJ_NEW_QSTR(MP_QSTR_bounds),mp_obj_new_tuple(6,bounds));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_object_info_obj,solaros_jet_scene_object_info);
static mp_obj_t solaros_jet_scene_camera_info(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_CAMERA_INFO};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));
    mp_obj_t result=python_jet_info_dict(&r,jet_camera_fields,JET_CAM_COUNT);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_camera_info_obj,solaros_jet_scene_camera_info);
static mp_obj_t solaros_jet_scene_point_light_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_POINT_LIGHT_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));
    mp_obj_t result=python_jet_info_dict(&r,jet_point_fields,JET_POINT_COUNT);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_point_light_info_obj,solaros_jet_scene_point_light_info);
static mp_obj_t solaros_jet_scene_settings_info(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SCENE_INFO};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));
    mp_obj_t result=python_jet_info_dict(&r,jet_scene_fields,JET_SCENE_COUNT);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_settings_info_obj,solaros_jet_scene_settings_info);
static mp_obj_t python_jet_list_command(mp_obj_t self,mp_obj_t object,mp_obj_t list,bool lod) {
    solar_os_jet_t *scene=python_jet_scene(self);size_t n=0;mp_obj_t *values=NULL;
    if(list!=mp_const_none) mp_obj_get_array(list,&n,&values);
    if(n>(lod ? 8U : 1024U)) mp_raise_ValueError(MP_ERROR_TEXT("too many Jet entries"));
    uint32_t *data=m_new(uint32_t,n);
    for(size_t i=0;i<n;++i) data[i]=lod ? python_u32_from_obj(values[i]) : python_jet_color(values[i]);
    solar_os_jet_command_t q={.opcode=lod ? SOLAR_OS_JET_LOD : SOLAR_OS_JET_GRADIENT,
        .handle=lod ? python_u32_from_obj(object) : 0,.data=data,.bytes=n*sizeof(*data)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));m_del(uint32_t,data,n);return mp_const_none;
}
static mp_obj_t solaros_jet_scene_gradient(mp_obj_t self,mp_obj_t colors) {
    return python_jet_list_command(self,mp_const_none,colors,false);
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_gradient_obj,solaros_jet_scene_gradient);
static mp_obj_t solaros_jet_scene_lod(mp_obj_t self,mp_obj_t object,mp_obj_t levels) {
    return python_jet_list_command(self,object,levels,true);
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_jet_scene_lod_obj,solaros_jet_scene_lod);
static mp_obj_t solaros_jet_scene_mesh_data(size_t count,const mp_obj_t *args) {
    return python_jet_mesh_data(count,args,0);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_jet_scene_mesh_data_obj,3,4,solaros_jet_scene_mesh_data);
static mp_obj_t solaros_jet_scene_mesh_update(size_t count,const mp_obj_t *args) {
    mp_obj_t mesh_args[]={args[0],args[2],args[3],count>4 ? args[4] : MP_OBJ_NEW_SMALL_INT(0)};
    return python_jet_mesh_data(4,mesh_args,python_u32_from_obj(args[1]));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_jet_scene_mesh_update_obj,4,5,solaros_jet_scene_mesh_update);
