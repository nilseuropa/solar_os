/* Model startup is a transaction over the same endpoints used by controller
 * bind/start. A failed rollback retains its controller handle for stop retry. */
static const char *const binding_keys[]={"ppr","gear_ratio","polarity","encoder_on_output","velocity_tau","position_zero",
    "polarity","max_drive","brake"};
static void binding_path(const controller_slot_t *slot,unsigned index,char *path,size_t bytes)
{
    snprintf(path,bytes,"%s.%s.%s",index<6?"enc":"mot",
        index<6?slot->binding.encoder:slot->binding.motor,binding_keys[index]);
}
static esp_err_t restore_binding(controller_slot_t *slot)
{
    if(!slot->managed) return ESP_OK;
    esp_err_t err=ESP_OK;
    if(slot->restore_encoder) {
        err=solar_os_encoder_stream_configure((uint32_t)slot->source_handle.private_data[0],&slot->saved_encoder);
        if(err!=ESP_OK) return err;
        slot->restore_encoder=false;
    }
    if(slot->restore_motor) {
        err=solar_os_motor_stream_configure((uint32_t)slot->target_handle.private_data[0],&slot->saved_motor);
        if(err!=ESP_OK) return err;
        slot->restore_motor=false;
    }
    char writer[SOLAR_OS_PARAMETER_WRITER_MAX];writer_name(slot,writer,sizeof(writer));
    for(unsigned i=0;i<9;++i) if(slot->parameter_claims & (1U<<i)) {
        char path[SOLAR_OS_PARAMETER_PATH_MAX];binding_path(slot,i,path,sizeof(path));
        err=solar_os_parameter_release(path,writer);
        if(err!=ESP_OK) return err;
        slot->parameter_claims &= ~(1U<<i);
    }
    return ESP_OK;
}
esp_err_t solar_os_controller_open_bound(const char *name,const char *owner,
    const solar_os_native_bound_controller_config_v1_t *c,uint32_t *id)
{
    if(id) *id=0;
    if(!c || !id || !c->motor[0] || strnlen(c->motor,sizeof(c->motor))>=sizeof(c->motor) ||
        !c->encoder[0] || strnlen(c->encoder,sizeof(c->encoder))>=sizeof(c->encoder) ||
        !solar_os_encoder_config_valid(&c->encoder_config) || c->encoder_config.ppr<=0 ||
        !solar_os_motor_config_valid(&c->motor_config) || !isfinite(c->acceleration_limit) || c->acceleration_limit<0 ||
        !isfinite(c->velocity_deadband) || c->velocity_deadband<0 ||
        !isfinite(c->friction_bias_forward) || c->friction_bias_forward<0 ||
        !isfinite(c->friction_bias_reverse) || c->friction_bias_reverse<0 ||
        c->friction_bias_forward>c->pid.output_max || -c->friction_bias_reverse<c->pid.output_min ||
        c->pid.output_min < -c->motor_config.max_drive || c->pid.output_max>c->motor_config.max_drive ||
        c->command_timeout_ms<c->pid.period_ms || c->command_timeout_ms>60000) return ESP_ERR_INVALID_ARG;
    solar_os_motor_status_t motor;
    esp_err_t err=solar_os_motor_status(c->motor,&motor);
    if(err!=ESP_OK) return err;
    if(motor.blocking_faults || !motor.info.can_drive || (c->motor_config.brake && !motor.info.can_brake)) return ESP_ERR_INVALID_STATE;
    solar_os_encoder_config_t encoder;
    err=solar_os_encoder_config_get(c->encoder,&encoder);
    if(err!=ESP_OK) return err;
    err=solar_os_controller_open_owned(name,owner,&c->pid,id);
    if(err!=ESP_OK) return err;
    lock();controller_slot_t *slot=find_owned(*id);
    slot->managed=true;slot->binding=*c;slot->info.enabled=false;
    solar_os_controller_config_t *config=&slot->info.config;
    snprintf(config->source,sizeof(config->source),"enc.%s",c->encoder);
    snprintf(config->target,sizeof(config->target),"stream:mot.%s",c->motor);
    config->acceleration_limit=c->acceleration_limit;config->velocity_deadband=c->velocity_deadband;
    config->friction_bias_forward=c->friction_bias_forward;config->friction_bias_reverse=c->friction_bias_reverse;
    config->command_timeout_ms=c->command_timeout_ms;
    char writer[SOLAR_OS_PARAMETER_WRITER_MAX];writer_name(slot,writer,sizeof(writer));
    for(unsigned i=0;i<9 && err==ESP_OK;++i) {
        char path[SOLAR_OS_PARAMETER_PATH_MAX];binding_path(slot,i,path,sizeof(path));
        err=solar_os_parameter_claim(path,writer);
        if(err==ESP_OK) { slot->parameter_claims|=1U<<i;slot->info.claimed=true; }
    }
    if(err==ESP_OK) err=start_locked(slot);
    if(err==ESP_OK) err=write_output(slot,0);
    if(err==ESP_OK) err=solar_os_encoder_config_get(c->encoder,&slot->saved_encoder);
    if(err==ESP_OK) err=solar_os_motor_config_get(c->motor,&slot->saved_motor);
    if(err==ESP_OK) {
        slot->restore_encoder=true;
        err=solar_os_encoder_stream_configure((uint32_t)slot->source_handle.private_data[0],&c->encoder_config);
    }
    if(err==ESP_OK) {
        slot->restore_motor=true;
        err=solar_os_motor_stream_configure((uint32_t)slot->target_handle.private_data[0],&c->motor_config);
        slot->target_handle.private_data[1]=c->motor_config.brake;
    }
    if(err==ESP_OK) err=solar_os_encoder_feedback((uint32_t)slot->source_handle.private_data[0],&slot->info.feedback);
    if(err!=ESP_OK) {
        esp_err_t cleanup=stop_locked(slot);
        if(cleanup==ESP_OK) { *slot=(controller_slot_t){0};*id=0; }
        else { slot->info.faulted=true;slot->info.last_error=cleanup; }
    } else {
        slot->info.command_valid=false;slot->info.waiting=true;slot->info.active_setpoint=0;
        slot->info.has_measurement=false;slot->last_step_ms=solar_os_time_uptime_ms();
    }
    xSemaphoreGive(mutex);return err;
}
static void bound_status(const controller_slot_t *slot,solar_os_native_bound_controller_status_v1_t *s)
{
    const solar_os_controller_info_t *i=&slot->info;
    *s=(solar_os_native_bound_controller_status_v1_t){.enabled=i->enabled,.claimed=i->claimed,
        .waiting=i->waiting,.faulted=i->faulted,.command_valid=i->command_valid,
        .command_expires_us=i->command_expires_us,.steps=i->steps,.faults=i->faults,.last_error=i->last_error,
        .target=i->config.setpoint,.setpoint=i->active_setpoint,.output=i->state.output+i->feedforward,
        .p=i->state.p,.i=i->state.i,.d=i->state.d,.saturated=i->state.saturated,
        .feedforward=i->feedforward,.feedback=i->feedback};
    if(!i->enabled || i->faulted) s->feedback.valid=0;
}
esp_err_t solar_os_controller_command_owned(uint32_t id,float setpoint,uint64_t acquired_us)
{
    lock();controller_slot_t *slot=find_owned(id);
    esp_err_t err=slot?ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND;
    if(slot && slot->managed && slot->info.enabled && !slot->info.faulted) {
        uint64_t time=(uint64_t)esp_timer_get_time(),timeout=(uint64_t)slot->info.config.command_timeout_ms*1000;
        if(acquired_us>time || (timeout && time-acquired_us>=timeout) || acquired_us>UINT64_MAX-timeout) err=ESP_ERR_TIMEOUT;
        else {
            err=command_locked(slot,setpoint);
            if(err==ESP_OK) slot->info.command_expires_us=timeout?acquired_us+timeout:0;
        }
    }
    xSemaphoreGive(mutex);return err;
}
esp_err_t solar_os_controller_step_bound(uint32_t id,solar_os_native_bound_controller_status_v1_t *s)
{
    if(!s) return ESP_ERR_INVALID_ARG;
    *s=(solar_os_native_bound_controller_status_v1_t){0};
    lock();controller_slot_t *slot=find_owned(id);
    esp_err_t err=slot?ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND;
    if(slot && slot->managed) {
        if(slot->info.enabled) { tick_locked(slot,solar_os_time_uptime_ms());err=ESP_OK; }
        bound_status(slot,s);
        if(slot->info.faulted) err=slot->info.last_error;
    }
    xSemaphoreGive(mutex);return err;
}
esp_err_t solar_os_controller_status_bound(uint32_t id,solar_os_native_bound_controller_status_v1_t *s)
{
    if(!s) return ESP_ERR_INVALID_ARG;
    *s=(solar_os_native_bound_controller_status_v1_t){0};
    lock();controller_slot_t *slot=find_owned(id);
    esp_err_t err=slot?slot->managed?ESP_OK:ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND;
    if(err==ESP_OK) bound_status(slot,s);
    xSemaphoreGive(mutex);return err;
}
