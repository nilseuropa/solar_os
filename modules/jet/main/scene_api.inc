static bool object_name_used(const char *name,uint32_t except=0) {
    if(!*name) return false;
    for(auto &item:active->items) if(item && item->id!=except && !std::strcmp(item->name,name)) return true;
    return false;
}
static int primitive(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(!valid_values(q,jet_primitive_fields,JET_PRIM_COUNT) || object_name_used(q.name)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_primitive_fields,JET_PRIM_COUNT);
    auto item=std::make_unique<Item>(uint32_t(v[JET_PRIM_color]));
    Material *mat=material(uint32_t(v[JET_PRIM_material]),&item->material);
    Material *mat2=material(uint32_t(v[JET_PRIM_material2]),mat);
    if(!mat || !mat2) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int width=v[JET_PRIM_width],height=v[JET_PRIM_height],depth=v[JET_PRIM_depth],radius=v[JET_PRIM_radius],segments=v[JET_PRIM_segments];
    switch(int(v[JET_PRIM_kind])) {
    case 0:case 9:item->object.reset(Primitives::createCube(width,height,depth,mat));break;
    case 1:item->object.reset(Primitives::createGrid(width,height,v[JET_PRIM_rows],v[JET_PRIM_cols],mat,mat2,v[JET_PRIM_per_cell_uv]));break;
    case 2:item->object.reset(Primitives::createPlane(width,height,mat));break;
    case 3:item->object.reset(Primitives::createPyramid(width,height,mat));break;
    case 4:item->object.reset(Primitives::createSphere(radius,segments,mat,v[JET_PRIM_u_scale],v[JET_PRIM_v_scale]));break;
    case 5:
        if(height<radius*2) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        item->object.reset(Primitives::createCapsule(radius,height-radius*2,segments,mat));break;
    case 6:item->object.reset(Primitives::createCylinder(radius,height,segments,v[JET_PRIM_caps],mat));break;
    case 7:item->object.reset(Primitives::createBillboard(width,height,mat));break;
    case 8:item->object.reset(Primitives::createQuad(width,height,mat));break;
    }
    if(!item->object || item->object->triangles.size()>SOLAR_OS_JET_MAX_TRIANGLES || item->object->vertices.size()>8192)
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if(v[JET_PRIM_kind]==9) {
        const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffe0,0xf81f,0x07ff};
        item->face_materials.resize(6);
        for(unsigned i=0;i<6;++i) {
            item->face_materials[i]=std::make_unique<Material>(colors[i]);
            item->face_materials[i]->name=nullptr;
            for(unsigned t=i*2;t<i*2+2;++t) item->object->triangles[t].material=item->face_materials[i].get();
        }
    }
    std::memcpy(item->name,q.name,sizeof(item->name));return admit(std::move(item),&r.handle);
}
static void object_values(const Item &item,double *v) {
    auto &o=*item.object;
    v[JET_OBJ_x]=o.position.x;v[JET_OBJ_y]=o.position.y;v[JET_OBJ_z]=o.position.z;
    v[JET_OBJ_rx]=o.rotation.x;v[JET_OBJ_ry]=o.rotation.y;v[JET_OBJ_rz]=o.rotation.z;
    v[JET_OBJ_scale_x]=double(o.scale.x)/1024;v[JET_OBJ_scale_y]=double(o.scale.y)/1024;v[JET_OBJ_scale_z]=double(o.scale.z)/1024;
    v[JET_OBJ_enabled]=o.enabled;v[JET_OBJ_culling]=int(o.cullingMode);v[JET_OBJ_blend]=int(o.blendMode);v[JET_OBJ_billboard]=o.isBillboard;
    v[JET_OBJ_depth_test]=!o.ignoreZBuffer;v[JET_OBJ_depth_write]=!o.noWriteZBuffer;v[JET_OBJ_depth_bias]=o.zBias;
    v[JET_OBJ_precise_sort]=o.preciseDepthSort;v[JET_OBJ_fade_near]=o.fadeNear;v[JET_OBJ_fade_far]=o.fadeFar;
    v[JET_OBJ_appear_near]=o.appearNear;v[JET_OBJ_appear_far]=o.appearFar;v[JET_OBJ_lod_bias]=o.lodBias;v[JET_OBJ_lod_persist]=o.lodPersist;
}
static int object_control(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    Item *item=find(q.handle);if(!item) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    Object &o=*item->object;r.handle=item->id;
    double old[SOLAR_OS_JET_VALUES]={0};object_values(*item,old);
    if(q.opcode==SOLAR_OS_JET_OBJECT_INFO) {
        std::memcpy(r.values,old,sizeof(old));std::memcpy(r.name,item->name,sizeof(r.name));
        r.count=o.vertexCount();r.handles[0]=o.triangleCount();
        r.handles[1]=o.boundingBoxMin.x;r.handles[2]=o.boundingBoxMin.y;r.handles[3]=o.boundingBoxMin.z;
        r.handles[4]=o.boundingBoxMax.x;r.handles[5]=o.boundingBoxMax.y;r.handles[6]=o.boundingBoxMax.z;
        r.handles[7]=o.lodMeshes.size();r.handles[8]=o.instances.size();return 0;
    }
    if(!valid_values(q,jet_object_fields,JET_OBJ_COUNT) || object_name_used(q.name,q.handle)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_object_fields,JET_OBJ_COUNT,old);
    if((v[JET_OBJ_fade_far] || v[JET_OBJ_fade_near]) && v[JET_OBJ_fade_far]<=v[JET_OBJ_fade_near]) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if((v[JET_OBJ_appear_far] || v[JET_OBJ_appear_near]) && v[JET_OBJ_appear_far]<=v[JET_OBJ_appear_near]) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    o.setPosition(v[JET_OBJ_x],v[JET_OBJ_y],v[JET_OBJ_z]);o.setRotation(int32_t(v[JET_OBJ_rx]),int32_t(v[JET_OBJ_ry]),int32_t(v[JET_OBJ_rz]));
    o.scale={int32_t(v[JET_OBJ_scale_x]*1024),int32_t(v[JET_OBJ_scale_y]*1024),int32_t(v[JET_OBJ_scale_z]*1024)};
    o.transformScale=o.scale.x!=1024 || o.scale.y!=1024 || o.scale.z!=1024;
    o.enabled=v[JET_OBJ_enabled];o.cullingMode=CullingMode(int(v[JET_OBJ_culling]));o.blendMode=BlendMode(int(v[JET_OBJ_blend]));
    o.isBillboard=v[JET_OBJ_billboard];o.ignoreZBuffer=!v[JET_OBJ_depth_test];o.noWriteZBuffer=!v[JET_OBJ_depth_write];o.zBias=v[JET_OBJ_depth_bias];
    o.preciseDepthSort=v[JET_OBJ_precise_sort];o.fadeNear=v[JET_OBJ_fade_near];o.fadeFar=v[JET_OBJ_fade_far];
    o.appearNear=v[JET_OBJ_appear_near];o.appearFar=v[JET_OBJ_appear_far];o.lodBias=v[JET_OBJ_lod_bias];o.lodPersist=v[JET_OBJ_lod_persist];
    if(q.fields&(1U<<JET_OBJ_name)) std::memcpy(item->name,q.name,sizeof(item->name));
    return 0;
}
static bool bounded_mesh(const Object &o) {
    for(auto &v:o.vertices) if(!coordinate(v.position.x)||!coordinate(v.position.y)||!coordinate(v.position.z)) return false;
    return coordinate(o.boundingBoxMin.x)&&coordinate(o.boundingBoxMin.y)&&coordinate(o.boundingBoxMin.z)&&
        coordinate(o.boundingBoxMax.x)&&coordinate(o.boundingBoxMax.y)&&coordinate(o.boundingBoxMax.z);
}
static int object_action(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(!valid_values(q,jet_object_action_fields,JET_OACT_COUNT)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    Item *item=find(q.handle);if(!item) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    auto &o=*item->object;r.handle=item->id;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_object_action_fields,JET_OACT_COUNT);
    Vector3 delta{int32_t(v[JET_OACT_x]),int32_t(v[JET_OACT_y]),int32_t(v[JET_OACT_z])};
    switch(int(v[JET_OACT_action])) {
    case 0:
        if(!coordinate(o.position.x+delta.x)||!coordinate(o.position.y+delta.y)||!coordinate(o.position.z+delta.z)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        o.translate(delta.x,delta.y,delta.z);return 0;
    case 1:o.rotate(delta);return 0;
    case 2:o.lookAt(delta);return 0;
    case 3:case 4: {
        if(v[JET_OACT_action]==4) for(auto &p:o.vertices) for(auto n:{p.position.x,p.position.y,p.position.z})
            if(!coordinate(int32_t(int64_t(n)*int64_t(v[JET_OACT_numerator])/int64_t(v[JET_OACT_denominator]))))
                return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        Object changed=o;
        if(v[JET_OACT_action]==3) changed.bakeRotation();else changed.bakeScale(v[JET_OACT_numerator],v[JET_OACT_denominator]);
        if(!bounded_mesh(changed)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        o=std::move(changed);refresh_instance_poses(*item);return 0;
    }
    case 5:return o.cachePositions() ? 0 : SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case 6:o.invalidatePositions();return 0;
    case 7: {
        auto copy=std::make_unique<Item>(0xffffff);copy->material=item->material;copy->object=std::make_unique<Object>(o);
        copy->instance_poses=item->instance_poses;
        copy->face_materials.resize(item->face_materials.size());
        for(auto &face:copy->object->triangles) {
            if(face.material==&item->material) face.material=&copy->material;
            for(unsigned i=0;i<item->face_materials.size();++i) if(item->face_materials[i] && face.material==item->face_materials[i].get()) {
                if(!copy->face_materials[i]) copy->face_materials[i]=std::make_unique<Material>(*item->face_materials[i]);
                face.material=copy->face_materials[i].get();
            }
        }
        return admit(std::move(copy),&r.handle);
    }
    case 8:o.bakeFlatLighting(&active->light,&active->ambient);return 0;
    case 9:for(auto &face:o.triangles) face.colorBaked=false;return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
static void camera_values(double *v) {
    auto &c=active->camera;v[JET_CAM_x]=c.position.x;v[JET_CAM_y]=c.position.y;v[JET_CAM_z]=c.position.z;
    v[JET_CAM_rx]=c.rotation.x;v[JET_CAM_ry]=c.rotation.y;v[JET_CAM_rz]=c.rotation.z;
    v[JET_CAM_fov]=active->fov;v[JET_CAM_near]=c.nearPlane;v[JET_CAM_far]=c.farPlane;
}
static int camera_control(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(q.opcode==SOLAR_OS_JET_CAMERA_INFO) {camera_values(r.values);return 0;}
    Camera next=active->camera;
    if(q.opcode==SOLAR_OS_JET_CAMERA_SET) {
        if(!valid_values(q,jet_camera_fields,JET_CAM_COUNT)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        double old[SOLAR_OS_JET_VALUES]={0},v[SOLAR_OS_JET_VALUES]={0};camera_values(old);merge_values(v,q,jet_camera_fields,JET_CAM_COUNT,old);
        if(v[JET_CAM_near]>=v[JET_CAM_far]) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        next.setPosition(v[JET_CAM_x],v[JET_CAM_y],v[JET_CAM_z]);next.setRotation(int32_t(v[JET_CAM_rx]),int32_t(v[JET_CAM_ry]),int32_t(v[JET_CAM_rz]));
        next.setFOV(int32_t(v[JET_CAM_fov]),int32_t(active->width));next.nearPlane=v[JET_CAM_near];next.farPlane=v[JET_CAM_far];active->fov=v[JET_CAM_fov];
    } else {
        if(!valid_values(q,jet_camera_action_fields,JET_CACT_COUNT)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_camera_action_fields,JET_CACT_COUNT);
        int32_t x=v[JET_CACT_x],y=v[JET_CACT_y],z=v[JET_CACT_z];
        switch(int(v[JET_CACT_action])) {
        case 0:next.translate(x,y,z);break;case 1:next.translateLocal(x,y,z);break;case 2:next.translateLocalY(x,y,z);break;
        case 3:next.rotate(x,y,z);break;case 4:next.rotateLocal(x,y,z);break;
        case 5:next.lookAt(Vector3{x,y,z});break;case 6:next.lookAtX(Vector3{x,y,z});break;case 7:next.lookAtY(Vector3{x,y,z});break;
        }
        if(!coordinate(next.position.x)||!coordinate(next.position.y)||!coordinate(next.position.z)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    active->camera=next;return 0;
}
static int light_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    LightResource *old=q.opcode==SOLAR_OS_JET_POINT_LIGHT ? nullptr : resource(active->lights,q.handle);
    if(q.opcode!=SOLAR_OS_JET_POINT_LIGHT && !old) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.opcode==SOLAR_OS_JET_POINT_LIGHT_INFO) {surface_reply(*old,r);return 0;}
    if(!valid_values(q,jet_point_fields,JET_POINT_COUNT) || duplicate_name(active->lights,q.name,q.handle)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_point_fields,JET_POINT_COUNT,old ? old->values : nullptr);
    if(v[JET_POINT_range_min]>=v[JET_POINT_range_max]) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    auto fresh=old ? nullptr : std::make_unique<LightResource>();auto *entry=old ? old : fresh.get();
    entry->value.position={int32_t(v[JET_POINT_x]),int32_t(v[JET_POINT_y]),int32_t(v[JET_POINT_z])};
    entry->value.color=rgb(uint32_t(v[JET_POINT_color]));entry->value.rangeMin=v[JET_POINT_range_min];entry->value.rangeMax=v[JET_POINT_range_max];entry->value.enabled=v[JET_POINT_enabled];
    std::memcpy(entry->values,v,sizeof(v));
    if(q.fields&(1U<<JET_POINT_name)) std::memcpy(entry->name,q.name,sizeof(entry->name));
    if(old) {r.handle=old->id;return 0;}
    if(active->next_id==UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    for(auto &slot:active->lights) if(!slot) {
        active->scene.addPointLight(&entry->value);entry->id=++active->next_id;r.handle=entry->id;slot=std::move(fresh);return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
static int remove_light(uint32_t id) {
    for(auto &slot:active->lights) if(slot && slot->id==id) {
        auto &lights=active->scene.getPointLights();lights.erase(std::remove(lights.begin(),lights.end(),&slot->value),lights.end());slot.reset();return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static bool lod_reaches(Object *node,Object *target,unsigned depth) {
    if(node==target) return true;
    if(depth>=SOLAR_OS_JET_MAX_OBJECTS) return true;
    for(auto *lod:node->lodMeshes) if(lod_reaches(lod,target,depth+1)) return true;
    return false;
}
static int scene_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    switch(q.opcode) {
    case SOLAR_OS_JET_PRIMITIVE:return primitive(q,r);
    case SOLAR_OS_JET_OBJECT_SET:case SOLAR_OS_JET_OBJECT_INFO:return object_control(q,r);
    case SOLAR_OS_JET_OBJECT_ACTION:return object_action(q,r);
    case SOLAR_OS_JET_CAMERA_SET:case SOLAR_OS_JET_CAMERA_INFO:case SOLAR_OS_JET_CAMERA_ACTION:return camera_control(q,r);
    case SOLAR_OS_JET_POINT_LIGHT:case SOLAR_OS_JET_POINT_LIGHT_SET:case SOLAR_OS_JET_POINT_LIGHT_INFO:return light_command(q,r);
    case SOLAR_OS_JET_SCENE_INFO:std::memcpy(r.values,active->settings,sizeof(r.values));return 0;
    case SOLAR_OS_JET_SCENE_SET: {
        if(!valid_values(q,jet_scene_fields,JET_SCENE_COUNT)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_scene_fields,JET_SCENE_COUNT,active->settings);
        if(v[JET_SCENE_texture_lod]&&v[JET_SCENE_texture_far]<=v[JET_SCENE_texture_near])
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if(v[JET_SCENE_background_cache]&&(!v[JET_SCENE_clear]||v[JET_SCENE_wireframe]))
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if(q.fields&(1U<<JET_SCENE_background_cache)) {
            if(v[JET_SCENE_background_cache]) {
                const size_t pixels=size_t(active->width)*active->height;
                std::vector<uint16_t> color(pixels),depth(pixels);
                active->pending_color.swap(color);active->pending_depth.swap(depth);
                active->background_pending=true;
            } else {
                active->scene.setBackgroundCache(nullptr,nullptr);
                active->background_pending=false;
                std::vector<uint16_t>().swap(active->background_color);
                std::vector<uint16_t>().swap(active->background_depth);
                std::vector<uint16_t>().swap(active->pending_color);
                std::vector<uint16_t>().swap(active->pending_depth);
            }
        }
        std::memcpy(active->settings,v,sizeof(v));
        active->scene.setClearBuffer(active->settings[JET_SCENE_clear]);active->scene.lodScale=active->settings[JET_SCENE_lod_scale];
        active->scene.lodBias=active->settings[JET_SCENE_lod_bias];
        auto *raster=active->scene.getRenderer();raster->waterTime=v[JET_SCENE_water_time];
        raster->textureLodEnabled=v[JET_SCENE_texture_lod];raster->textureLodNear=v[JET_SCENE_texture_near];
        raster->textureLodFar=v[JET_SCENE_texture_far];raster->wireframeMode=v[JET_SCENE_wireframe];return 0;
    }
    case SOLAR_OS_JET_GRADIENT: {
        if(q.bytes%4 || q.bytes>1024*4 || (q.bytes && !q.data)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        size_t n=q.bytes/4;std::vector<uint32_t> stops(n);
        for(size_t i=0;i<n;++i) {std::memcpy(&stops[i],static_cast<const uint8_t *>(q.data)+i*4,4);if(stops[i]>0xffffff)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;}
        std::vector<uint16_t> gradient(n ? active->height : 0);
        for(size_t y=0;y<gradient.size();++y) {
            size_t scaled=active->height>1 ? y*(n-1)*1024/(active->height-1) : 0,index=scaled/1024;
            uint32_t a=stops[index],b=stops[std::min(index+1,n-1)],color=0;unsigned fraction=scaled%1024;
            for(unsigned shift:{0U,8U,16U})color|=(((((a>>shift)&255)*(1024-fraction)+((b>>shift)&255)*fraction)/1024)&255)<<shift;
            gradient[y]=rgb565(color);
        }
        active->gradient.swap(gradient);active->scene.backgroundGradientColors=n ? active->gradient.data() : nullptr;return 0;
    }
    case SOLAR_OS_JET_LOD: {
        Item *item=find(q.handle);if(!item)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        if(q.bytes%4 || q.bytes>8*4 || (q.bytes && !q.data))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        JetVector<Object*> levels;levels.reserve(q.bytes/4);
        for(size_t i=0;i<q.bytes/4;++i) {
            uint32_t id;std::memcpy(&id,static_cast<const uint8_t *>(q.data)+i*4,4);Item *lod=find(id);
            if(!lod)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
            if(lod_reaches(lod->object.get(),item->object.get(),0))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            levels.push_back(lod->object.get());
        }
        item->object->lodMeshes.swap(levels);return 0;
    }
    default:return overlay_command(q,r);
    }
}
