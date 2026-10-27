uint16_t ShaderResource::getPixel(uint32_t raw_u,uint32_t raw_v,ShaderParameters *params) {
    if(failed)return 0;
    ++calls;
    int32_t u=int32_t(raw_u),v=int32_t(raw_v);
    switch(int(values[JET_SHADER_kind])) {
    case 0: {
        const double factor=screenSpace ? 1.0/values[JET_SHADER_scale] : values[JET_SHADER_scale]/1024.0;
        const int64_t x=int64_t(std::floor(u*factor)),y=int64_t(std::floor(v*factor));
        return rgb565(uint32_t(values[((x+y)&1) ? JET_SHADER_secondary : JET_SHADER_color]));
    }
    case 1:return rgb565(((uint32_t(u)&1023)*255/1023<<16)|((uint32_t(v)&1023)*255/1023<<8));
    case 2: {
        unsigned value=std::min(255U,unsigned(std::max<int32_t>(0,params->zBuffer))*255/unsigned(values[JET_SHADER_scale]));
        return rgb565(value*0x010101U);
    }
    default: {
        if(active->frame_cancel&&active->frame_cancel(active->frame_cancel_user)) {failed=true;return 0;}
        solar_os_jet_shader_parameters_t p={params->screenPos.x,params->screenPos.y,params->zBuffer,
            {params->localCoords.x,params->localCoords.y,params->localCoords.z},
            {params->worldCoords.x,params->worldCoords.y,params->worldCoords.z},params->frameNumber};
        uint32_t color=0;
        int error=callback.function(callback.user,callback.slot,screenSpace ? u : u/1024.0,screenSpace ? v : v/1024.0,&p,&color);
        if(error||color>0xffffff) {failed=true;active->shader_fault=true;return 0;}
        return rgb565(color);
    }
    }
}
static int shader_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    auto *old=q.opcode==SOLAR_OS_JET_SHADER ? nullptr : resource(active->shaders,q.handle);
    if(q.opcode!=SOLAR_OS_JET_SHADER&&!old)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.opcode==SOLAR_OS_JET_SHADER_INFO) {surface_reply(*old,r);r.values[16]=old->calls;r.values[17]=old->failed;return 0;}
    if(!valid_values(q,jet_shader_fields,JET_SHADER_COUNT)||duplicate_name(active->shaders,q.name,q.handle))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_shader_fields,JET_SHADER_COUNT,old ? old->values : nullptr);
    solar_os_jet_shader_callback_t callback=old ? old->callback : solar_os_jet_shader_callback_t{};
    if(q.data||q.bytes) {
        if(!q.data||q.bytes!=sizeof(callback))return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        std::memcpy(&callback,q.data,sizeof(callback));
        if(!callback.function||callback.slot>=SOLAR_OS_JET_MAX_SHADERS)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if(v[JET_SHADER_kind]==3&&!callback.function)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if(v[JET_SHADER_kind]!=3)callback={};
    auto fresh=old ? nullptr : std::make_unique<ShaderResource>();auto *entry=old ? old : fresh.get();
    entry->callback=callback;std::memcpy(entry->values,v,sizeof(v));
    entry->screenSpace=v[JET_SHADER_screen_space];entry->hasAlpha=v[JET_SHADER_alpha_key]>=0;
    entry->needsCoordinates=v[JET_SHADER_kind]==3;
    entry->alphaColor=entry->hasAlpha ? rgb565(uint32_t(v[JET_SHADER_alpha_key])) : 0;
    entry->failed=false;entry->calls=0;
    if(old) {if(q.fields&(1U<<JET_SHADER_name))std::memcpy(old->name,q.name,sizeof(old->name));r.handle=old->id;return 0;}
    return store(active->shaders,std::move(fresh),q,r);
}
