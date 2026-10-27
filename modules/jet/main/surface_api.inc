template<class T,size_t N> static T *resource(std::unique_ptr<T> (&items)[N],uint32_t id) {
    for(auto &item:items) if(item && item->id==id) return item.get();
    return nullptr;
}
template<class T,size_t N> static int store(std::unique_ptr<T> (&items)[N],std::unique_ptr<T> item,
    const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(active->next_id==UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    for(auto &slot:items) if(!slot) {
        item->id=++active->next_id; std::memcpy(item->name,q.name,sizeof(item->name));
        r.handle=item->id; slot=std::move(item); return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
template<class T,size_t N> static bool duplicate_name(std::unique_ptr<T> (&items)[N],const char *name,uint32_t except) {
    if(!*name) return false;
    for(auto &item:items) if(item && item->id!=except && !std::strcmp(item->name,name)) return true;
    return false;
}
static bool valid_values(const solar_os_jet_command_t &q,const solar_os_jet_field_t *fields,unsigned n) {
    if(q.fields & ~((1U<<n)-1)) return false;
    for(unsigned i=0;i<n;++i) if(q.fields&(1U<<i)) {
        double v=q.values[i];
        if(!std::isfinite(v) || v<fields[i].minimum || v>fields[i].maximum ||
            (fields[i].kind!=JET_FLOAT && std::floor(v)!=v)) return false;
    }
    return true;
}
static void merge_values(double *out,const solar_os_jet_command_t &q,const solar_os_jet_field_t *fields,
    unsigned n,const double *old=nullptr) {
    for(unsigned i=0;i<n;++i) out[i]=(q.fields&(1U<<i)) ? q.values[i] : (old ? old[i] : fields[i].initial);
}
static Material *material(uint32_t id,Material *fallback=nullptr) {
    if(!id) return fallback;
    auto *entry=resource(active->materials,id); return entry ? &entry->value : nullptr;
}
static void surface_reply(const Resource &entry,solar_os_jet_reply_t &r) {
    r.handle=entry.id;std::memcpy(r.values,entry.values,sizeof(r.values));std::memcpy(r.name,entry.name,sizeof(r.name));
}
static void material_values(Material &updated,const double *v,Texture *texture) {
    updated.color=rgb565(uint32_t(v[JET_MAT_color]));updated.diffuseMap=texture;
    updated.shadingMode=ShadingMode(int(v[JET_MAT_shading]));updated.alpha=v[JET_MAT_alpha];
    updated.diffuse=v[JET_MAT_diffuse];updated.specular=v[JET_MAT_specular];updated.specularExponent=v[JET_MAT_gloss];
    updated.emissive=v[JET_MAT_emissive];updated.perspectiveCorrect=v[JET_MAT_perspective];
    updated.waterYBias=v[JET_MAT_water_bias];updated.waterReflectionMaxY=v[JET_MAT_reflection_max_y];
}
static int material_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    auto *old=q.opcode==SOLAR_OS_JET_MATERIAL ? nullptr : resource(active->materials,q.handle);
    if(q.opcode!=SOLAR_OS_JET_MATERIAL && !old) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.opcode==SOLAR_OS_JET_MATERIAL_INFO) {surface_reply(*old,r);return 0;}
    if(!valid_values(q,jet_material_fields,JET_MAT_COUNT)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if(duplicate_name(active->materials,q.name,old ? old->id : 0)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_material_fields,JET_MAT_COUNT,old ? old->values : nullptr);
    auto *texture=resource(active->textures,uint32_t(v[JET_MAT_texture]));
    if(v[JET_MAT_texture] && !texture) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    auto *shader=resource(active->shaders,uint32_t(v[JET_MAT_shader]));
    if(v[JET_MAT_shader] && !shader)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(shader) {
        if(v[JET_MAT_shading]==3||v[JET_MAT_shading]==5)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if(old) {
            for(auto &sprite:active->sprites)if(sprite&&sprite->value.material==&old->value)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
            for(auto &flare:active->flares)if(flare)for(unsigned i=0;i<flare->count;++i)
                if(flare->elements[i].values[JET_ELEM_material]==old->id)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        }
    }
    auto fresh=old ? nullptr : std::make_unique<MaterialResource>();
    auto *entry=old ? old : fresh.get();
    Material updated=entry->value;
    material_values(updated,v,texture ? texture->value.get() : nullptr);
    updated.shader=shader;
    entry->value=updated;std::memcpy(entry->values,v,sizeof(v));
    if(q.fields&(1U<<JET_MAT_name)) std::memcpy(entry->name,q.name,sizeof(entry->name));
    if(old) {r.handle=old->id;return 0;}
    return store(active->materials,std::move(fresh),q,r);
}
static void texture_settings(Texture &tex,const double *v) {
    tex.addressMode=TextureAddressMode(int(v[JET_TEX_address]));tex.hasAlpha=v[JET_TEX_alpha_key]>=0;
    tex.alphaColor=tex.hasAlpha ? rgb565(uint32_t(v[JET_TEX_alpha_key])) : 0;
    tex.screenSpace=v[JET_TEX_screen_space];tex.reflectionMap=v[JET_TEX_reflection];tex.bilinear=v[JET_TEX_bilinear];
    tex.paletteOffset=tex.paletteSize ? int(v[JET_TEX_offset])%tex.paletteSize : 0;
}
static int texture_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    auto *old=q.opcode==SOLAR_OS_JET_TEXTURE ? nullptr : resource(active->textures,q.handle);
    if(q.opcode!=SOLAR_OS_JET_TEXTURE && !old) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.opcode==SOLAR_OS_JET_TEXTURE_INFO) {
        surface_reply(*old,r);r.values[16]=old->width;r.values[17]=old->height;
        r.values[18]=old->value->paletteSize;
        if(old->tiles) {
            r.values[19]=old->tiles->pages.size()+old->tiles->coarse.size()+old->tiles->palette.size()*2;
            r.values[20]=old->tiles->filtered.size()*2;
        }
        r.values[21]=old->tiles&&old->tiles->view.coarseOnly ? old->tiles->view.coarseWidth : old->width;
        r.values[22]=old->tiles&&old->tiles->view.coarseOnly ? old->tiles->view.coarseHeight : old->height;return 0;
    }
    if(!valid_values(q,jet_texture_fields,JET_TEX_COUNT) ||
        duplicate_name(active->textures,q.name,old ? old->id : 0)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_texture_fields,JET_TEX_COUNT,old ? old->values : nullptr);
    if(!old && (!std::isfinite(q.values[16]) || !std::isfinite(q.values[17]) ||
        q.values[16]<1 || q.values[16]>1024 || q.values[17]<1 || q.values[17]>1024 ||
        std::floor(q.values[16])!=q.values[16] || std::floor(q.values[17])!=q.values[17])) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    unsigned width=old ? old->width : unsigned(q.values[16]),height=old ? old->height : unsigned(q.values[17]);
    if(old && v[JET_TEX_format]!=old->values[JET_TEX_format]) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t count=size_t(width)*height;unsigned format=v[JET_TEX_format];
    if(v[JET_TEX_tiled]&&format!=2)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t expected=count*(format==0 ? 2 : format==1 ? 3 : 1);
    bool replace=!old || q.data || q.bytes;
    if(replace && (!q.data || q.bytes!=expected)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    bool palette_replace=q.extra || q.extra_bytes;
    if(palette_replace && (format!=2 || !q.extra || !q.extra_bytes || q.extra_bytes%4 || q.extra_bytes>256*4))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if(format==2 && !old && !palette_replace) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if(old && !replace && !palette_replace) {
        bool reconfigure=(bool(old->tiles)!=bool(v[JET_TEX_tiled]))||
            (v[JET_TEX_tiled]&&(v[JET_TEX_filter]!=old->values[JET_TEX_filter]||v[JET_TEX_mip_level]!=old->values[JET_TEX_mip_level]||
                (v[JET_TEX_filter]==3&&v[JET_TEX_offset]!=old->values[JET_TEX_offset])));
        auto tiles=reconfigure ? prepare_tiles(*old,v) : nullptr;
        if(reconfigure)old->tiles=std::move(tiles);
        else if(old->tiles&&v[JET_TEX_offset]!=old->values[JET_TEX_offset])tile_palette(*old->tiles,*old,v);
        texture_settings(*old->value,v);std::memcpy(old->values,v,sizeof(v));
        bind_tiles(*old->value,old->tiles.get(),v);
        if(q.fields&(1U<<JET_TEX_name)) std::memcpy(old->name,q.name,sizeof(old->name));
        r.handle=old->id;return 0;
    }
    auto entry=std::make_unique<TextureResource>();
    entry->width=width;entry->height=height;
    if(old) {entry->pixels=old->pixels;entry->palette=old->palette;}
    if(replace) {
        entry->pixels.resize(format==2 ? (count+1)/2 : count);
        auto *bytes=static_cast<const uint8_t *>(q.data);
        if(format==2) std::memcpy(entry->pixels.data(),bytes,count);
        else for(size_t i=0;i<count;++i) entry->pixels[i]=format==0 ? uint16_t(bytes[i*2]*256U+bytes[i*2+1]) :
            rgb565(uint32_t(bytes[i*3])*65536U+uint32_t(bytes[i*3+1])*256U+bytes[i*3+2]);
    }
    if(palette_replace) {
        entry->palette.resize(q.extra_bytes/4);
        for(size_t i=0;i<entry->palette.size();++i) {
            uint32_t color;std::memcpy(&color,static_cast<const uint8_t *>(q.extra)+i*4,4);
            if(color>0xffffff) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            entry->palette[i]=rgb565(color);
        }
    }
    if(format==2) {
        auto *indices=reinterpret_cast<const uint8_t *>(entry->pixels.data());
        for(size_t i=0;i<count;++i) if(indices[i]>=entry->palette.size()) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    entry->value=std::make_unique<Texture>(width,height,entry->pixels.data());
    entry->tiles=prepare_tiles(*entry,v);
    auto &tex=*entry->value;
    tex.name=entry->name;
    tex.palette=entry->palette.empty() ? nullptr : entry->palette.data();tex.paletteSize=entry->palette.size();
    texture_settings(tex,v);
    bind_tiles(tex,entry->tiles.get(),v);
    std::memcpy(entry->values,v,sizeof(v));
    if(old) {
        old->pixels.swap(entry->pixels);old->palette.swap(entry->palette);old->tiles=std::move(entry->tiles);*old->value=tex;
        old->value->name=old->name;
        std::memcpy(old->values,v,sizeof(v));
        if(q.fields&(1U<<JET_TEX_name)) std::memcpy(old->name,q.name,sizeof(old->name));
        r.handle=old->id;return 0;
    }
    return store(active->textures,std::move(entry),q,r);
}
static int remove_surface(uint32_t id) {
    if(auto *entry=resource(active->shaders,id)) {
        for(auto &mat:active->materials)if(mat&&mat->value.shader==entry)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        for(auto &slot:active->shaders)if(slot.get()==entry) {slot.reset();return 0;}
    }
    if(auto *entry=resource(active->materials,id)) {
        for(auto &flare:active->flares)if(flare)for(unsigned i=0;i<flare->count;++i)
            if(flare->elements[i].values[JET_ELEM_material]==id)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        for(auto &mesh:active->prototypes)if(mesh)for(auto &face:mesh->value->triangles)
            if(face.material==&entry->value)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        for(auto &item:active->items)if(item)for(auto &instance:item->object->instances) {
            if(instance.materialOverride==&entry->value)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
            if(instance.triangleMaterials)for(auto *mat:*instance.triangleMaterials)
                if(mat==&entry->value)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        }
        for(auto &sprite:active->sprites) if(sprite && sprite->value.material==&entry->value)
            return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        for(auto &item:active->items) if(item) for(auto &face:item->object->triangles)
            if(face.material==&entry->value) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        for(auto &slot:active->materials) if(slot.get()==entry) {slot.reset();return 0;}
    }
    if(auto *entry=resource(active->textures,id)) {
        for(auto &mat:active->materials) if(mat && mat->value.diffuseMap==entry->value.get())
            return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        for(auto &slot:active->textures) if(slot.get()==entry) {slot.reset();return 0;}
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int mesh_data(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    size_t n=q.bytes/sizeof(solar_os_jet_mesh_vertex_t),t=q.extra_bytes/sizeof(solar_os_jet_mesh_face_t);
    if(!q.data || !q.extra || uintptr_t(q.data)%alignof(solar_os_jet_mesh_vertex_t) ||
        uintptr_t(q.extra)%alignof(solar_os_jet_mesh_face_t) ||
        q.bytes%sizeof(solar_os_jet_mesh_vertex_t) || q.extra_bytes%sizeof(solar_os_jet_mesh_face_t) ||
        !n || n>SOLAR_OS_JET_MAX_VERTICES || !t || t>SOLAR_OS_JET_MAX_TRIANGLES ||
        q.fields&~SOLAR_OS_JET_MESH_NORMALS) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    auto *vertices=static_cast<const solar_os_jet_mesh_vertex_t *>(q.data);
    auto *faces=static_cast<const solar_os_jet_mesh_face_t *>(q.extra);
    auto item=std::make_unique<Item>(0xffffff);item->object=std::make_unique<Object>();
    Item *existing=q.handle ? find(q.handle) : nullptr;
    if(q.handle && !existing) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(existing && existing->object->triangleCount()-existing->object->triangles.size()+t>SOLAR_OS_JET_MAX_TRIANGLES)
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    auto &obj=*item->object;obj.vertices.reserve(t*3);obj.triangles.reserve(t);
    for(size_t i=0;i<n;++i) {
        auto &v=vertices[i];
        if(!coordinate(v.x)||!coordinate(v.y)||!coordinate(v.z)||v.u<-32768||v.u>32768||v.v<-32768||v.v>32768||
            v.nx<-1024||v.nx>1024||v.ny<-1024||v.ny>1024||v.nz<-1024||v.nz>1024)
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    for(size_t i=0;i<t;++i) {
        auto &f=faces[i];if(f.a>=n||f.b>=n||f.c>=n) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        auto *mat=material(f.material,&item->material);if(!mat) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        for(auto index:{f.a,f.b,f.c}) {
            auto &v=vertices[index];obj.addVertex({{v.x,v.y,v.z},{v.u,v.v},{v.nx,v.ny,v.nz}});
        }
        obj.addTriangle(i*3,i*3+1,i*3+2,mat);
    }
    if(!(q.fields&SOLAR_OS_JET_MESH_NORMALS)) obj.computeFlatNormals();
    obj.calculateBoundingBox();
    if(existing) {
        for(auto &face:obj.triangles) if(face.material==&item->material) face.material=&existing->material;
        existing->object->vertices.swap(obj.vertices);existing->object->triangles.swap(obj.triangles);
        existing->object->calculateBoundingBox();r.handle=existing->id;return 0;
    }
    return admit(std::move(item),&r.handle);
}
static int execute(void *s,const solar_os_jet_command_t *command,solar_os_jet_reply_t *reply) {
    if(!s || s!=active) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if(!command || !reply || !std::memchr(command->name,0,sizeof(command->name))) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return call([&] {
        const auto &q=*command;auto &r=*reply;r={};
        switch(q.opcode) {
        case SOLAR_OS_JET_INFO: {
            r.handle=SOLAR_OS_JET_FEATURE_MATERIALS|SOLAR_OS_JET_FEATURE_TEXTURES|SOLAR_OS_JET_FEATURE_GEOMETRY|SOLAR_OS_JET_FEATURE_SCENE|SOLAR_OS_JET_FEATURE_SPRITES|SOLAR_OS_JET_FEATURE_PICKING|SOLAR_OS_JET_FEATURE_ASSETS|SOLAR_OS_JET_FEATURE_INSTANCING|SOLAR_OS_JET_FEATURE_PARTICLES|SOLAR_OS_JET_FEATURE_FLARES|SOLAR_OS_JET_FEATURE_POSTFX|SOLAR_OS_JET_FEATURE_TILED_TEXTURES|SOLAR_OS_JET_FEATURE_SHADERS;
            int objects,triangles,vertices;active->scene.getStatistics(objects,triangles,vertices);
            r.handle|=SOLAR_OS_JET_FEATURE_RENDERER_CONTROLS;
            r.handle|=SOLAR_OS_JET_FEATURE_BACKGROUND_CACHE;
            r.values[0]=objects;r.values[1]=triangles;r.values[2]=vertices;
            r.values[3]=SOLAR_OS_JET_MAX_MATERIALS;r.values[4]=SOLAR_OS_JET_MAX_TEXTURES;
            r.values[5]=SOLAR_OS_JET_MAX_LIGHTS;r.values[6]=SOLAR_OS_JET_MAX_OBJECTS;
            r.values[7]=SOLAR_OS_JET_MAX_SPRITES;r.values[8]=SOLAR_OS_JET_MAX_PICKS;
            r.values[9]=SOLAR_OS_JET_MAX_PROTOTYPES;r.values[10]=SOLAR_OS_JET_MAX_INSTANCES;
            r.values[11]=SOLAR_OS_JET_MAX_PARTICLES;r.values[12]=SOLAR_OS_JET_MAX_FLARES;r.values[13]=SOLAR_OS_JET_MAX_FLARE_ELEMENTS;r.values[14]=SOLAR_OS_JET_MAX_SHADERS;return 0;
        }
        case SOLAR_OS_JET_MATERIAL:case SOLAR_OS_JET_MATERIAL_SET:case SOLAR_OS_JET_MATERIAL_INFO:
            return material_command(q,r);
        case SOLAR_OS_JET_TEXTURE:case SOLAR_OS_JET_TEXTURE_SET:case SOLAR_OS_JET_TEXTURE_INFO:
            return texture_command(q,r);
        case SOLAR_OS_JET_ASSIGN: {
            auto *item=find(q.handle);if(!item) return int(SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
            if(!std::isfinite(q.values[0]) || q.values[0]<0 || q.values[0]>UINT32_MAX || std::floor(q.values[0])!=q.values[0])
                return int(SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
            auto *mat=material(uint32_t(q.values[0]),&item->material);if(!mat) return int(SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
            for(auto &face:item->object->triangles) {face.material=mat;face.colorBaked=false;}
            for(size_t i=0;i<item->object->instances.size();++i) {
                auto &instance=item->object->instances[i];instance.materialOverride=q.values[0] ? mat : nullptr;
                instance.triangleMaterials.reset();item->instance_poses[i].values[JET_INST_material]=q.values[0];
            }
            return 0;
        }
        case SOLAR_OS_JET_MESH_DATA:return mesh_data(q,r);
        case SOLAR_OS_JET_SHADER:case SOLAR_OS_JET_SHADER_SET:case SOLAR_OS_JET_SHADER_INFO:return shader_command(q,r);
        default:return scene_command(q,r);
        }
    });
}
/* Definition follows serialized<> in engine.cpp. */
