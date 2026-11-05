/* Calibrated feedback and parameter/stream adapters share the device lock. */
static const char *const encoder_keys[]={"ppr","gear_ratio","polarity","encoder_on_output","velocity_tau","position_zero"};

bool solar_os_encoder_config_valid(const solar_os_encoder_config_t *c)
{
    return c && isfinite(c->ppr) && c->ppr>=0 && c->ppr<=1e9f &&
        isfinite(c->gear_ratio) && c->gear_ratio>=1e-6f && c->gear_ratio<=1e6f &&
        (c->polarity==1 || c->polarity==-1) && c->encoder_on_output<=1 &&
        isfinite(c->velocity_tau) && c->velocity_tau>=0 && c->velocity_tau<=60 &&
        isfinite(c->position_zero) && fabsf(c->position_zero)<=1e20f;
}
static float encoder_value(const solar_os_encoder_config_t *c,unsigned i)
{
    switch(i) {
    case 0:return c->ppr; case 1:return c->gear_ratio; case 2:return c->polarity;
    case 3:return c->encoder_on_output; case 4:return c->velocity_tau; default:return c->position_zero;
    }
}
static esp_err_t encoder_parameter_get(void *user,float *value)
{
    encoder_parameter_t *p=user; encoder_t *e;
    esp_err_t err=acquire(p->device->info.name,&e);
    if(err==ESP_OK) { *value=encoder_value(&e->config,p->index); release(e); }
    return err;
}
static esp_err_t encoder_parameter_set(void *user,float value)
{
    encoder_parameter_t *p=user; encoder_t *e;
    esp_err_t err=acquire(p->device->info.name,&e);
    if(err!=ESP_OK) return err;
    solar_os_encoder_config_t c=e->config;
    if(!isfinite(value)) err=ESP_ERR_INVALID_ARG;
    else switch(p->index) {
    case 0:c.ppr=value;break; case 1:c.gear_ratio=value;break;
    case 2:if(value!=1 && value!=-1) err=ESP_ERR_INVALID_ARG;else c.polarity=(int)value;break;
    case 3:if(value!=0 && value!=1) err=ESP_ERR_INVALID_ARG;else c.encoder_on_output=(unsigned)value;break;
    case 4:c.velocity_tau=value;break; default:c.position_zero=value;break;
    }
    if(err==ESP_OK && !solar_os_encoder_config_valid(&c)) err=ESP_ERR_INVALID_ARG;
    if(e->claim) err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK) { e->config=c; e->feedback=(solar_os_native_encoder_feedback_v1_t){0}; e->accumulated=0; }
    release(e); return err;
}
esp_err_t solar_os_encoder_config_get(const char *name,solar_os_encoder_config_t *c)
{
    if(!c) return ESP_ERR_INVALID_ARG;
    encoder_t *e;esp_err_t err=acquire(name,&e);
    if(err==ESP_OK) { *c=e->config;release(e); }
    return err;
}
esp_err_t solar_os_encoder_configure(const char *name,const char *key,float value)
{
    if(!valid_name(name,SOLAR_OS_ENCODER_NAME_MAX) || !key) return ESP_ERR_INVALID_ARG;
    for(unsigned i=0;i<6;++i) if(!strcmp(key,encoder_keys[i])) {
        char path[SOLAR_OS_PARAMETER_PATH_MAX];snprintf(path,sizeof(path),"enc.%s.%s",name,key);
        return solar_os_parameter_set(path,value);
    }
    return ESP_ERR_INVALID_ARG;
}
esp_err_t solar_os_encoder_stream_configure(uint32_t claim,const solar_os_encoder_config_t *c)
{
    if(!solar_os_encoder_config_valid(c)) return ESP_ERR_INVALID_ARG;
    encoder_t *e;esp_err_t err=acquire_owned(claim,&e);
    if(err!=ESP_OK) return err;
    int32_t count;
    err=e->ops.read(e->ctx,&count);
    if(err==ESP_OK) {
        e->config=*c;e->accumulated=0;
        e->feedback=(solar_os_native_encoder_feedback_v1_t){.count=count,.decoding=e->info.mode,
            .timestamp_us=(uint64_t)esp_timer_get_time(),.position=c->position_zero,
            .counts_per_rev=(float)solar_os_native_encoder_counts_per_rev(c,e->info.mode)};
    }
    release(e);return err;
}
esp_err_t solar_os_encoder_feedback(uint32_t claim,solar_os_native_encoder_feedback_v1_t *feedback)
{
    if(!feedback) return ESP_ERR_INVALID_ARG;
    encoder_t *e;esp_err_t err=acquire_owned(claim,&e);
    if(err==ESP_OK) { *feedback=e->feedback;release(e); }
    return err;
}
static esp_err_t encoder_stream_open(void *user,const char *owner,
    const solar_os_stream_open_options_t *options,solar_os_stream_handle_t *handle)
{
    (void)owner;(void)options;encoder_t *e=user;
    uint32_t claim;esp_err_t err=solar_os_encoder_claim(e->info.name,&claim);
    if(err!=ESP_OK) return err;
    solar_os_encoder_sample_t sample;
    err=solar_os_encoder_sample_owned(claim,&sample);
    if(err!=ESP_OK) { (void)solar_os_encoder_release(claim);return err; }
    encoder_t *owned;err=acquire_owned(claim,&owned);
    if(err!=ESP_OK) { (void)solar_os_encoder_release(claim);return err; }
    owned->stream_claim=claim;owned->accumulated=0;
    owned->feedback=(solar_os_native_encoder_feedback_v1_t){.count=sample.count,.decoding=sample.decoding,
        .timestamp_us=sample.timestamp_us,.position=owned->config.position_zero,
        .counts_per_rev=(float)solar_os_native_encoder_counts_per_rev(&owned->config,sample.decoding)};
    handle->private_data[0]=claim;release(owned);return ESP_OK;
}
static esp_err_t encoder_stream_close(void *user,solar_os_stream_handle_t *handle)
{
    (void)user;esp_err_t err=solar_os_encoder_release((uint32_t)handle->private_data[0]);
    return err==ESP_ERR_NOT_FOUND?ESP_OK:err;
}
static esp_err_t feedback_advance(encoder_t *e,solar_os_native_encoder_feedback_v1_t *f,
    int64_t *accumulated,int32_t count,uint64_t time,bool seed_velocity)
{
    const solar_os_encoder_config_t *c=&e->config;
    if(c->ppr<=0) return ESP_ERR_INVALID_STATE;
    if(time<=f->timestamp_us) return ESP_ERR_TIMEOUT;
    uint32_t raw=(uint32_t)count-(uint32_t)f->count;
    int64_t delta=raw<=INT32_MAX?(int64_t)raw:(int64_t)raw-4294967296LL;
    if((delta>0 && *accumulated>INT64_MAX-delta) ||
        (delta<0 && *accumulated<INT64_MIN-delta)) return ESP_ERR_INVALID_RESPONSE;
    double counts=solar_os_native_encoder_counts_per_rev(c,e->info.mode);
    double scale=6.2831853071795864769/counts;
    float dt=(float)(time-f->timestamp_us)/1000000;
    float velocity=(float)(c->polarity*delta*scale/dt);
    int64_t total=*accumulated+delta;
    float position=(float)(c->position_zero+(double)c->polarity*(double)total*scale);
    float filtered=seed_velocity?velocity:f->velocity+(velocity-f->velocity)*dt/(c->velocity_tau+dt);
    if(!isfinite(filtered) || !isfinite(position)) return ESP_ERR_INVALID_RESPONSE;
    *accumulated=total;
    *f=(solar_os_native_encoder_feedback_v1_t){.count=count,.decoding=e->info.mode,.timestamp_us=time,
        .position=position,.velocity=filtered,.counts_per_rev=(float)counts,.valid=1};
    return ESP_OK;
}
static esp_err_t encoder_stream_sample(void *user,const solar_os_stream_read_options_t *options,
    solar_os_stream_scalar_sample_t *sample)
{
    encoder_t *e=user,*owned;
    esp_err_t err=acquire_owned(e->stream_claim,&owned);
    if(err!=ESP_OK) return err;
    uint64_t time=(uint64_t)esp_timer_get_time();
    if(options && options->window_ms && (time<=owned->feedback.timestamp_us ||
        time-owned->feedback.timestamp_us<(uint64_t)options->window_ms*1000)) {
        release(owned);return ESP_ERR_TIMEOUT;
    }
    int32_t count;err=owned->ops.read(owned->ctx,&count);
    if(err==ESP_OK) err=feedback_advance(owned,&owned->feedback,&owned->accumulated,count,(uint64_t)esp_timer_get_time(),false);
    if(err==ESP_OK) *sample=(solar_os_stream_scalar_sample_t){.value=owned->feedback.velocity,.uptime_ms=owned->feedback.timestamp_us/1000};
    release(owned);return err;
}
static esp_err_t encoder_stream_observe(void *user,const solar_os_stream_read_options_t *options,
    solar_os_stream_scalar_sample_t *sample)
{
    (void)options;encoder_t *e=user,*owned;
    esp_err_t err=acquire(e->info.name,&owned);
    if(err!=ESP_OK) return err;
    if(!owned->stream_claim) err=ESP_ERR_NOT_SUPPORTED;
    else if(!owned->feedback.valid) err=ESP_ERR_TIMEOUT;
    else *sample=(solar_os_stream_scalar_sample_t){.value=owned->feedback.velocity,.uptime_ms=owned->feedback.timestamp_us/1000};
    release(owned);return err;
}
static esp_err_t publish(encoder_t *e)
{
    solar_os_stream_driver_t stream={.open=encoder_stream_open,.close_checked=encoder_stream_close,
        .read_scalar_sample=encoder_stream_sample,.observe_scalar_sample=encoder_stream_observe,.user=e};
    strlcpy(stream.info.id,e->endpoint,sizeof(stream.info.id));
    strlcpy(stream.info.provider,"encoder",sizeof(stream.info.provider));
    strlcpy(stream.info.device,e->info.name,sizeof(stream.info.device));
    strlcpy(stream.info.unit,"rad/s",sizeof(stream.info.unit));
    strlcpy(stream.info.format,"f32",sizeof(stream.info.format));
    strlcpy(stream.info.summary,"calibrated output-shaft velocity",sizeof(stream.info.summary));
    stream.info.type=SOLAR_OS_STREAM_TYPE_SCALAR;stream.info.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE;
    stream.info.sharing=SOLAR_OS_STREAM_SHARING_EXCLUSIVE;
    esp_err_t err=solar_os_stream_register(&stream);
    if(err!=ESP_OK) return err;
    e->stream_registered=true;
    solar_os_parameter_definition_t definitions[6];
    const float low[]={0,1e-6f,-1,0,0,-1e20f},high[]={1e9f,1e6f,1,1,60,1e20f};
    for(unsigned i=0;i<6;++i) {
        e->parameter_users[i]=(encoder_parameter_t){.device=e,.index=i};
        definitions[i]=(solar_os_parameter_definition_t){.name=encoder_keys[i],.label=encoder_keys[i],.unit="",
            .minimum=low[i],.maximum=high[i],.get=encoder_parameter_get,.set=encoder_parameter_set,
            .user=&e->parameter_users[i]};
    }
    return solar_os_parameters_register(e->endpoint,definitions,6,&e->parameters);
}
static esp_err_t unpublish(encoder_t *e)
{
    esp_err_t err=ESP_OK;
    if(e->stream_registered) {
        err=solar_os_stream_unregister(e->endpoint);
        if(err!=ESP_OK) return err;
        e->stream_registered=false;
    }
    if(e->parameters.token) err=solar_os_parameters_unregister(&e->parameters);
    return err;
}
static esp_err_t reap_failed(const char *name)
{
    lock();encoder_t *e=find(name);
    if(!e || !e->registration_failed) { xSemaphoreGive(registry_mutex);return ESP_OK; }
    if(e->cleanup_busy || e->refs || e->claim) { xSemaphoreGive(registry_mutex);return ESP_ERR_INVALID_STATE; }
    e->cleanup_busy=true;xSemaphoreGive(registry_mutex);
    esp_err_t err=unpublish(e);
    lock();
    if(err==ESP_OK) { encoder_t **link=&encoders;while(*link!=e) link=&(*link)->next;*link=e->next; }
    else e->cleanup_busy=false;
    xSemaphoreGive(registry_mutex);
    if(err==ESP_OK) { vSemaphoreDelete(e->operation_mutex);solar_os_memory_free(e->operation_storage);solar_os_memory_free(e); }
    return err;
}
