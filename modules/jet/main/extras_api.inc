static void extras_hook(Scene &,void *user) {
    auto *engine=static_cast<Engine *>(user);
    if(engine!=active||(engine->frame_cancel&&engine->frame_cancel(engine->frame_cancel_user)))return;
    if(engine->background_pending) {
        const size_t bytes=size_t(engine->width)*engine->height*sizeof(uint16_t);
        std::memcpy(engine->pending_color.data(),engine->frame_color,bytes);
        std::memcpy(engine->pending_depth.data(),engine->frame_depth,bytes);
        engine->background_captured=true;
    }
    auto *raster=engine->scene.getRenderer();int queries=raster->pickQueryCount;raster->pickQueryCount=0;
    for(auto &pool:engine->particles)if(pool) {
        if(pool->values[JET_PART_enabled])pool->value.render(&engine->scene,&engine->camera,engine->width,engine->height);
        else pool->value.lastRenderedTriangles=0;
    }
    raster->pickQueryCount=queries;
    process_postfx();
    for(auto &flare:engine->flares)if(flare&&flare->values[JET_FLARE_enabled])
        flare->value->update(engine->frame_dt,engine->width,engine->height);
}
static void extras_hook_state() {
    bool used=bool(active->effects)||active->background_pending;for(auto &pool:active->particles)used|=bool(pool);for(auto &flare:active->flares)used|=bool(flare);
    active->scene.setPostGeometryHook(used ? extras_hook : nullptr,used ? active : nullptr);
}
static void user_pick_queries() {
    PickQuery points[SOLAR_OS_JET_MAX_PICKS];
    for(unsigned i=0;i<active->query_count;++i)points[i]={active->query_points[i].x,active->query_points[i].y};
    active->scene.setPickQueries(points,active->query_count);
}
static void prepare_extras() {
    bool used=false;for(auto &flare:active->flares)used|=bool(flare);if(!used)return;
    PickQuery points[MAX_PICK_QUERIES];unsigned count=active->query_count;
    for(unsigned i=0;i<count;++i)points[i]={active->query_points[i].x,active->query_points[i].y};
    for(auto &flare:active->flares)if(flare) {
        if(!flare->values[JET_FLARE_enabled]) {
            for(unsigned i=0;i<flare->count;++i)flare->value->getSprite(i)->enabled=false;
            continue;
        }
        flare->value->setPickSlot(count);flare->value->prepare(&active->camera,active->width,active->height);
        points[count]=active->scene.getPickQueries()[count];++count;
    }
    active->scene.setPickQueries(points,count);
}
static int particle_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    auto *old=q.opcode==SOLAR_OS_JET_PARTICLES ? nullptr : resource(active->particles,q.handle);
    if(q.opcode!=SOLAR_OS_JET_PARTICLES&&!old)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.opcode==SOLAR_OS_JET_PARTICLES_INFO) {
        surface_reply(*old,r);r.values[16]=old->value.activeCount();r.values[17]=old->value.lastRenderedTriangles;
        r.values[18]=PARTICLE_POOL_SIZE;r.values[19]=old->value.randomSeed();return 0;
    }
    if(q.opcode==SOLAR_OS_JET_PARTICLES_CLEAR) {for(auto &p:old->value.pool)p={};old->value.lastRenderedTriangles=0;return 0;}
    if(q.opcode==SOLAR_OS_JET_PARTICLES_EMIT) {
        if(!valid_values(q,jet_emit_fields,JET_EMIT_COUNT))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_emit_fields,JET_EMIT_COUNT);
        Vec3f origin{float(v[JET_EMIT_x]),float(v[JET_EMIT_y]),float(v[JET_EMIT_z])};
        Vec3f velocity{float(v[JET_EMIT_vx]),float(v[JET_EMIT_vy]),float(v[JET_EMIT_vz])};
        unsigned before=old->value.activeCount();
        if(v[JET_EMIT_kind]==0)old->value.emitSparks(origin,{float(v[JET_EMIT_nx]),float(v[JET_EMIT_ny]),float(v[JET_EMIT_nz])},v[JET_EMIT_speed],v[JET_EMIT_count],velocity);
        else old->value.emitWaterSplash(origin,{float(v[JET_EMIT_tx]),float(v[JET_EMIT_ty]),float(v[JET_EMIT_tz])},
            {float(v[JET_EMIT_sx]),float(v[JET_EMIT_sy]),float(v[JET_EMIT_sz])},{float(v[JET_EMIT_ux]),float(v[JET_EMIT_uy]),float(v[JET_EMIT_uz])},v[JET_EMIT_speed],v[JET_EMIT_count],velocity);
        r.count=old->value.activeCount()-before;return 0;
    }
    if(!valid_values(q,jet_particle_fields,JET_PART_COUNT)||duplicate_name(active->particles,q.name,q.handle))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    auto fresh=old ? nullptr : std::make_unique<ParticleResource>();auto *entry=old ? old : fresh.get();
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_particle_fields,JET_PART_COUNT,old ? old->values : nullptr);
    entry->value.worldScale=v[JET_PART_world_scale];entry->value.additiveSparks=v[JET_PART_additive];
    if(!old||(q.fields&(1U<<JET_PART_seed)))entry->value.seed(uint32_t(v[JET_PART_seed]));
    std::memcpy(entry->values,v,sizeof(v));
    if(q.fields&(1U<<JET_PART_name))std::memcpy(entry->name,q.name,sizeof(entry->name));
    if(old) {r.handle=entry->id;return 0;}
    int err=store(active->particles,std::move(fresh),q,r);if(!err)extras_hook_state();return err;
}
static bool flare_element(const solar_os_jet_element_t &input,LensFlare::Element &out,solar_os_jet_element_t &stored,unsigned index) {
    solar_os_jet_command_t q={};q.fields=input.fields;std::memcpy(q.values,input.values,sizeof(q.values));
    if(!valid_values(q,jet_element_fields,JET_ELEM_COUNT))return false;
    merge_values(stored.values,q,jet_element_fields,JET_ELEM_COUNT);stored.fields=(1U<<JET_ELEM_COUNT)-1;
    if(!(input.fields&(1U<<JET_ELEM_z_order)))stored.values[JET_ELEM_z_order]=100+index;
    auto *v=stored.values;out.material=material(uint32_t(v[JET_ELEM_material]));
    if(v[JET_ELEM_material]&&!out.material)return false;
    if(out.material&&out.material->shader)return false;
    out.width=v[JET_ELEM_width];out.height=v[JET_ELEM_height];out.axisT=v[JET_ELEM_axis];out.baseAlpha=v[JET_ELEM_alpha];
    out.blendMode=v[JET_ELEM_blend] ? BlendMode::BLEND_ADD : BlendMode::BLEND_REPLACE;out.scale=v[JET_ELEM_scale];out.zOrder=v[JET_ELEM_z_order];
    out.textureFlags=(v[JET_ELEM_flip_x]?Sprite2D::FLIP_X:0)|(v[JET_ELEM_flip_y]?Sprite2D::FLIP_Y:0)|
        (v[JET_ELEM_mirror_x]?Sprite2D::MIRROR_X:0)|(v[JET_ELEM_mirror_y]?Sprite2D::MIRROR_Y:0);return true;
}
static int flare_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    auto *old=q.opcode==SOLAR_OS_JET_FLARE ? nullptr : resource(active->flares,q.handle);
    if(q.opcode!=SOLAR_OS_JET_FLARE&&!old)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.opcode==SOLAR_OS_JET_FLARE_INFO) {
        surface_reply(*old,r);r.values[16]=old->value->getVisibility();r.values[17]=old->value->isSunOnScreen();
        int x=old->value->projectedX(),y=old->value->projectedY();
        r.values[18]=r.values[17]&&x>=0&&y>=0&&x<active->width&&y<active->height;r.values[19]=x;r.values[20]=y;r.values[21]=old->count;return 0;
    }
    if(q.opcode==SOLAR_OS_JET_FLARE_ELEMENT_SET||q.opcode==SOLAR_OS_JET_FLARE_ELEMENT_INFO) {
        double index=q.values[22];if(!std::isfinite(index)||index<0||index>=old->count||index!=std::floor(index))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        auto &element=old->elements[unsigned(index)];
        if(q.opcode==SOLAR_OS_JET_FLARE_ELEMENT_INFO) {r.handle=old->id;r.count=old->count;std::memcpy(r.values,element.values,sizeof(r.values));return 0;}
        if(!valid_values(q,jet_element_fields,JET_ELEM_COUNT))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        solar_os_jet_element_t input={},staged;input.fields=(1U<<JET_ELEM_COUNT)-1;
        merge_values(input.values,q,jet_element_fields,JET_ELEM_COUNT,element.values);LensFlare::Element value;
        if(!flare_element(input,value,staged,unsigned(index)))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        old->value->setElement(unsigned(index),value);element=staged;return 0;
    }
    if(!valid_values(q,jet_flare_fields,JET_FLARE_COUNT)||duplicate_name(active->flares,q.name,q.handle))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_flare_fields,JET_FLARE_COUNT,old ? old->values : nullptr);
    double length=std::sqrt(v[JET_FLARE_x]*v[JET_FLARE_x]+v[JET_FLARE_y]*v[JET_FLARE_y]+v[JET_FLARE_z]*v[JET_FLARE_z]);
    if(!length)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    Vec3f direction{float(v[JET_FLARE_x]/length),float(v[JET_FLARE_y]/length),float(v[JET_FLARE_z]/length)};
    auto fresh=old ? nullptr : std::make_unique<FlareResource>();auto *entry=old ? old : fresh.get();
    if(!old) {
        if(!q.data||q.bytes%sizeof(solar_os_jet_element_t)||!q.bytes||q.bytes/sizeof(solar_os_jet_element_t)>SOLAR_OS_JET_MAX_FLARE_ELEMENTS)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        entry->count=q.bytes/sizeof(solar_os_jet_element_t);LensFlare::Element elements[SOLAR_OS_JET_MAX_FLARE_ELEMENTS];
        for(unsigned i=0;i<entry->count;++i) {
            solar_os_jet_element_t input;std::memcpy(&input,static_cast<const uint8_t *>(q.data)+i*sizeof(input),sizeof(input));
            if(!flare_element(input,elements[i],entry->elements[i],i))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
        bool free=false;for(auto &slot:active->flares)free|=!slot;
        if(!free)return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
        if(active->next_id==UINT32_MAX)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        size_t previous=active->scene.getSprites().size();
        try {entry->value=std::make_unique<LensFlare>(&active->scene,elements,entry->count,0,direction,v[JET_FLARE_fade_speed]);}
        catch(...) {active->scene.getSprites().resize(previous);throw;}
    }
    entry->value->setDirection(direction);entry->value->setFadeSpeed(v[JET_FLARE_fade_speed]);
    std::memcpy(entry->values,v,sizeof(v));
    if(q.fields&(1U<<JET_FLARE_name))std::memcpy(entry->name,q.name,sizeof(entry->name));
    if(old) {r.handle=entry->id;return 0;}
    int err=store(active->flares,std::move(fresh),q,r);if(!err)extras_hook_state();return err;
}
static int remove_extras(uint32_t id) {
    for(auto &slot:active->particles)if(slot&&slot->id==id) {slot.reset();extras_hook_state();return 0;}
    for(auto &slot:active->flares)if(slot&&slot->id==id) {
        auto &sprites=active->scene.getSprites();
        sprites.erase(std::remove_if(sprites.begin(),sprites.end(),[&](Sprite2D *sprite) {
            for(unsigned i=0;i<slot->count;++i)if(sprite==slot->value->getSprite(i))return true;
            return false;
        }),sprites.end());
        slot.reset();extras_hook_state();user_pick_queries();return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int extras_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    switch(q.opcode) {
    case SOLAR_OS_JET_PARTICLES:case SOLAR_OS_JET_PARTICLES_SET:case SOLAR_OS_JET_PARTICLES_INFO:
    case SOLAR_OS_JET_PARTICLES_EMIT:case SOLAR_OS_JET_PARTICLES_CLEAR:return particle_command(q,r);
    case SOLAR_OS_JET_FLARE:case SOLAR_OS_JET_FLARE_SET:case SOLAR_OS_JET_FLARE_INFO:
    case SOLAR_OS_JET_FLARE_ELEMENT_SET:case SOLAR_OS_JET_FLARE_ELEMENT_INFO:return flare_command(q,r);
    case SOLAR_OS_JET_STEP: {
        double dt=q.values[0];if(!std::isfinite(dt)||dt<0||dt>1)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;active->frame_dt=dt;
        active->settings[JET_SCENE_water_time]=std::fmod(active->settings[JET_SCENE_water_time]+dt,1000000.0);
        active->scene.getRenderer()->waterTime=active->settings[JET_SCENE_water_time];
        for(auto &pool:active->particles)if(pool&&pool->values[JET_PART_enabled]) {
            pool->value.update(dt);
            for(auto &p:pool->value.pool)if(p.active&&(!std::isfinite(p.pos.x)||!std::isfinite(p.pos.y)||!std::isfinite(p.pos.z)||
                std::abs(p.pos.x)>4096||std::abs(p.pos.y)>4096||std::abs(p.pos.z)>4096))p.active=false;
        }
        return 0;
    }
    default:return postfx_command(q,r);
    }
}
