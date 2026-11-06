/* Receive handles are pinned under osc_mutex; provider callbacks run without
 * that mutex. Busy bindings reject concurrent removal or dispatch. */
static esp_err_t osc_receive_close_slot(size_t index, uint32_t id, bool remove)
{
    xSemaphoreTake(osc_mutex,portMAX_DELAY);
    osc_binding_slot_t *slot=&osc_bindings[index];
    if(!slot->active || slot->info.id!=id) { xSemaphoreGive(osc_mutex);return ESP_OK; }
    if(slot->busy) { xSemaphoreGive(osc_mutex);return ESP_ERR_INVALID_STATE; }
    slot->busy=true;xSemaphoreGive(osc_mutex);
    esp_err_t err=solar_os_stream_handle_valid(&slot->sink)?solar_os_stream_close_ex(&slot->sink):ESP_OK;
    xSemaphoreTake(osc_mutex,portMAX_DELAY);
    if(err==ESP_OK && remove)memset(slot,0,sizeof(*slot));
    else {
        slot->busy=false;slot->info.target_claimed=solar_os_stream_handle_valid(&slot->sink);
        slot->info.last_error=err;
    }
    xSemaphoreGive(osc_mutex);return err;
}

esp_err_t solar_os_osc_receive_release(void)
{
    if(osc_ensure_mutex()!=ESP_OK)return ESP_ERR_NO_MEM;
    esp_err_t err=ESP_OK;
    for(size_t i=0;i<SOLAR_OS_OSC_BINDING_MAX;++i) {
        xSemaphoreTake(osc_mutex,portMAX_DELAY);
        uint32_t id=osc_bindings[i].active && osc_bindings[i].info.config.direction==SOLAR_OS_OSC_RECEIVE?osc_bindings[i].info.id:0;
        xSemaphoreGive(osc_mutex);
        if(id) { esp_err_t one=osc_receive_close_slot(i,id,false);if(one!=ESP_OK && err==ESP_OK)err=one; }
    }
    return err;
}

typedef struct {
    const char *address,*types;
    size_t address_len,count;
    float values[SOLAR_OS_OSC_ARGUMENT_MAX];
} osc_message_t;
typedef struct { size_t count;osc_message_t messages[SOLAR_OS_OSC_PACKET_UPDATE_MAX]; } osc_packet_t;

static esp_err_t osc_parse_packet(const uint8_t *data,size_t bytes,unsigned depth,osc_packet_t *packet)
{
    if(!bytes)return ESP_ERR_INVALID_RESPONSE;
    if(bytes>=8 && !memcmp(data,"#bundle\0",8)) {
        if(depth>=SOLAR_OS_OSC_BUNDLE_DEPTH_MAX || bytes<16 || osc_read_u32(data+8)!=0 || osc_read_u32(data+12)!=1)
            return ESP_ERR_NOT_SUPPORTED;
        size_t offset=16;
        while(offset<bytes) {
            if(bytes-offset<4)return ESP_ERR_INVALID_RESPONSE;
            uint32_t size=osc_read_u32(data+offset);offset+=4;
            if(!size || size>bytes-offset)return ESP_ERR_INVALID_RESPONSE;
            esp_err_t err=osc_parse_packet(data+offset,size,depth+1,packet);
            if(err!=ESP_OK)return err;
            offset+=size;
        }
        return ESP_OK;
    }
    if(packet->count==SOLAR_OS_OSC_PACKET_UPDATE_MAX)return ESP_ERR_INVALID_SIZE;
    osc_message_t *message=&packet->messages[packet->count];
    size_t offset=0,types_len=0;
    esp_err_t err=osc_read_string(data,bytes,&offset,&message->address,&message->address_len);
    if(err!=ESP_OK || !osc_address_valid(message->address))return ESP_ERR_INVALID_RESPONSE;
    err=osc_read_string(data,bytes,&offset,&message->types,&types_len);
    if(err!=ESP_OK || types_len<2 || types_len>SOLAR_OS_OSC_ARGUMENT_MAX+1 || message->types[0]!=',')return ESP_ERR_INVALID_RESPONSE;
    message->count=types_len-1;
    if(!strncmp(message->address,OSC_PARAMETER_PREFIX,sizeof(OSC_PARAMETER_PREFIX)-1) && message->count!=1)
        return ESP_ERR_INVALID_RESPONSE;
    for(size_t i=0;i<message->count;++i) {
        char type=message->types[i+1];float value;
        if(type=='f' || type=='i') {
            if(bytes-offset<4)return ESP_ERR_INVALID_RESPONSE;
            uint32_t raw=osc_read_u32(data+offset);offset+=4;
            if(type=='f')memcpy(&value,&raw,sizeof(value));else value=(float)(int32_t)raw;
        } else if(type=='T' || type=='F')value=type=='T'?1:0;
        else return ESP_ERR_NOT_SUPPORTED;
        if(!isfinite(value))return ESP_ERR_INVALID_RESPONSE;
        message->values[i]=value;
    }
    if(offset!=bytes)return ESP_ERR_INVALID_RESPONSE;
    ++packet->count;return ESP_OK;
}

static bool osc_receive_matches(const osc_binding_slot_t *slot,const osc_message_t *message)
{
    return slot->active && slot->info.config.direction==SOLAR_OS_OSC_RECEIVE &&
        !strcmp(slot->info.config.address,message->address);
}

static esp_err_t osc_receive_pin(const osc_packet_t *packet,bool *pinned)
{
    unsigned updates=0;xSemaphoreTake(osc_mutex,portMAX_DELAY);
    for(size_t m=0;m<packet->count;++m) {
        const osc_message_t *message=&packet->messages[m];
        if(!strncmp(message->address,OSC_PARAMETER_PREFIX,sizeof(OSC_PARAMETER_PREFIX)-1))++updates;
        for(size_t i=0;i<SOLAR_OS_OSC_BINDING_MAX;++i)if(osc_receive_matches(&osc_bindings[i],message)) {
            if(osc_bindings[i].busy) { xSemaphoreGive(osc_mutex);return ESP_ERR_INVALID_STATE; }
            ++updates;pinned[i]=true;
        }
    }
    if(updates>SOLAR_OS_OSC_PACKET_UPDATE_MAX) { xSemaphoreGive(osc_mutex);return ESP_ERR_INVALID_SIZE; }
    for(size_t i=0;i<SOLAR_OS_OSC_BINDING_MAX;++i)if(pinned[i])osc_bindings[i].busy=true;
    xSemaphoreGive(osc_mutex);return ESP_OK;
}

static esp_err_t osc_receive_message(const osc_message_t *message,const bool *pinned,
                                     solar_os_osc_dispatch_result_t *result)
{
    size_t indices[SOLAR_OS_OSC_PACKET_UPDATE_MAX],count=0;
    bool opened[SOLAR_OS_OSC_PACKET_UPDATE_MAX]={0};
    for(size_t i=0;i<SOLAR_OS_OSC_BINDING_MAX;++i)
        if(pinned[i] && osc_receive_matches(&osc_bindings[i],message))indices[count++]=i;
    if(!count)return ESP_ERR_NOT_FOUND;
    esp_err_t err=ESP_OK;uint64_t now=solar_os_time_uptime_ms();
    /* Validate all mapped arguments and destinations before opening/writing. */
    for(size_t k=0;k<count;++k) {
        osc_binding_slot_t *slot=&osc_bindings[indices[k]];
        if(slot->info.config.argument>=message->count) { err=ESP_ERR_INVALID_ARG;goto reject; }
        solar_os_stream_info_t info;
        err=solar_os_stream_get_info(slot->info.config.source,&info);
        xSemaphoreTake(osc_mutex,portMAX_DELAY);slot->info.target_available=err==ESP_OK;xSemaphoreGive(osc_mutex);
        if(err!=ESP_OK)goto reject;
        if(info.type!=SOLAR_OS_STREAM_TYPE_SCALAR || info.direction==SOLAR_OS_STREAM_DIRECTION_SOURCE || strcmp(info.format,"f32")) {
            err=ESP_ERR_NOT_SUPPORTED;goto reject;
        }
        float value=message->values[slot->info.config.argument];
        if(info.has_scalar_range && (value<info.scalar_minimum || value>info.scalar_maximum)) { err=ESP_ERR_INVALID_ARG;goto reject; }
    }
    for(size_t k=0;k<count;++k) {
        osc_binding_slot_t *slot=&osc_bindings[indices[k]];
        if(!solar_os_stream_handle_valid(&slot->sink)) {
            const solar_os_stream_open_options_t options={.direction=SOLAR_OS_STREAM_DIRECTION_SINK};
            err=solar_os_stream_open_ex(slot->info.config.source,slot->info.config.name,&options,&slot->sink);
            opened[k]=solar_os_stream_handle_valid(&slot->sink);
            if(err!=ESP_OK)goto reject;
        }
    }
    for(size_t k=0;k<count;++k) {
        osc_binding_slot_t *slot=&osc_bindings[indices[k]];
        float value=message->values[slot->info.config.argument];size_t written=0;
        err=solar_os_stream_write(&slot->sink,&value,sizeof(value),0,&written);
        if(err==ESP_OK && written!=sizeof(value))err=ESP_ERR_INVALID_RESPONSE;
        if(err!=ESP_OK) {
            /* Provider failures can leave a partial cycle; never replay it. */
            result->rejected_values+=(uint32_t)(count-k);goto reject_written;
        }
        ++result->applied;
        xSemaphoreTake(osc_mutex,portMAX_DELAY);
        slot->info.has_value=true;slot->info.last_value=value;
        slot->info.last_receive_ms=now;++slot->info.received;slot->info.last_error=ESP_OK;
        xSemaphoreGive(osc_mutex);
    }
    goto finish;
reject:
    result->rejected_values+=(uint32_t)count;
reject_written:
    for(size_t k=0;k<count;++k)if(opened[k]) {
        esp_err_t close=solar_os_stream_close_ex(&osc_bindings[indices[k]].sink);
        if(close!=ESP_OK)err=close;
    }
finish:
    xSemaphoreTake(osc_mutex,portMAX_DELAY);
    for(size_t k=0;k<count;++k) {
        solar_os_osc_binding_info_t *info=&osc_bindings[indices[k]].info;
        info->target_claimed=solar_os_stream_handle_valid(&osc_bindings[indices[k]].sink);
        if(err!=ESP_OK) { info->last_error=err;++info->write_errors; }
    }
    xSemaphoreGive(osc_mutex);return ESP_OK;
}
