static void solua_jet_options(lua_State *L,int index,const solar_os_jet_field_t *fields,unsigned n,solar_os_jet_command_t *q) {
    if(lua_isnoneornil(L,index)) {
        for(unsigned i=0;i<n;++i) q->values[i]=fields[i].initial;
        return;
    }
    luaL_checktype(L,index,LUA_TTABLE);index=lua_absindex(L,index);
    lua_pushnil(L);
    while(lua_next(L,index)) {
        luaL_checktype(L,-2,LUA_TSTRING);size_t length;const char *key=lua_tolstring(L,-2,&length);
        unsigned i=0;
        for(;i<n && (strlen(fields[i].name)!=length || memcmp(key,fields[i].name,length));++i) {}
        if(i==n) luaL_error(L,"unknown Jet option");
        lua_pop(L,1);
    }
    for(unsigned i=0;i<n;++i) {
        q->values[i]=fields[i].initial;lua_getfield(L,index,fields[i].name);
        if(lua_isnil(L,-1)) {lua_pop(L,1);continue;}
        q->fields|=1U<<i;bool retain=false;
        switch(fields[i].kind) {
        case JET_NAME: {
            size_t length;const char *name=luaL_checklstring(L,-1,&length);
            if(length>=sizeof(q->name) || memchr(name,0,length)) luaL_error(L,"invalid Jet resource name");
            memcpy(q->name,name,length);q->name[length]=0;break;
        }
        case JET_ENUM: {
            size_t length;const char *text=luaL_checklstring(L,-1,&length);
            int choice=solar_os_jet_choice(fields[i].choices,text,length);
            if(choice<0) luaL_error(L,"unknown Jet option");
            q->values[i]=choice;break;
        }
        case JET_BOOL:luaL_checktype(L,-1,LUA_TBOOLEAN);q->values[i]=lua_toboolean(L,-1);break;
        case JET_FLOAT:q->values[i]=luaL_checknumber(L,-1);break;
        case JET_BYTES:luaL_checktype(L,-1,LUA_TSTRING);q->data=lua_tolstring(L,-1,&q->bytes);break;
        case JET_PALETTE:case JET_HANDLES: {
            luaL_checktype(L,-1,LUA_TTABLE);int colors=lua_absindex(L,-1);size_t length=lua_rawlen(L,colors);
            if((!length&&fields[i].kind==JET_PALETTE) || length>(fields[i].kind==JET_HANDLES ? SOLAR_OS_JET_MAX_TRIANGLES : 256))
                luaL_error(L,"invalid Jet entry list");
            uint32_t *palette=lua_newuserdatauv(L,length*sizeof(*palette),0);
            for(size_t k=0;k<length;++k) {
                lua_rawgeti(L,colors,k+1);lua_Integer id=luaL_checkinteger(L,-1);
                if(fields[i].kind==JET_HANDLES) {
                    if(id<0||(uint64_t)id>UINT32_MAX)luaL_error(L,"invalid Jet handle");
                    palette[k]=(uint32_t)id;
                } else palette[k]=solua_jet_integer(L,-1,0,0xffffff,0);
                lua_pop(L,1);
            }
            lua_replace(L,colors);retain=true;q->extra=palette;q->extra_bytes=length*sizeof(*palette);break;
        }
        default:q->values[i]=luaL_checkinteger(L,-1);break;
        }
        if(!isfinite(q->values[i]) || q->values[i]<fields[i].minimum || q->values[i]>fields[i].maximum)
            luaL_error(L,"Jet option out of range");
        if(!retain) lua_pop(L,1);
    }
}
static void solua_jet_info_table(lua_State *L,const solar_os_jet_reply_t *r,const solar_os_jet_field_t *fields,unsigned n) {
    lua_newtable(L);int table=lua_absindex(L,-1);solua_set_int(L,table,"id",r->handle);
    for(unsigned i=0;i<n;++i) {
        const solar_os_jet_field_t *f=&fields[i];
        if(f->kind==JET_BYTES || f->kind==JET_PALETTE || f->kind==JET_HANDLES) continue;
        if(f->kind==JET_NAME) lua_pushstring(L,r->name);
        else if(f->kind==JET_ENUM) {
            size_t length;const char *name=solar_os_jet_choice_at(f->choices,r->values[i],&length);lua_pushlstring(L,name,length);
        } else if(f->kind==JET_BOOL) lua_pushboolean(L,r->values[i]!=0);
        else if(f->kind==JET_FLOAT) lua_pushnumber(L,r->values[i]);
        else lua_pushinteger(L,r->values[i]);
        lua_setfield(L,table,f->name);
    }
}
static int solua_jet_scene_capabilities(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_INFO};solar_os_jet_reply_t r={0};
    esp_err_t err;
    SOLUA_JET_CALL((err=solar_os_jet_execute(scene,&q,&r),err==ESP_ERR_NOT_SUPPORTED ? ESP_OK : err));
    lua_newtable(L);lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_MATERIALS);lua_setfield(L,-2,"materials");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_SHADERS);lua_setfield(L,-2,"shaders");solua_set_int(L,-1,"max_shaders",r.values[14]);
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_RENDERER_CONTROLS);lua_setfield(L,-2,"renderer_controls");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_BACKGROUND_CACHE);lua_setfield(L,-2,"background_cache");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_TEXTURES);lua_setfield(L,-2,"textures");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_GEOMETRY);lua_setfield(L,-2,"geometry");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_SCENE);lua_setfield(L,-2,"scene");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_SPRITES);lua_setfield(L,-2,"sprites");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_PICKING);lua_setfield(L,-2,"picking");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_ASSETS);lua_setfield(L,-2,"assets");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_INSTANCING);lua_setfield(L,-2,"instancing");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_PARTICLES);lua_setfield(L,-2,"particles");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_FLARES);lua_setfield(L,-2,"flares");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_POSTFX);lua_setfield(L,-2,"postfx");
    lua_pushboolean(L,r.handle&SOLAR_OS_JET_FEATURE_TILED_TEXTURES);lua_setfield(L,-2,"tiled_textures");
    const char *names[]={"objects","triangles","vertices","max_materials","max_textures","max_lights","max_objects","max_sprites","max_picks","max_prototypes","max_instances","max_particle_systems","max_flares","max_flare_elements"};
    for(unsigned i=0;i<14;++i) solua_set_int(L,-1,names[i],r.values[i]);
    return 1;
}
static int solua_jet_scene_material(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MATERIAL};solar_os_jet_reply_t r;
    if(lua_gettop(L)>2) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,2,jet_material_fields,JET_MAT_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_material_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MATERIAL_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,3,jet_material_fields,JET_MAT_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_material_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MATERIAL_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_material_fields,JET_MAT_COUNT);return 1;
}
static int solua_jet_scene_texture(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_TEXTURE};solar_os_jet_reply_t r;
    if(lua_gettop(L)>5) return luaL_error(L,"too many Jet arguments");
    unsigned width=solua_jet_integer(L,2,1,1024,0),height=solua_jet_integer(L,3,1,1024,0);
    luaL_checktype(L,4,LUA_TSTRING);size_t bytes;const char *pixels=lua_tolstring(L,4,&bytes);
    solua_jet_options(L,5,jet_texture_fields,JET_TEX_COUNT,&q);
    if(q.fields&(1U<<JET_TEX_pixels)) return luaL_error(L,"duplicate Jet pixels");
    q.data=pixels;q.bytes=bytes;q.values[16]=width;q.values[17]=height;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
static int solua_jet_scene_texture_set(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_TEXTURE_SET,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    if(lua_gettop(L)>3) return luaL_error(L,"too many Jet arguments");
    solua_jet_options(L,3,jet_texture_fields,JET_TEX_COUNT,&q);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int solua_jet_scene_texture_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_TEXTURE_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_texture_fields,JET_TEX_COUNT);
    solua_set_int(L,-1,"width",r.values[16]);solua_set_int(L,-1,"height",r.values[17]);solua_set_int(L,-1,"palette_size",r.values[18]);
    solua_set_int(L,-1,"tile_bytes",r.values[19]);solua_set_int(L,-1,"filter_bytes",r.values[20]);
    solua_set_int(L,-1,"sampler_width",r.values[21] ? r.values[21] : r.values[16]);
    solua_set_int(L,-1,"sampler_height",r.values[22] ? r.values[22] : r.values[17]);return 1;
}
static uint32_t solua_jet_optional_handle(lua_State *L,int index) {
    lua_Integer id=luaL_optinteger(L,index,0);
    if(id<0 || (uint64_t)id>UINT32_MAX) luaL_error(L,"invalid Jet handle");
    return (uint32_t)id;
}
static int solua_jet_scene_assign(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_ASSIGN,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    q.values[0]=solua_jet_optional_handle(L,3);SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));return 0;
}
static int32_t solua_jet_fixed(lua_State *L,int index,double limit) {
    double value=luaL_checknumber(L,index);
    if(!isfinite(value) || value < -limit || value > limit) luaL_error(L,"Jet vector out of range");
    return (int32_t)(value*1024);
}
static int solua_jet_scene_sample_texture(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SAMPLE_TEXTURE,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    q.values[0]=luaL_checknumber(L,3);q.values[1]=luaL_checknumber(L,4);
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.values[0]);return 1;
}
static int solua_jet_mesh_data(lua_State *L,uint32_t handle) {
    solar_os_jet_t *scene=solua_jet_scene(L);luaL_checktype(L,2,LUA_TTABLE);luaL_checktype(L,3,LUA_TTABLE);
    size_t n=lua_rawlen(L,2),t=lua_rawlen(L,3),stride=0;
    if(!n || n>SOLAR_OS_JET_MAX_VERTICES || !t || t>SOLAR_OS_JET_MAX_TRIANGLES) return luaL_error(L,"invalid Jet mesh size");
    uint32_t mat=solua_jet_optional_handle(L,4);
    solar_os_jet_mesh_vertex_t *v=lua_newuserdatauv(L,n*sizeof(*v),0);
    solar_os_jet_mesh_face_t *faces=lua_newuserdatauv(L,t*sizeof(*faces),0);
    for(size_t i=0;i<n;++i) {
        lua_rawgeti(L,2,i+1);luaL_checktype(L,-1,LUA_TTABLE);size_t width=lua_rawlen(L,-1);
        if((width!=3 && width!=5 && width!=8) || (i && width!=stride)) return luaL_error(L,"expected consistent xyz, xyzuv or xyzuv-normal vertices");
        stride=width;int32_t a[8]={0};
        for(unsigned k=0;k<width;++k) {
            lua_rawgeti(L,-1,k+1);
            if(k<3) {luaL_checkinteger(L,-1);a[k]=solua_jet_integer(L,-1,-4096,4096,0);}
            else a[k]=solua_jet_fixed(L,-1,k<5 ? 32 : 1);
            lua_pop(L,1);
        }
        v[i]=(solar_os_jet_mesh_vertex_t){a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]};lua_pop(L,1);
    }
    for(size_t i=0;i<t;++i) {
        lua_rawgeti(L,3,i+1);luaL_checktype(L,-1,LUA_TTABLE);size_t width=lua_rawlen(L,-1);
        if(width!=3 && width!=4) return luaL_error(L,"expected triangle indices and optional material");
        uint16_t indices[3];
        for(unsigned k=0;k<3;++k) {
            lua_rawgeti(L,-1,k+1);luaL_checkinteger(L,-1);indices[k]=solua_jet_integer(L,-1,1,n,1)-1;lua_pop(L,1);
        }
        uint32_t face_mat=mat;
        if(width==4) {lua_rawgeti(L,-1,4);luaL_checkinteger(L,-1);face_mat=solua_jet_optional_handle(L,-1);lua_pop(L,1);}
        faces[i]=(solar_os_jet_mesh_face_t){indices[0],indices[1],indices[2],0,face_mat};lua_pop(L,1);
    }
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_MESH_DATA,.handle=handle,.fields=stride==8 ? SOLAR_OS_JET_MESH_NORMALS : 0,
        .data=v,.bytes=n*sizeof(*v),.extra=faces,.extra_bytes=t*sizeof(*faces)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));lua_pushinteger(L,r.handle);return 1;
}
