static int python_jet_shader_callback(void *user,uint32_t slot,double u,double v,
    const solar_os_jet_shader_parameters_t *params,uint32_t *color) {
    python_jet_t *self=user;
    if(self->shader_error!=MP_OBJ_NULL)return SOLAR_OS_NATIVE_ERROR_FAILED;
    nlr_buf_t nlr;python_jet_in_callback=true;
    if(nlr_push(&nlr)==0) {
        mp_obj_t dict=mp_obj_new_dict(0);
        python_dict_store_int(dict,"x",params->screen_x);python_dict_store_int(dict,"y",params->screen_y);
        python_dict_store_int(dict,"depth",params->depth);python_dict_store_uint(dict,"frame",params->frame);
        mp_obj_t local[3],world[3];
        for(unsigned i=0;i<3;++i) {local[i]=mp_obj_new_int(params->local[i]);world[i]=mp_obj_new_int(params->world[i]);}
        mp_obj_dict_store(dict,mp_obj_new_str("local",5),mp_obj_new_tuple(3,local));
        mp_obj_dict_store(dict,mp_obj_new_str("world",5),mp_obj_new_tuple(3,world));
        mp_obj_t args[]={mp_obj_new_float(u),mp_obj_new_float(v),dict};
        *color=python_jet_color(mp_call_function_n_kw(self->shader_functions[slot],3,0,args));
        nlr_pop();
    } else self->shader_error=MP_OBJ_FROM_PTR(nlr.ret_val);
    python_jet_in_callback=false;
    return self->shader_error==MP_OBJ_NULL ? 0 : SOLAR_OS_NATIVE_ERROR_FAILED;
}
static mp_obj_t python_jet_shader(size_t count,const mp_obj_t *args,mp_map_t *kwargs,bool update) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);python_jet_t *self=python_jet_object(args[0]);
    unsigned first=update ? 2 : 1;
    if(count>first+1)mp_raise_TypeError(MP_ERROR_TEXT("too many Jet shader arguments"));
    uint32_t handle=update ? python_u32_from_obj(args[1]) : 0;
    unsigned slot=0;for(;slot<SOLAR_OS_JET_MAX_SHADERS;++slot)if(self->shader_ids[slot]==handle)break;
    if(slot==SOLAR_OS_JET_MAX_SHADERS)mp_raise_ValueError(MP_ERROR_TEXT("Jet shader slot unavailable"));
    solar_os_jet_command_t q={.opcode=update ? SOLAR_OS_JET_SHADER_SET : SOLAR_OS_JET_SHADER,.handle=handle};
    solar_os_jet_reply_t r;
    mp_obj_t function=count>first ? args[first] : MP_OBJ_NULL;
    mp_obj_t keyword=python_kw_value(kwargs,"callback");
    if(keyword!=MP_OBJ_NULL) {
        if(function!=MP_OBJ_NULL)mp_raise_TypeError(MP_ERROR_TEXT("duplicate Jet callback"));
        function=keyword;
    }
    mp_obj_t entries[JET_SHADER_COUNT*2];size_t used=0;
    if(kwargs)for(size_t i=0;i<kwargs->alloc;++i) {
        if(!mp_map_slot_is_filled(kwargs,i))continue;
        size_t length;const char *key=mp_obj_str_get_data(kwargs->table[i].key,&length);
        if(length==8&&!memcmp(key,"callback",8))continue;
        if(used==JET_SHADER_COUNT)mp_raise_TypeError(MP_ERROR_TEXT("unknown Jet option"));
        entries[used*2]=kwargs->table[i].key;entries[used*2+1]=kwargs->table[i].value;++used;
    }
    mp_map_t options;mp_map_init_fixed_table(&options,used,entries);
    python_jet_options(0,NULL,&options,jet_shader_fields,JET_SHADER_COUNT,&q);
    solar_os_jet_shader_callback_t callback={python_jet_shader_callback,self,slot};
    if(function!=MP_OBJ_NULL&&function!=mp_const_none) {
        if(!mp_obj_is_callable(function))mp_raise_TypeError(MP_ERROR_TEXT("expected a shader function"));
        if((q.fields&(1U<<JET_SHADER_kind))&&q.values[JET_SHADER_kind]!=3)
            mp_raise_ValueError(MP_ERROR_TEXT("callback requires shader kind callback"));
        q.fields|=1U<<JET_SHADER_kind;q.values[JET_SHADER_kind]=3;q.data=&callback;q.bytes=sizeof(callback);
    }
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));self->shader_ids[slot]=r.handle;
    if(q.data)self->shader_functions[slot]=function;
    else if(!update||((q.fields&(1U<<JET_SHADER_kind))&&q.values[JET_SHADER_kind]!=3))self->shader_functions[slot]=MP_OBJ_NULL;
    return update ? mp_const_none : mp_obj_new_int_from_uint(r.handle);
}
static mp_obj_t solaros_jet_scene_shader(size_t n,const mp_obj_t *args,mp_map_t *kw) {return python_jet_shader(n,args,kw,false);}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_shader_obj,1,solaros_jet_scene_shader);
static mp_obj_t solaros_jet_scene_shader_set(size_t n,const mp_obj_t *args,mp_map_t *kw) {return python_jet_shader(n,args,kw,true);}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_shader_set_obj,2,solaros_jet_scene_shader_set);
static mp_obj_t solaros_jet_scene_shader_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SHADER_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));mp_obj_t dict=python_jet_info_dict(&r,jet_shader_fields,JET_SHADER_COUNT);
    python_dict_store_uint(dict,"calls",r.values[16]);python_dict_store_bool(dict,"failed",r.values[17]);return dict;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_shader_info_obj,solaros_jet_scene_shader_info);
