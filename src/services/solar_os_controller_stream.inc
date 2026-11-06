/* Setpoint streams share the parameter writer claim. Source readers and echo
 * observe the last accepted command without acquiring that writer claim. */
static void input_path(const controller_slot_t *slot,char *path,size_t bytes)
{ snprintf(path,bytes,"controller.%s.setpoint",slot->info.config.name); }
static void input_writer_name(const controller_slot_t *slot,char *name,size_t bytes)
{ snprintf(name,bytes,"input.%s",slot->info.config.name); }
static esp_err_t input_open(void *user,const char *owner,
    const solar_os_stream_open_options_t *options,solar_os_stream_handle_t *handle)
{
    (void)owner;controller_slot_t *slot=user;
    if(options->direction==SOLAR_OS_STREAM_DIRECTION_SOURCE) return ESP_OK;
    lock();esp_err_t err=slot->active?ESP_OK:ESP_ERR_NOT_FOUND;
    if(err==ESP_OK && (slot->owner_id || slot->input_writer || tuning_running(slot))) err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK) {
        char path[SOLAR_OS_STREAM_ID_MAX],writer[SOLAR_OS_PARAMETER_WRITER_MAX];
        input_path(slot,path,sizeof(path));input_writer_name(slot,writer,sizeof(writer));
        err=solar_os_parameter_claim(path,writer);
        if(err==ESP_OK) { slot->input_writer=true;slot->info.input_claimed=true;handle->private_data[0]=1; }
    }
    xSemaphoreGive(mutex);return err;
}
static esp_err_t input_close(void *user,solar_os_stream_handle_t *handle)
{
    if(!handle->private_data[0]) return ESP_OK;
    controller_slot_t *slot=user;lock();
    char path[SOLAR_OS_STREAM_ID_MAX],writer[SOLAR_OS_PARAMETER_WRITER_MAX];
    input_path(slot,path,sizeof(path));input_writer_name(slot,writer,sizeof(writer));
    esp_err_t err=solar_os_parameter_release(path,writer);
    if(err==ESP_OK || err==ESP_ERR_NOT_FOUND) { slot->input_writer=false;slot->info.input_claimed=false;handle->private_data[0]=0;err=ESP_OK; }
    xSemaphoreGive(mutex);return err;
}
static esp_err_t input_write(void *user,solar_os_stream_handle_t *handle,
    const void *data,size_t bytes,uint32_t timeout_ms,size_t *written)
{
    (void)timeout_ms;*written=0;
    if(!handle->private_data[0] || !data || bytes!=sizeof(float)) return ESP_ERR_INVALID_ARG;
    controller_slot_t *slot=user;
    char path[SOLAR_OS_STREAM_ID_MAX],writer[SOLAR_OS_PARAMETER_WRITER_MAX];
    input_path(slot,path,sizeof(path));input_writer_name(slot,writer,sizeof(writer));
    float value;memcpy(&value,data,sizeof(value));
    esp_err_t err=solar_os_parameter_set_owned(path,writer,value);
    if(err==ESP_OK) *written=bytes;
    return err;
}
static esp_err_t input_sample(void *user,const solar_os_stream_read_options_t *options,
    solar_os_stream_scalar_sample_t *sample)
{
    (void)options;controller_slot_t *slot=user;lock();
    esp_err_t err=!slot->active?ESP_ERR_NOT_FOUND:!slot->input_sample_valid?ESP_ERR_TIMEOUT:ESP_OK;
    if(err==ESP_OK) *sample=(solar_os_stream_scalar_sample_t){.value=slot->input_value,.uptime_ms=slot->input_time_us/1000};
    xSemaphoreGive(mutex);return err;
}
static esp_err_t publish_input(controller_slot_t *slot)
{
    solar_os_stream_driver_t stream={.open=input_open,.close_checked=input_close,.write=input_write,
        .read_scalar_sample=input_sample,.observe_scalar_sample=input_sample,.user=slot};
    input_path(slot,stream.info.id,sizeof(stream.info.id));
    strlcpy(stream.info.provider,"controller",sizeof(stream.info.provider));
    strlcpy(stream.info.device,slot->info.config.name,sizeof(stream.info.device));
    strlcpy(stream.info.format,"f32",sizeof(stream.info.format));
    strlcpy(stream.info.summary,"last accepted setpoint; native measurement units",sizeof(stream.info.summary));
    stream.info.type=SOLAR_OS_STREAM_TYPE_SCALAR;
    stream.info.direction=slot->owner_id?SOLAR_OS_STREAM_DIRECTION_SOURCE:SOLAR_OS_STREAM_DIRECTION_DUPLEX;
    stream.info.sharing=SOLAR_OS_STREAM_SHARING_MIXED;
    esp_err_t err=solar_os_stream_register(&stream);
    if(err==ESP_OK) slot->input_registered=true;
    return err;
}
static esp_err_t unpublish_input(controller_slot_t *slot)
{
    if(!slot->input_registered) return ESP_OK;
    char path[SOLAR_OS_STREAM_ID_MAX];input_path(slot,path,sizeof(path));
    esp_err_t err=solar_os_stream_unregister(path);
    if(err==ESP_OK) slot->input_registered=false;
    return err;
}
