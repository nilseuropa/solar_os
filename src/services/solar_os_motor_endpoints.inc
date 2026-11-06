static const char *const motor_keys[]={"polarity","max_drive","brake"};
bool solar_os_motor_config_valid(const solar_os_motor_config_t *c)
{ return c && (c->polarity==1 || c->polarity==-1) && isfinite(c->max_drive) && c->max_drive>0 && c->max_drive<=1 && c->brake<=1; }
static esp_err_t drive_guarded(motor_t *m,float value,uint64_t valid_until_us)
{
    if(!isfinite(value) || fabsf(value)>1) return ESP_ERR_INVALID_ARG;
    if(fabsf(value)>m->config.max_drive) return ESP_ERR_INVALID_ARG;
    if(value==0) return m->ops.coast(m->ctx);
    if(!m->ops.drive) return ESP_ERR_NOT_SUPPORTED;
    solar_os_motor_status_t status={0};
    esp_err_t err=m->ops.status(m->ctx,&status);
    if(err!=ESP_OK) return err;
    if(status.blocking_faults) return ESP_ERR_INVALID_STATE;
    if(valid_until_us && (uint64_t)esp_timer_get_time()>=valid_until_us) return ESP_ERR_TIMEOUT;
    return m->ops.drive(m->ctx,value*m->config.polarity);
}
static esp_err_t motor_parameter_get(void *user,float *value)
{
    motor_parameter_t *p=user;motor_t *m;esp_err_t err=acquire(p->device->info.name,&m);
    if(err==ESP_OK) { *value=p->index==0?m->config.polarity:p->index==1?m->config.max_drive:m->config.brake;release(m); }
    return err;
}
static esp_err_t motor_parameter_set(void *user,float value)
{
    motor_parameter_t *p=user;motor_t *m;esp_err_t err=acquire(p->device->info.name,&m);
    if(err!=ESP_OK) return err;
    solar_os_motor_config_t c=m->config;
    if(!isfinite(value)) err=ESP_ERR_INVALID_ARG;
    else if(p->index==0) { if(value!=1 && value!=-1) err=ESP_ERR_INVALID_ARG;else c.polarity=(int)value; }
    else if(p->index==1) c.max_drive=value;
    else if(value!=0 && value!=1) err=ESP_ERR_INVALID_ARG;
    else c.brake=(unsigned)value;
    if(err==ESP_OK && (!solar_os_motor_config_valid(&c) || (c.brake && !m->info.can_brake))) err=ESP_ERR_INVALID_ARG;
    if(m->claim) err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK) {
        /* Changing sign or drive policy must not reinterpret a live demand. */
        err=m->ops.coast(m->ctx);
        if(err==ESP_OK) m->config=c;
    }
    release(m);return err;
}
esp_err_t solar_os_motor_config_get(const char *name,solar_os_motor_config_t *c)
{
    if(!c) return ESP_ERR_INVALID_ARG;
    motor_t *m;esp_err_t err=acquire(name,&m);
    if(err==ESP_OK) { *c=m->config;release(m); }return err;
}
esp_err_t solar_os_motor_configure(const char *name,const char *key,float value)
{
    if(!valid_name(name,SOLAR_OS_MOTOR_NAME_MAX) || !key) return ESP_ERR_INVALID_ARG;
    for(unsigned i=0;i<3;++i) if(!strcmp(key,motor_keys[i])) {
        char path[SOLAR_OS_PARAMETER_PATH_MAX];snprintf(path,sizeof(path),"mot.%s.%s",name,key);
        return solar_os_parameter_set(path,value);
    }
    return ESP_ERR_INVALID_ARG;
}
esp_err_t solar_os_motor_stream_configure(uint32_t claim,const solar_os_motor_config_t *c)
{
    if(!solar_os_motor_config_valid(c)) return ESP_ERR_INVALID_ARG;
    motor_t *m;esp_err_t err=acquire_owned(claim,&m);
    if(err!=ESP_OK) return err;
    if(c->brake && !m->info.can_brake) err=ESP_ERR_NOT_SUPPORTED;
    else { err=m->ops.coast(m->ctx);if(err==ESP_OK) m->config=*c; }
    release(m);return err;
}
static esp_err_t motor_stream_open(void *user,const char *owner,
    const solar_os_stream_open_options_t *options,solar_os_stream_handle_t *handle)
{
    (void)owner;(void)options;motor_t *m=user;uint32_t claim;
    esp_err_t err=solar_os_motor_claim(m->info.name,&claim);
    if(err==ESP_OK) { handle->private_data[0]=claim;handle->private_data[1]=m->config.brake; }
    return err;
}
static esp_err_t motor_stream_close(void *user,solar_os_stream_handle_t *handle)
{
    (void)user;motor_t *m;uint32_t claim=(uint32_t)handle->private_data[0];
    esp_err_t err=acquire_owned(claim,&m);
    if(err==ESP_ERR_NOT_FOUND) return ESP_OK;
    if(err!=ESP_OK) return err;
    bool brake=handle->private_data[1]!=0;release(m);
    return solar_os_motor_release(claim,brake);
}
static esp_err_t motor_stream_write(void *user,solar_os_stream_handle_t *handle,
    const void *data,size_t bytes,uint32_t timeout_ms,size_t *written)
{
    (void)user;(void)timeout_ms;*written=0;
    if((bytes!=sizeof(float) && bytes!=sizeof(solar_os_stream_scalar_command_t)) || !data) return ESP_ERR_INVALID_ARG;
    solar_os_stream_scalar_command_t command={0};memcpy(&command,data,bytes);
    float value=command.value;
    esp_err_t err;
    if(value==0) {
        motor_t *m;err=acquire_owned((uint32_t)handle->private_data[0],&m);
        if(err==ESP_OK) { bool brake=m->config.brake;release(m);err=solar_os_motor_stop_owned((uint32_t)handle->private_data[0],brake); }
    } else {
        motor_t *m;err=acquire_owned((uint32_t)handle->private_data[0],&m);
        if(err==ESP_OK) { err=drive_guarded(m,value,command.valid_until_us);release(m); }
    }
    if(err==ESP_OK) *written=bytes;
    return err;
}
static esp_err_t publish(motor_t *m)
{
    solar_os_stream_driver_t stream={.open=motor_stream_open,.close_checked=motor_stream_close,
        .write=motor_stream_write,.user=m};
    strlcpy(stream.info.id,m->endpoint,sizeof(stream.info.id));
    strlcpy(stream.info.provider,"motor",sizeof(stream.info.provider));
    strlcpy(stream.info.device,m->info.name,sizeof(stream.info.device));
    strlcpy(stream.info.unit,"drive",sizeof(stream.info.unit));
    strlcpy(stream.info.format,"f32",sizeof(stream.info.format));
    strlcpy(stream.info.summary,"normalized signed motor drive",sizeof(stream.info.summary));
    stream.info.type=SOLAR_OS_STREAM_TYPE_SCALAR;stream.info.direction=SOLAR_OS_STREAM_DIRECTION_SINK;
    stream.info.sharing=SOLAR_OS_STREAM_SHARING_EXCLUSIVE;
    esp_err_t err=solar_os_stream_register(&stream);if(err!=ESP_OK) return err;
    m->stream_registered=true;
    solar_os_parameter_definition_t definitions[3];
    const float low[]={-1,0,0},high[]={1,1,1};
    for(unsigned i=0;i<3;++i) {
        m->parameter_users[i]=(motor_parameter_t){.device=m,.index=i};
        definitions[i]=(solar_os_parameter_definition_t){.name=motor_keys[i],.label=motor_keys[i],.unit="",
            .minimum=low[i],.maximum=high[i],.get=motor_parameter_get,.set=motor_parameter_set,
            .user=&m->parameter_users[i]};
    }
    return solar_os_parameters_register(m->endpoint,definitions,3,&m->parameters);
}
static esp_err_t unpublish(motor_t *m)
{
    esp_err_t err=ESP_OK;
    if(m->stream_registered) {
        err=solar_os_stream_unregister(m->endpoint);if(err!=ESP_OK) return err;m->stream_registered=false;
    }
    if(m->parameters.token) err=solar_os_parameters_unregister(&m->parameters);
    return err;
}
static esp_err_t reap_failed(const char *name)
{
    lock();motor_t *m=find(name);
    if(!m || !m->registration_failed) { xSemaphoreGive(registry_mutex);return ESP_OK; }
    if(m->cleanup_busy || m->refs || m->claim) { xSemaphoreGive(registry_mutex);return ESP_ERR_INVALID_STATE; }
    m->cleanup_busy=true;xSemaphoreGive(registry_mutex);
    esp_err_t err=unpublish(m);
    lock();
    if(err==ESP_OK) { motor_t **link=&motors;while(*link!=m) link=&(*link)->next;*link=m->next; }
    else m->cleanup_busy=false;
    xSemaphoreGive(registry_mutex);
    if(err==ESP_OK) { vSemaphoreDelete(m->operation_mutex);solar_os_memory_free(m->operation_storage);solar_os_memory_free(m); }
    return err;
}
