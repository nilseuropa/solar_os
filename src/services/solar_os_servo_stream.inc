/* Servo calibration, commands and streams use radians. Streams report the
 * commanded angle, not measured feedback. */
static esp_err_t servo_stream_open(void *user,const char *owner,
    const solar_os_stream_open_options_t *options,solar_os_stream_handle_t *handle)
{
    (void)owner;servo_slot_t *slot=user,*held;
    esp_err_t err=servo_acquire(slot->info.name,NULL,&held);
    if(err!=ESP_OK) return err;
    if(held->info.closing) err=ESP_ERR_INVALID_STATE;
    else if(options->direction!=SOLAR_OS_STREAM_DIRECTION_SOURCE) {
        if(!held->info.stream_writable || held->info.input_claimed) err=ESP_ERR_INVALID_STATE;
        else {
            held->info.input_claimed=true;held->input_revoked=false;handle->private_data[0]=1;
        }
    }
    servo_release(held);return err;
}
static esp_err_t servo_stream_close(void *user,solar_os_stream_handle_t *handle)
{
    if(!handle->private_data[0]) return ESP_OK;
    servo_slot_t *slot=user,*held;esp_err_t err=servo_acquire(slot->info.name,NULL,&held);
    if(err==ESP_OK) {
        held->info.input_claimed=false;handle->private_data[0]=0;servo_release(held);
    }
    return err;
}
static esp_err_t servo_stream_write(void *user,solar_os_stream_handle_t *handle,
    const void *data,size_t bytes,uint32_t timeout_ms,size_t *written)
{
    (void)timeout_ms;*written=0;
    if(!handle->private_data[0] || !data || bytes!=sizeof(float)) return ESP_ERR_INVALID_ARG;
    float radians;memcpy(&radians,data,sizeof(radians));
    if(!isfinite(radians)) return ESP_ERR_INVALID_ARG;
    servo_slot_t *slot=user,*held;esp_err_t err=servo_acquire(slot->info.name,NULL,&held);
    if(err!=ESP_OK) return err;
    if(held->pwm_closed) err=ESP_ERR_NOT_FOUND;
    else if(held->info.closing || held->input_revoked || !held->info.input_claimed) err=ESP_ERR_INVALID_STATE;
    else err=servo_set_locked(held,radians);
    if(err==ESP_OK) *written=bytes;
    servo_release(held);return err;
}
static esp_err_t servo_stream_sample(void *user,const solar_os_stream_read_options_t *options,
    solar_os_stream_scalar_sample_t *sample)
{
    (void)options;servo_slot_t *slot=user,*held;esp_err_t err=servo_acquire(slot->info.name,NULL,&held);
    if(err!=ESP_OK) return err;
    if(held->info.closing) err=ESP_ERR_INVALID_STATE;
    else if(!held->info.has_command) err=ESP_ERR_TIMEOUT;
    else *sample=(solar_os_stream_scalar_sample_t){
        .value=held->info.commanded_angle_rad,.uptime_ms=held->command_time_us/1000};
    servo_release(held);return err;
}
static esp_err_t servo_publish(servo_slot_t *slot)
{
    solar_os_stream_driver_t stream={.open=servo_stream_open,.close_checked=servo_stream_close,
        .write=servo_stream_write,.read_scalar_sample=servo_stream_sample,.observe_scalar_sample=servo_stream_sample,.user=slot};
    snprintf(stream.info.id,sizeof(stream.info.id),"servo.%s",slot->info.name);
    strlcpy(stream.info.provider,"servo",sizeof(stream.info.provider));
    strlcpy(stream.info.device,slot->info.name,sizeof(stream.info.device));
    strlcpy(stream.info.format,"f32",sizeof(stream.info.format));
    strlcpy(stream.info.unit,"rad",sizeof(stream.info.unit));
    strlcpy(stream.info.summary,"commanded servo angle; no measured feedback",sizeof(stream.info.summary));
    stream.info.type=SOLAR_OS_STREAM_TYPE_SCALAR;
    stream.info.direction=slot->info.stream_writable?SOLAR_OS_STREAM_DIRECTION_DUPLEX:SOLAR_OS_STREAM_DIRECTION_SOURCE;
    stream.info.sharing=SOLAR_OS_STREAM_SHARING_MIXED;
    esp_err_t err=solar_os_stream_register(&stream);
    if(err==ESP_OK) slot->stream_registered=true;
    return err;
}
