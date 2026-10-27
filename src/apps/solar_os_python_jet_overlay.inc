static mp_obj_t solaros_jet_scene_sprite(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SPRITE};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_sprite_fields,JET_SPR_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_sprite_obj,1,solaros_jet_scene_sprite);
static mp_obj_t solaros_jet_scene_sprite_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SPRITE_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_sprite_fields,JET_SPR_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_sprite_set_obj,2,solaros_jet_scene_sprite_set);
static mp_obj_t solaros_jet_scene_sprite_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SPRITE_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return python_jet_info_dict(&r,jet_sprite_fields,JET_SPR_COUNT);
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_sprite_info_obj,solaros_jet_scene_sprite_info);
static mp_obj_t solaros_jet_scene_pick(mp_obj_t self,mp_obj_t points) {
    solar_os_jet_t *scene=python_jet_scene(self);size_t n=0;mp_obj_t *entries=NULL;
    if(points!=mp_const_none)mp_obj_get_array(points,&n,&entries);
    if(n>SOLAR_OS_JET_MAX_PICKS)mp_raise_ValueError(MP_ERROR_TEXT("too many Jet pick points"));
    solar_os_jet_pick_t *data=m_new(solar_os_jet_pick_t,n);
    for(size_t i=0;i<n;++i) {
        size_t width;mp_obj_t *p;mp_obj_get_array(entries[i],&width,&p);
        if(width!=2)mp_raise_ValueError(MP_ERROR_TEXT("expected Jet pick x,y"));
        data[i]=(solar_os_jet_pick_t){python_jet_integer(p[0],-1,1023,0),python_jet_integer(p[1],-1,1023,0)};
    }
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_PICK,.data=data,.bytes=n*sizeof(*data)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));m_del(solar_os_jet_pick_t,data,n);return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_pick_obj,solaros_jet_scene_pick);
static mp_obj_t solaros_jet_scene_picks(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INFO};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));bool instancing=(r.handle&SOLAR_OS_JET_FEATURE_INSTANCING)!=0;
    q.opcode=SOLAR_OS_JET_PICK_RESULT;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));
    mp_obj_t list=mp_obj_new_list(0,NULL);unsigned count=r.count;
    for(unsigned i=0;i<count;++i) {
        if(i) {q.values[0]=i;PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));}
        mp_obj_t entry=mp_obj_new_dict(0);
        python_dict_store_bool(entry,"hit",r.values[0]);python_dict_store_uint(entry,"object",r.handle);
        python_dict_store_uint(entry,"mesh",instancing ? r.values[5] : 0);
        python_dict_store_uint(entry,"batch",instancing ? r.values[7] : 0);
        mp_obj_dict_store(entry,mp_obj_new_str("instance",8),mp_obj_new_int(instancing ? r.values[6] : -1));
        const char *names[]={"triangle","depth","x","y"};
        for(unsigned k=0;k<4;++k)mp_obj_dict_store(entry,mp_obj_new_str(names[k],strlen(names[k])),mp_obj_new_int(r.values[k+1]));
        mp_obj_list_append(list,entry);
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_picks_obj,solaros_jet_scene_picks);
