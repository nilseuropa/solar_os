static uint32_t instance_material_id(const Material *mat) {
    for(auto &entry:active->materials)if(entry && &entry->value==mat)return entry->id;
    return 0;
}
static void refresh_instance_poses(Item &item) {
    constexpr double degrees=180.0/3.14159265358979323846;
    for(size_t i=0;i<item.instance_poses.size();++i) {
        auto &t=item.object->instances[i].transform;auto *v=item.instance_poses[i].values;const float *m=t.basis;
        v[JET_INST_x]=t.position.x;v[JET_INST_y]=t.position.y;v[JET_INST_z]=t.position.z;
        double y=std::asin(std::clamp(-double(m[6]),-1.0,1.0)),x,z;
        if(std::abs(std::cos(y))>.001) {x=std::atan2(double(m[7]),double(m[8]));z=std::atan2(double(m[3]),double(m[0]));}
        else {x=std::atan2(-double(m[5]),double(m[4]));z=0;}
        v[JET_INST_rx]=std::round(x*degrees);v[JET_INST_ry]=std::round(y*degrees);v[JET_INST_rz]=std::round(z*degrees);
    }
}
static int prototype_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(q.opcode==SOLAR_OS_JET_PROTOTYPE_INFO) {
        auto *entry=resource(active->prototypes,q.handle);if(!entry)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        surface_reply(*entry,r);const auto &o=*entry->value;r.count=o.vertices.size();r.handles[0]=o.triangles.size();
        r.handles[1]=o.boundingBoxMin.x;r.handles[2]=o.boundingBoxMin.y;r.handles[3]=o.boundingBoxMin.z;
        r.handles[4]=o.boundingBoxMax.x;r.handles[5]=o.boundingBoxMax.y;r.handles[6]=o.boundingBoxMax.z;return 0;
    }
    Item *source=find(q.handle);if(!source)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.fields&~1U||duplicate_name(active->prototypes,q.name,0))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if(!source->object->instances.empty())return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if(source->object->triangles.empty())return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    auto entry=std::make_unique<MeshResource>();entry->fallback=source->material;
    entry->face_materials.resize(source->face_materials.size());Object copy=*source->object;
    copy.lodMeshes.clear();copy.setPosition(0,0,0);copy.setRotation(0,0,0);copy.scale={1024,1024,1024};copy.transformScale=false;
    for(auto &face:copy.triangles) {
        if(face.material==&source->material)face.material=&entry->fallback;
        for(size_t i=0;i<source->face_materials.size();++i)if(source->face_materials[i]&&face.material==source->face_materials[i].get()) {
            if(!entry->face_materials[i])entry->face_materials[i]=std::make_unique<Material>(*source->face_materials[i]);
            face.material=entry->face_materials[i].get();
        }
    }
    entry->value=Object::freezeMesh(std::move(copy));if(!entry->value)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return store(active->prototypes,std::move(entry),q,r);
}
static int remove_prototype(uint32_t id) {
    for(auto &slot:active->prototypes)if(slot&&slot->id==id) {
        for(auto &item:active->items)if(item)for(auto &pose:item->instance_poses)
            if(pose.prototype==id)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        slot.reset();return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static bool instance_bounds(const Object &mesh,const Object::InstanceTransform &t) {
    const auto &lo=mesh.boundingBoxMin,&hi=mesh.boundingBoxMax;const float *m=t.basis;
    for(unsigned corner=0;corner<8;++corner) {
        float x=(corner&1)?hi.x:lo.x,y=(corner&2)?hi.y:lo.y,z=(corner&4)?hi.z:lo.z;
        float p[]={m[0]*x+m[1]*y+m[2]*z+t.position.x,m[3]*x+m[4]*y+m[5]*z+t.position.y,m[6]*x+m[7]*y+m[8]*z+t.position.z};
        for(float value:p)if(!std::isfinite(value)||value<-4096||value>4096)return false;
    }
    return true;
}
static int instance_edit(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(!valid_values(q,jet_instance_fields,JET_INST_COUNT))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    bool create=q.opcode==SOLAR_OS_JET_INSTANCE,update=q.opcode==SOLAR_OS_JET_INSTANCE_SET;
    auto fresh=create ? std::make_unique<Item>(0xffffff) : nullptr;
    if(create)fresh->object=std::make_unique<Object>();
    Item *item=create ? fresh.get() : find(q.handle);if(!item)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    auto &object=*item->object;size_t index=object.instances.size();
    if(update) {
        if(!std::isfinite(q.values[22])||q.values[22]<0||q.values[22]>=index||q.values[22]!=std::floor(q.values[22]))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        index=q.values[22];
    } else if(index==SOLAR_OS_JET_MAX_INSTANCES)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if(!update&&(!std::isfinite(q.values[16])||q.values[16]<1||q.values[16]>UINT32_MAX||q.values[16]!=std::floor(q.values[16])))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    uint32_t prototype=update ? item->instance_poses[index].prototype : uint32_t(q.values[16]);
    auto *mesh=resource(active->prototypes,prototype);if(!mesh)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    Object::MeshInstance next=update ? object.instances[index] : Object::MeshInstance{};
    if(!update)next.mesh=mesh->value;
    if(!update&&object.triangleCount()+next.mesh->triangles.size()>SOLAR_OS_JET_MAX_TRIANGLES)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    InstancePose pose;pose.prototype=prototype;
    merge_values(pose.values,q,jet_instance_fields,JET_INST_COUNT,update ? item->instance_poses[index].values : nullptr);
    auto *v=pose.values;
    const uint32_t rotation=(1U<<JET_INST_rx)|(1U<<JET_INST_ry)|(1U<<JET_INST_rz);
    if(!update||(q.fields&rotation))next.transform=Object::InstanceTransform::rotated({int32_t(v[JET_INST_rx]),int32_t(v[JET_INST_ry]),int32_t(v[JET_INST_rz])});
    next.transform.position={int32_t(v[JET_INST_x]),int32_t(v[JET_INST_y]),int32_t(v[JET_INST_z])};
    if(!instance_bounds(*next.mesh,next.transform))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    next.materialOverride=material(uint32_t(v[JET_INST_material]));
    if(v[JET_INST_material]&&!next.materialOverride)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.fields&(1U<<JET_INST_materials)) {
        if(q.extra_bytes%4||(q.extra_bytes&&!q.extra)||q.extra_bytes/4>SOLAR_OS_JET_MAX_TRIANGLES)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        size_t count=q.extra_bytes/4;if(count&&count!=next.mesh->triangles.size())return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        if(!count)next.triangleMaterials.reset();
        else {
            auto table=std::make_shared<JetVector<Material*>>();table->reserve(count);
            for(size_t i=0;i<count;++i) {
                uint32_t id;std::memcpy(&id,static_cast<const uint8_t *>(q.extra)+i*4,4);Material *mat=material(id);
                if(id&&!mat)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
                table->push_back(mat);
            }
            next.triangleMaterials=std::move(table);
        }
    }
    if(update) {object.instances[index]=std::move(next);item->instance_poses[index]=pose;}
    else {
        // Reserve both arrays before publishing either logical entry.
        object.instances.reserve(index+1);item->instance_poses.reserve(index+1);
        object.instances.push_back(std::move(next));item->instance_poses.push_back(pose);
    }
    object.calculateBoundingBox();r.count=index;
    if(create)return admit(std::move(fresh),&r.handle);
    r.handle=item->id;return 0;
}
static int instance_info(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    Item *item=find(q.handle);if(!item)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    size_t count=item->object->instances.size();double index=q.values[22],page=q.values[23];
    if(!std::isfinite(index)||index<0||index>=count||index!=std::floor(index)||
        !std::isfinite(page)||page<0||page>64||page!=std::floor(page))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const auto &entry=item->object->instances[size_t(index)];const auto &pose=item->instance_poses[size_t(index)];
    r.handle=item->id;r.count=count;std::memcpy(r.values,pose.values,sizeof(pose.values));
    r.values[8]=pose.prototype;r.values[9]=entry.triangleMaterials ? entry.triangleMaterials->size() : 0;
    auto *prototype=resource(active->prototypes,pose.prototype);r.values[10]=!prototype||entry.mesh!=prototype->value;
    for(unsigned i=0;i<9;++i)r.values[11+i]=entry.transform.basis[i];
    if(entry.triangleMaterials)for(size_t i=size_t(page)*32;i<std::min(size_t(page)*32+32,entry.triangleMaterials->size());++i)
        r.handles[i%32]=instance_material_id((*entry.triangleMaterials)[i]);
    return 0;
}
static int instance_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    switch(q.opcode) {
    case SOLAR_OS_JET_PROTOTYPE:case SOLAR_OS_JET_PROTOTYPE_INFO:return prototype_command(q,r);
    case SOLAR_OS_JET_INSTANCE:case SOLAR_OS_JET_INSTANCE_ADD:case SOLAR_OS_JET_INSTANCE_SET:return instance_edit(q,r);
    case SOLAR_OS_JET_INSTANCE_INFO:return instance_info(q,r);
    case SOLAR_OS_JET_INSTANCE_REMOVE:case SOLAR_OS_JET_INSTANCE_CLEAR: {
        Item *item=find(q.handle);if(!item)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        auto &instances=item->object->instances;
        if(q.opcode==SOLAR_OS_JET_INSTANCE_CLEAR) {instances.clear();item->instance_poses.clear();}
        else {
            double index=q.values[22];if(!std::isfinite(index)||index<0||index>=instances.size()||index!=std::floor(index))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            instances.erase(instances.begin()+size_t(index));item->instance_poses.erase(item->instance_poses.begin()+size_t(index));
        }
        item->object->calculateBoundingBox();return 0;
    }
    default:return extras_command(q,r);
    }
}
