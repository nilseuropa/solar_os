static void python_jet_options(size_t count,const mp_obj_t *args,mp_map_t *kwargs,
    const solar_os_jet_field_t *fields,unsigned n,solar_os_jet_command_t *q) {
    const char *names[SOLAR_OS_JET_VALUES];mp_obj_t values[SOLAR_OS_JET_VALUES];
    for(unsigned i=0;i<n;++i) names[i]=fields[i].name;
    python_jet_args(count,args,kwargs,names,n,values);
    for(unsigned i=0;i<n;++i) {
        mp_obj_t value=values[i];q->values[i]=fields[i].initial;
        if(value==MP_OBJ_NULL) continue;
        q->fields|=1U<<i;
        switch(fields[i].kind) {
        case JET_NAME: {
            size_t length;const char *name=mp_obj_str_get_data(value,&length);
            if(length>=sizeof(q->name) || memchr(name,0,length)) mp_raise_ValueError(MP_ERROR_TEXT("invalid Jet resource name"));
            memcpy(q->name,name,length);q->name[length]=0;break;
        }
        case JET_ENUM: {
            size_t length;const char *text=mp_obj_str_get_data(value,&length);
            int index=solar_os_jet_choice(fields[i].choices,text,length);
            if(index<0) mp_raise_ValueError(MP_ERROR_TEXT("unknown Jet option"));
            q->values[i]=index;break;
        }
        case JET_BOOL:q->values[i]=mp_obj_is_true(value);break;
        case JET_FLOAT:q->values[i]=mp_obj_get_float(value);break;
        case JET_BYTES: {
            mp_buffer_info_t buffer;mp_get_buffer_raise(value,&buffer,MP_BUFFER_READ);
            q->data=buffer.buf;q->bytes=buffer.len;break;
        }
        case JET_PALETTE:case JET_HANDLES: {
            size_t length=0;mp_obj_t *colors=NULL;
            if(fields[i].kind!=JET_HANDLES||value!=mp_const_none)mp_obj_get_array(value,&length,&colors);
            if((!length&&fields[i].kind==JET_PALETTE) || length>(fields[i].kind==JET_HANDLES ? SOLAR_OS_JET_MAX_TRIANGLES : 256))
                mp_raise_ValueError(MP_ERROR_TEXT("invalid Jet entry list"));
            uint32_t *palette=m_new(uint32_t,length);
            for(size_t k=0;k<length;++k) palette[k]=fields[i].kind==JET_HANDLES ? python_u32_from_obj(colors[k]) : python_jet_color(colors[k]);
            q->extra=palette;q->extra_bytes=length*sizeof(*palette);break;
        }
        default:
            if(fields[i].maximum==4294967295.0) q->values[i]=python_u32_from_obj(value);
            else q->values[i]=mp_obj_get_int(value);
            break;
        }
        if(!isfinite(q->values[i]) || q->values[i]<fields[i].minimum || q->values[i]>fields[i].maximum)
            mp_raise_ValueError(MP_ERROR_TEXT("Jet option out of range"));
    }
}
static mp_obj_t python_jet_info_dict(const solar_os_jet_reply_t *r,const solar_os_jet_field_t *fields,unsigned n) {
    mp_obj_t dict=mp_obj_new_dict(0);
    python_dict_store_uint(dict,"id",r->handle);
    for(unsigned i=0;i<n;++i) {
        const solar_os_jet_field_t *f=&fields[i];mp_obj_t value;
        if(f->kind==JET_BYTES || f->kind==JET_PALETTE || f->kind==JET_HANDLES) continue;
        if(f->kind==JET_NAME) value=mp_obj_new_str(r->name,strlen(r->name));
        else if(f->kind==JET_ENUM) {
            size_t length;const char *name=solar_os_jet_choice_at(f->choices,r->values[i],&length);
            value=mp_obj_new_str(name,length);
        } else if(f->kind==JET_BOOL) value=mp_obj_new_bool(r->values[i]!=0);
        else if(f->kind==JET_FLOAT) value=mp_obj_new_float(r->values[i]);
        else if(r->values[i]>=0) value=mp_obj_new_int_from_uint((uint32_t)r->values[i]);
        else value=mp_obj_new_int((mp_int_t)r->values[i]);
        mp_obj_dict_store(dict,mp_obj_new_str(f->name,strlen(f->name)),value);
    }
    return dict;
}
static mp_obj_t solaros_jet_scene_capabilities(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INFO};solar_os_jet_reply_t r={0};
    esp_err_t err;
    PYTHON_JET_CALL((err=solar_os_jet_execute(scene,&q,&r),err==ESP_ERR_NOT_SUPPORTED ? ESP_OK : err));
    mp_obj_t dict=mp_obj_new_dict(0);
    python_dict_store_bool(dict,"materials",r.handle&SOLAR_OS_JET_FEATURE_MATERIALS);
    python_dict_store_bool(dict,"textures",r.handle&SOLAR_OS_JET_FEATURE_TEXTURES);
    python_dict_store_bool(dict,"geometry",r.handle&SOLAR_OS_JET_FEATURE_GEOMETRY);
    python_dict_store_bool(dict,"scene",r.handle&SOLAR_OS_JET_FEATURE_SCENE);
    python_dict_store_bool(dict,"sprites",r.handle&SOLAR_OS_JET_FEATURE_SPRITES);
    python_dict_store_bool(dict,"picking",r.handle&SOLAR_OS_JET_FEATURE_PICKING);
    python_dict_store_bool(dict,"assets",r.handle&SOLAR_OS_JET_FEATURE_ASSETS);
    python_dict_store_bool(dict,"instancing",r.handle&SOLAR_OS_JET_FEATURE_INSTANCING);
    python_dict_store_bool(dict,"particles",r.handle&SOLAR_OS_JET_FEATURE_PARTICLES);
    python_dict_store_bool(dict,"flares",r.handle&SOLAR_OS_JET_FEATURE_FLARES);
    python_dict_store_bool(dict,"postfx",r.handle&SOLAR_OS_JET_FEATURE_POSTFX);
    python_dict_store_bool(dict,"tiled_textures",r.handle&SOLAR_OS_JET_FEATURE_TILED_TEXTURES);
    python_dict_store_bool(dict,"shaders",r.handle&SOLAR_OS_JET_FEATURE_SHADERS);
    python_dict_store_bool(dict,"renderer_controls",r.handle&SOLAR_OS_JET_FEATURE_RENDERER_CONTROLS);
    python_dict_store_bool(dict,"background_cache",r.handle&SOLAR_OS_JET_FEATURE_BACKGROUND_CACHE);
    const char *names[]={"objects","triangles","vertices","max_materials","max_textures","max_lights","max_objects","max_sprites","max_picks","max_prototypes","max_instances","max_particle_systems","max_flares","max_flare_elements"};
    for(unsigned i=0;i<14;++i) python_dict_store_uint(dict,names[i],r.values[i]);
    python_dict_store_uint(dict,"max_shaders",r.values[14]);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_capabilities_obj,solaros_jet_scene_capabilities);
static mp_obj_t solaros_jet_scene_material(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MATERIAL};solar_os_jet_reply_t r;
    python_jet_options(count-1,args+1,kwargs,jet_material_fields,JET_MAT_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_material_obj,1,solaros_jet_scene_material);
static mp_obj_t solaros_jet_scene_material_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MATERIAL_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_material_fields,JET_MAT_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_material_set_obj,2,solaros_jet_scene_material_set);
static mp_obj_t solaros_jet_scene_material_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MATERIAL_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return python_jet_info_dict(&r,jet_material_fields,JET_MAT_COUNT);
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_material_info_obj,solaros_jet_scene_material_info);
static mp_obj_t solaros_jet_scene_texture(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_TEXTURE};solar_os_jet_reply_t r;
    unsigned width=python_jet_integer(args[1],1,1024,1),height=python_jet_integer(args[2],1,1024,1);
    python_jet_options(count-4,args+4,kwargs,jet_texture_fields,JET_TEX_COUNT,&q);
    if(q.fields&(1U<<JET_TEX_pixels)) mp_raise_TypeError(MP_ERROR_TEXT("duplicate Jet pixels"));
    mp_buffer_info_t pixels;mp_get_buffer_raise(args[3],&pixels,MP_BUFFER_READ);q.data=pixels.buf;q.bytes=pixels.len;
    q.values[16]=width;q.values[17]=height;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.handle);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_texture_obj,4,solaros_jet_scene_texture);
static mp_obj_t solaros_jet_scene_texture_set(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_TEXTURE_SET,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    python_jet_options(count-2,args+2,kwargs,jet_texture_fields,JET_TEX_COUNT,&q);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_texture_set_obj,2,solaros_jet_scene_texture_set);
static mp_obj_t solaros_jet_scene_texture_info(mp_obj_t self,mp_obj_t handle) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_TEXTURE_INFO,.handle=python_u32_from_obj(handle)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));mp_obj_t dict=python_jet_info_dict(&r,jet_texture_fields,JET_TEX_COUNT);
    python_dict_store_uint(dict,"width",r.values[16]);python_dict_store_uint(dict,"height",r.values[17]);
    python_dict_store_uint(dict,"palette_size",r.values[18]);python_dict_store_uint(dict,"tile_bytes",r.values[19]);
    python_dict_store_uint(dict,"filter_bytes",r.values[20]);python_dict_store_uint(dict,"sampler_width",r.values[21] ? r.values[21] : r.values[16]);
    python_dict_store_uint(dict,"sampler_height",r.values[22] ? r.values[22] : r.values[17]);return dict;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_texture_info_obj,solaros_jet_scene_texture_info);
static mp_obj_t solaros_jet_scene_assign(mp_obj_t self,mp_obj_t object,mp_obj_t material) {
    solar_os_jet_t *scene=python_jet_scene(self);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_ASSIGN,.handle=python_u32_from_obj(object)};solar_os_jet_reply_t r;
    q.values[0]=python_u32_from_obj(material);PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_jet_scene_assign_obj,solaros_jet_scene_assign);
static mp_obj_t solaros_jet_scene_sample_texture(size_t count,const mp_obj_t *args) {
    (void)count;solar_os_jet_t *scene=python_jet_scene(args[0]);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SAMPLE_TEXTURE,.handle=python_u32_from_obj(args[1])};solar_os_jet_reply_t r;
    q.values[0]=mp_obj_get_float(args[2]);q.values[1]=mp_obj_get_float(args[3]);
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));return mp_obj_new_int_from_uint(r.values[0]);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_jet_scene_sample_texture_obj,4,4,solaros_jet_scene_sample_texture);
static int32_t python_jet_fixed(mp_obj_t object,float limit) {
    double value=mp_obj_get_float(object);
    if(!isfinite(value) || value < -limit || value > limit) mp_raise_ValueError(MP_ERROR_TEXT("Jet vector out of range"));
    return (int32_t)(value*1024);
}
static mp_obj_t python_jet_mesh_data(size_t count,const mp_obj_t *args,uint32_t handle) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    size_t n,t;mp_obj_t *vertices,*triangles;mp_obj_get_array(args[1],&n,&vertices);mp_obj_get_array(args[2],&t,&triangles);
    if(!n || n>SOLAR_OS_JET_MAX_VERTICES || !t || t>SOLAR_OS_JET_MAX_TRIANGLES)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid Jet mesh size"));
    uint32_t mat=count>3 ? python_u32_from_obj(args[3]) : 0;
    solar_os_jet_mesh_vertex_t *v=m_new(solar_os_jet_mesh_vertex_t,n);
    solar_os_jet_mesh_face_t *faces=m_new(solar_os_jet_mesh_face_t,t);
    size_t stride=0;
    for(size_t i=0;i<n;++i) {
        size_t width;mp_obj_t *a;mp_obj_get_array(vertices[i],&width,&a);
        if((width!=3 && width!=5 && width!=8) || (i && width!=stride)) mp_raise_ValueError(MP_ERROR_TEXT("expected consistent xyz, xyzuv or xyzuv-normal vertices"));
        stride=width;v[i]=(solar_os_jet_mesh_vertex_t){.x=python_jet_integer(a[0],-4096,4096,0),
            .y=python_jet_integer(a[1],-4096,4096,0),.z=python_jet_integer(a[2],-4096,4096,0)};
        if(width>=5) {v[i].u=python_jet_fixed(a[3],32);v[i].v=python_jet_fixed(a[4],32);}
        if(width==8) {v[i].nx=python_jet_fixed(a[5],1);v[i].ny=python_jet_fixed(a[6],1);v[i].nz=python_jet_fixed(a[7],1);}
    }
    for(size_t i=0;i<t;++i) {
        size_t width;mp_obj_t *a;mp_obj_get_array(triangles[i],&width,&a);
        if(width!=3 && width!=4) mp_raise_ValueError(MP_ERROR_TEXT("expected triangle indices and optional material"));
        faces[i]=(solar_os_jet_mesh_face_t){python_jet_integer(a[0],0,n-1,0),python_jet_integer(a[1],0,n-1,0),
            python_jet_integer(a[2],0,n-1,0),0,width==4 ? python_u32_from_obj(a[3]) : mat};
    }
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MESH_DATA,.handle=handle,.fields=stride==8 ? SOLAR_OS_JET_MESH_NORMALS : 0,
        .data=v,.bytes=n*sizeof(*v),.extra=faces,.extra_bytes=t*sizeof(*faces)};solar_os_jet_reply_t r;
    PYTHON_JET_CALL(solar_os_jet_execute(scene,&q,&r));m_del(solar_os_jet_mesh_vertex_t,v,n);m_del(solar_os_jet_mesh_face_t,faces,t);
    return mp_obj_new_int_from_uint(r.handle);
}
