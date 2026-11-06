/* The OS publishes graph endpoints; the robot library owns kinematics,
 * odometry and wheel coordination. Serialized by the frontend mutex. */
static EXT_RAM_BSS_ATTR struct {
    uint32_t id, registered;
    solar_os_robot_diff_drive_config_t config;
    solar_os_parameter_registration_t parameters;
    float input[2];uint64_t stamp[2];bool writer[2];
    bool accepting;
    solar_os_robot_diff_drive_status_t latest;
} drive_front;
static portMUX_TYPE drive_cache_lock=portMUX_INITIALIZER_UNLOCKED;
static void drive_cache_command(void *context,solar_os_robot_diff_drive_command_t *out)
{
    (void)context;portENTER_CRITICAL(&drive_cache_lock);
    if(drive_front.accepting) *out=(solar_os_robot_diff_drive_command_t){
        .linear=drive_front.input[0],.angular=drive_front.input[1],
        .linear_us=drive_front.stamp[0],.angular_us=drive_front.stamp[1]};
    else *out=(solar_os_robot_diff_drive_command_t){0};
    portEXIT_CRITICAL(&drive_cache_lock);
}
static void drive_cache_publish(void *context,const solar_os_robot_diff_drive_status_t *info)
{ (void)context;portENTER_CRITICAL(&drive_cache_lock);drive_front.latest=*info;portEXIT_CRITICAL(&drive_cache_lock); }
static void drive_front_accept(bool accepting)
{ portENTER_CRITICAL(&drive_cache_lock);drive_front.accepting=accepting;portEXIT_CRITICAL(&drive_cache_lock); }
static unsigned drive_indices[]={0,1,2,3,4,5,6};
static const char *const drive_stream_keys[]={"cmd_linear","cmd_angular","odom_x","odom_y","odom_yaw","odom_linear","odom_angular"};
static const char *const drive_parameter_keys[]={"wheel_radius","wheel_separation","linear_velocity_limit","angular_velocity_limit","command_timeout_ms"};
static void drive_stream_id(unsigned i,char *out,size_t bytes)
{ snprintf(out,bytes,"diff_drive.%s.%s",drive_front.config.name,drive_stream_keys[i]); }
static esp_err_t drive_stream_open(void *user,const char *owner,const solar_os_stream_open_options_t *options,solar_os_stream_handle_t *handle)
{
    (void)owner;unsigned i=*(unsigned *)user;
    if(options->direction==SOLAR_OS_STREAM_DIRECTION_SOURCE) return ESP_OK;
    portENTER_CRITICAL(&drive_cache_lock);esp_err_t e=ESP_ERR_INVALID_STATE;
    if(i<2 && drive_front.accepting && drive_front.id && !drive_front.writer[i]) {
        drive_front.writer[i]=true;handle->private_data[0]=1;e=ESP_OK;
    }
    portEXIT_CRITICAL(&drive_cache_lock);return e;
}
static esp_err_t drive_stream_close(void *user,solar_os_stream_handle_t *handle)
{
    unsigned i=*(unsigned *)user;portENTER_CRITICAL(&drive_cache_lock);
    if(i<2 && handle->private_data[0]) { drive_front.writer[i]=false;handle->private_data[0]=0; }
    portEXIT_CRITICAL(&drive_cache_lock);return ESP_OK;
}
static esp_err_t drive_stream_write(void *user,solar_os_stream_handle_t *handle,const void *data,size_t bytes,uint32_t timeout,size_t *written)
{
    (void)timeout;unsigned i=*(unsigned *)user;*written=0;
    if(i>1 || !handle->private_data[0] || !data || bytes!=sizeof(float)) return ESP_ERR_INVALID_ARG;
    float value;memcpy(&value,data,sizeof(value));
    if(!isfinite(value)) return ESP_ERR_INVALID_ARG;
    uint64_t time=(uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&drive_cache_lock);
    esp_err_t e=drive_front.accepting && drive_front.id && drive_front.latest.state!=SOLAR_OS_ROBOT_FAULT?ESP_OK:ESP_ERR_INVALID_STATE;
    float limit=i?drive_front.config.angular_limit:drive_front.config.linear_limit;
    if(fabsf(value)>limit) e=ESP_ERR_INVALID_ARG;
    if(e==ESP_OK) {
        drive_front.input[i]=value;drive_front.stamp[i]=time;*written=bytes;
    }
    portEXIT_CRITICAL(&drive_cache_lock);return e;
}
static esp_err_t drive_stream_sample(void *user,const solar_os_stream_read_options_t *options,solar_os_stream_scalar_sample_t *out)
{
    (void)options;unsigned i=*(unsigned *)user;portENTER_CRITICAL(&drive_cache_lock);
    esp_err_t e=drive_front.id?ESP_OK:ESP_ERR_NOT_FOUND;
    if(e==ESP_OK && i<2) {
        if(!drive_front.stamp[i]) e=ESP_ERR_TIMEOUT;
        else *out=(solar_os_stream_scalar_sample_t){.value=drive_front.input[i],.uptime_ms=drive_front.stamp[i]/1000};
    } else if(e==ESP_OK) {
        const solar_os_robot_diff_drive_status_t *s=&drive_front.latest;
        if(!s->odometry_valid) e=ESP_ERR_TIMEOUT;
        if(e==ESP_OK) {
            float value=i==2?(float)s->x:i==3?(float)s->y:i==4?(float)s->yaw:i==5?s->linear_velocity:s->angular_velocity;
            if(!isfinite(value)) e=ESP_ERR_INVALID_RESPONSE;
            else *out=(solar_os_stream_scalar_sample_t){.value=value,.uptime_ms=s->sample_us/1000};
        }
    }
    portEXIT_CRITICAL(&drive_cache_lock);return e;
}
static esp_err_t drive_parameter_get(void *user,float *value)
{
    unsigned i=*(unsigned *)user;portENTER_CRITICAL(&drive_cache_lock);
    *value=i==0?drive_front.config.wheel_radius:i==1?drive_front.config.wheel_separation:
        i==2?drive_front.config.linear_limit:i==3?drive_front.config.angular_limit:(float)drive_front.config.command_timeout_ms;
    portEXIT_CRITICAL(&drive_cache_lock);return ESP_OK;
}
static esp_err_t drive_parameter_set(void *user,float value)
{ (void)user;(void)value;return ESP_ERR_INVALID_STATE; } /* Owned URDF configuration. */
static esp_err_t drive_front_publish(void)
{
    for(unsigned i=0;i<7;++i) {
        solar_os_stream_driver_t d={.open=drive_stream_open,.close_checked=drive_stream_close,.write=drive_stream_write,
            .read_scalar_sample=drive_stream_sample,.observe_scalar_sample=drive_stream_sample,.user=&drive_indices[i]};
        drive_stream_id(i,d.info.id,sizeof(d.info.id));
        strlcpy(d.info.provider,"diff_drive",sizeof(d.info.provider));
        strlcpy(d.info.device,drive_front.config.name,sizeof(d.info.device));
        strlcpy(d.info.format,"f32",sizeof(d.info.format));
        strlcpy(d.info.unit,i==0 || i==5?"m/s":i==1 || i==6?"rad/s":i==4?"rad":"m",sizeof(d.info.unit));
        strlcpy(d.info.summary,i<2?"body velocity command; paired freshness required":"wheel-feedback odometry",sizeof(d.info.summary));
        d.info.type=SOLAR_OS_STREAM_TYPE_SCALAR;d.info.direction=i<2?SOLAR_OS_STREAM_DIRECTION_DUPLEX:SOLAR_OS_STREAM_DIRECTION_SOURCE;
        d.info.sharing=SOLAR_OS_STREAM_SHARING_MIXED;
        if(i<2) {
            d.info.has_scalar_range=true;
            d.info.scalar_maximum=i?drive_front.config.angular_limit:drive_front.config.linear_limit;
            d.info.scalar_minimum=-d.info.scalar_maximum;
        }
        esp_err_t e=solar_os_stream_register(&d);if(e!=ESP_OK)return e;
        drive_front.registered|=1u<<i;
    }
    solar_os_parameter_definition_t defs[5];memset(defs,0,sizeof(defs));
    for(unsigned i=0;i<5;++i) {
        defs[i]=(solar_os_parameter_definition_t){.name=drive_parameter_keys[i],.label=drive_parameter_keys[i],
            .unit=i<2?"m":i==2?"m/s":i==3?"rad/s":"ms",.minimum=0,.maximum=1e20f,
            .get=drive_parameter_get,.set=drive_parameter_set,.user=&drive_indices[i]};
    }
    char owner[SOLAR_OS_PARAMETER_OWNER_MAX];snprintf(owner,sizeof(owner),"diff_drive.%s",drive_front.config.name);
    esp_err_t e=solar_os_parameters_register(owner,defs,5,&drive_front.parameters);
    /* Reserve immutable URDF configuration so generic writers cannot claim
     * it as a controller output and then fail every safe-output write. */
    for(unsigned i=0;e==ESP_OK && i<5;++i) {
        char path[SOLAR_OS_PARAMETER_PATH_MAX];snprintf(path,sizeof(path),"%s.%s",owner,drive_parameter_keys[i]);
        e=solar_os_parameter_claim(path,"robot");
    }
    return e;
}
static esp_err_t drive_front_unpublish(void)
{
    esp_err_t e=ESP_OK;
    if(drive_front.parameters.token) e=solar_os_parameters_unregister(&drive_front.parameters);
    for(unsigned i=0;i<7;++i) if(drive_front.registered&(1u<<i)) {
        char id[SOLAR_OS_STREAM_ID_MAX];drive_stream_id(i,id,sizeof(id));
        esp_err_t one=solar_os_stream_unregister(id);
        if(one==ESP_OK) drive_front.registered&=~(1u<<i);
        else if(e==ESP_OK)e=one;
    }
    return e;
}
static esp_err_t drive_front_start(void)
{
    solar_os_robot_diff_drive_config_t c;
    esp_err_t e=result(model.api->model_diff_drive(model.id,0,&c));
    if(e==ESP_ERR_NOT_FOUND)return ESP_OK;
    if(e!=ESP_OK)return e;
    entry_t *l=find(c.left_joint),*r=find(c.right_joint);
    if(!l || !r || !l->model_owned || !r->model_owned)return ESP_ERR_INVALID_STATE;
    const solar_os_robot_diff_drive_io_t io={.command=drive_cache_command,.publish=drive_cache_publish};
    e=result(model.api->diff_drive_open(model.id,l->joint,r->joint,&io,NULL,&drive_front.id));
    if(e!=ESP_OK)return e;
    solar_os_robot_diff_drive_status_t s;
    e=result(model.api->diff_drive_status(drive_front.id,&s));
    if(e==ESP_OK) { drive_front.config=s.config;drive_cache_publish(NULL,&s);e=drive_front_publish(); }
    if(e==ESP_OK)e=result(model.api->diff_drive_enable(drive_front.id));
    return e;
}
esp_err_t solar_os_robot_diff_drive_command(const char *name,float linear,float angular)
{
    lock();esp_err_t e=drive_front.id && (!name || !strcmp(name,drive_front.config.name))?ESP_OK:ESP_ERR_NOT_FOUND;
    uint64_t time=(uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&drive_cache_lock);
    if(e==ESP_OK && (!drive_front.accepting || drive_front.writer[0] || drive_front.writer[1] || drive_front.latest.state==SOLAR_OS_ROBOT_FAULT))e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK && (!isfinite(linear) || !isfinite(angular) || fabsf(linear)>drive_front.config.linear_limit ||
        fabsf(angular)>drive_front.config.angular_limit))e=ESP_ERR_INVALID_ARG;
    if(e==ESP_OK) {
        drive_front.input[0]=linear;drive_front.input[1]=angular;
        drive_front.stamp[0]=drive_front.stamp[1]=time;
    }
    portEXIT_CRITICAL(&drive_cache_lock);
    xSemaphoreGive(mutex);return e;
}
esp_err_t solar_os_robot_diff_drive_status(const char *name,solar_os_robot_diff_drive_status_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    lock();esp_err_t e=drive_front.id && (!name || !strcmp(name,drive_front.config.name))?
        result(model.api->diff_drive_status(drive_front.id,out)):ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex);return e;
}
