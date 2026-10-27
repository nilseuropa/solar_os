static void process_postfx() {
    if(!active->effects||!active->frame_color)return;
    auto &fx=*active->effects;const auto *v=active->effect_values;auto *frame=active->frame_color;
    if(v[JET_FX_fxaa])fx.applyFXAA(frame);
    if(v[JET_FX_bloom])fx.applyBloom(frame);
    if(v[JET_FX_crt])fx.applyCRT(frame,v[JET_FX_crt],false,false);
    if(v[JET_FX_pixelate]>1)fx.applyPixelate(frame,v[JET_FX_pixelate]);
    if(v[JET_FX_chromatic])fx.applyChromatic(frame,v[JET_FX_chromatic]);
    if(v[JET_FX_motion_blur])fx.applyMotionBlur(frame,v[JET_FX_motion_blur]);
}
static int postfx_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(q.opcode==SOLAR_OS_JET_SAMPLE_TEXTURE) {
        auto *entry=resource(active->textures,q.handle);if(!entry)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        for(unsigned i=0;i<2;++i)if(!std::isfinite(q.values[i])||q.values[i]<-32||q.values[i]>32)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        uint16_t color=entry->value->getPixel(int(q.values[0]*1024),int(q.values[1]*1024));
        r.handle=entry->id;r.values[0]=((((color>>11)*255+15)/31)<<16)|
            (((((color>>5)&63)*255+31)/63)<<8)|(((color&31)*255+15)/31);return 0;
    }
    if(q.opcode==SOLAR_OS_JET_POSTFX_INFO) {
        std::memcpy(r.values,active->effect_values,sizeof(r.values));
        r.values[16]=active->effects ? active->effects->scratchBytes() : 0;
        r.values[17]=active->effects&&active->effects->hasHistory();return 0;
    }
    if(q.opcode==SOLAR_OS_JET_POSTFX_RESET) {if(active->effects)active->effects->resetHistory();return 0;}
    if(q.opcode!=SOLAR_OS_JET_POSTFX&&q.opcode!=SOLAR_OS_JET_POSTFX_CLEAR)return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    if(q.opcode==SOLAR_OS_JET_POSTFX&&!valid_values(q,jet_postfx_fields,JET_FX_COUNT))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};
    if(q.opcode==SOLAR_OS_JET_POSTFX_CLEAR) {solar_os_jet_command_t defaults={};merge_values(v,defaults,jet_postfx_fields,JET_FX_COUNT);}
    else merge_values(v,q,jet_postfx_fields,JET_FX_COUNT,active->effect_values);
    bool needed=v[JET_FX_fxaa]||v[JET_FX_bloom]||v[JET_FX_crt]||v[JET_FX_motion_blur]||v[JET_FX_chromatic]||v[JET_FX_pixelate]>1;
    if(needed) {
        auto fresh=active->effects ? nullptr : std::make_unique<PostFX>(active->width,active->height);
        auto *fx=fresh ? fresh.get() : active->effects.get();
        unsigned buffers=(v[JET_FX_fxaa]?PostFX::FXAA:0)|(v[JET_FX_bloom]?PostFX::BLOOM:0)|
            (v[JET_FX_motion_blur]?PostFX::MOTION:0)|(v[JET_FX_chromatic]?PostFX::CHROMATIC:0);
        fx->prepare(buffers);
        if(v[JET_FX_motion_blur]&&!active->effect_values[JET_FX_motion_blur])fx->resetHistory();
        if(fresh)active->effects=std::move(fresh);
    } else active->effects.reset();
    std::memcpy(active->effect_values,v,sizeof(v));
    auto *raster=active->scene.getRenderer();raster->celShadingEnabled=v[JET_FX_cel_bits]>0;raster->celShadingBits=v[JET_FX_cel_bits];
    extras_hook_state();return 0;
}
