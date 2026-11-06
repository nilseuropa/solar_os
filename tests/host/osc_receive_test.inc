typedef struct {float value;unsigned writes,opens,closes;bool fail,close_fail,short_write,reenter;} sink_state_t;
static sink_state_t sinks[2];
static esp_err_t sink_read(void *user,const solar_os_stream_read_options_t *options,float *value)
{(void)user;(void)options;*value=0;return ESP_OK;}
static esp_err_t sink_open(void *user,const char *owner,const solar_os_stream_open_options_t *o,solar_os_stream_handle_t *h)
{(void)owner;(void)o;(void)h;++((sink_state_t *)user)->opens;return ESP_OK;}
static esp_err_t sink_close(void *user,solar_os_stream_handle_t *h)
{(void)h;sink_state_t *s=user;++s->closes;return s->close_fail?ESP_FAIL:ESP_OK;}
static esp_err_t sink_write(void *user,solar_os_stream_handle_t *h,const void *data,size_t bytes,uint32_t timeout,size_t *written)
{
    (void)h;(void)timeout;assert(bytes==sizeof(float));sink_state_t *s=user;
    if(s->reenter) {
        solar_os_osc_binding_info_t info;assert(solar_os_osc_binding_get(0,&info));
        assert(solar_os_osc_unbind("linear")==ESP_ERR_INVALID_STATE);
        assert(solar_os_osc_receive_release()==ESP_ERR_INVALID_STATE);
    }
    if(s->fail)return ESP_FAIL;
    memcpy(&s->value,data,bytes);++s->writes;*written=s->short_write?bytes-1:bytes;return ESP_OK;
}
static solar_os_stream_driver_t sink_driver(unsigned i)
{
    solar_os_stream_driver_t driver={.info={.provider="test",.format="f32",.unit="m/s",
        .type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_SINK,
        .sharing=SOLAR_OS_STREAM_SHARING_EXCLUSIVE,.has_scalar_range=true,.scalar_minimum=-1,.scalar_maximum=1},
        .open=sink_open,.close_checked=sink_close,.write=sink_write,.user=&sinks[i]};
    strcpy(driver.info.id,i?"angular":"linear");return driver;
}
static solar_os_osc_binding_config_t receiver(const char *name,const char *target,unsigned arg)
{
    solar_os_osc_binding_config_t config={.direction=SOLAR_OS_OSC_RECEIVE,.argument=arg};
    strcpy(config.name,name);strcpy(config.source,target);strcpy(config.address,"/base/cmd_vel");return config;
}
static size_t make_values(uint8_t *packet,const char *address,const char *types,const float *values)
{
    size_t offset=append_string(packet,0,address);offset=append_string(packet,offset,types);
    for(size_t i=1;types[i];++i) {
        uint32_t raw;
        if(types[i]=='f')memcpy(&raw,&values[i-1],sizeof(raw));
        else if(types[i]=='i')raw=(uint32_t)(int32_t)values[i-1];
        else continue;
        write_u32(packet,offset,raw);offset+=4;
    }
    return offset;
}
static solar_os_osc_binding_info_t receive_info(size_t index)
{solar_os_osc_binding_info_t info;assert(solar_os_osc_binding_get(index,&info));return info;}
static void test_receivers(void)
{
    assert(solar_os_osc_clear()==ESP_OK);
    solar_os_stream_driver_t a=sink_driver(0),b=sink_driver(1);
    assert(solar_os_stream_register(&a)==ESP_OK && solar_os_stream_register(&b)==ESP_OK);
    solar_os_osc_binding_config_t left=receiver("linear","linear",0),right=receiver("angular","angular",1);
    assert(solar_os_osc_bind(&left,NULL)==ESP_OK && solar_os_osc_bind(&right,NULL)==ESP_OK);
    assert(!receive_info(0).target_claimed);
    solar_os_osc_binding_info_t info=receive_info(0);
    assert(!solar_os_osc_binding_due(&info,100));
    solar_os_osc_binding_config_t duplicate=left;strcpy(duplicate.name,"duplicate");
    assert(solar_os_osc_bind(&duplicate,NULL)==ESP_ERR_INVALID_STATE);
    duplicate.argument=8;assert(solar_os_osc_bind(&duplicate,NULL)==ESP_ERR_INVALID_ARG);
    strcpy(duplicate.address,"/solaros/parameter/synth/test");duplicate.argument=0;
    assert(solar_os_osc_bind(&duplicate,NULL)==ESP_ERR_INVALID_ARG);
    solar_os_stream_driver_t readonly=a;strcpy(readonly.info.id,"readonly");
    readonly.info.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE;readonly.info.has_scalar_range=false;
    readonly.read_scalar=sink_read;
    assert(solar_os_stream_register(&readonly)==ESP_OK);
    duplicate=receiver("readonly","readonly",0);
    assert(solar_os_osc_bind(&duplicate,NULL)==ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_stream_unregister("readonly")==ESP_OK);
    uint8_t packet[512],element[128];float values[]={.2f,-.3f};
    size_t size=make_values(packet,"/base/cmd_vel",",ff",values);
    solar_os_osc_dispatch_result_t result;
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==2 && result.messages==1);
    assert(sinks[0].value==.2f && sinks[1].value==-.3f && sinks[0].opens==1 && sinks[1].opens==1);
    assert(receive_info(0).target_claimed && receive_info(0).received==1 && receive_info(0).last_receive_ms==100);
    now_ms+=50;
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==2);
    assert(sinks[0].opens==1 && sinks[0].writes==2 && receive_info(0).last_receive_ms==150);
    assert(solar_os_stream_unregister("linear")==ESP_ERR_INVALID_STATE);
    solar_os_stream_handle_t rival=SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open("linear","rival",&rival)==ESP_ERR_INVALID_STATE);
    unsigned written=sinks[0].writes;
    /* Invalid second arguments cannot renew or write the first stream. */
    values[1]=2;size=make_values(packet,"/base/cmd_vel",",ff",values);now_ms+=50;
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==0 && result.rejected_values==2);
    assert(sinks[0].writes==written && receive_info(0).last_receive_ms==150);
    size=make_values(packet,"/base/cmd_vel",",f",values);
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.rejected_values==2 && sinks[0].writes==written);
    values[1]=NAN;size=make_values(packet,"/base/cmd_vel",",ff",values);
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_ERR_INVALID_RESPONSE && sinks[0].writes==written);
    values[1]=-.3f;size=make_values(packet,"/base/cmd_vel",",ff",values);
    assert(solar_os_osc_dispatch_packet(packet,size-1,&result)==ESP_ERR_INVALID_RESPONSE && sinks[0].writes==written);
    packet[size]=0;
    assert(solar_os_osc_dispatch_packet(packet,size+1,&result)==ESP_ERR_INVALID_RESPONSE && sinks[0].writes==written);
    /* A malformed later bundle element invalidates the entire packet. */
    size_t n=make_values(element,"/base/cmd_vel",",ff",values);
    memcpy(packet,"#bundle\0",8);memset(packet+8,0,8);packet[15]=1;
    write_u32(packet,16,(uint32_t)n);memcpy(packet+20,element,n);packet[20+n]=0;
    assert(solar_os_osc_dispatch_packet(packet,21+n,&result)==ESP_ERR_INVALID_RESPONSE && sinks[0].writes==written);
    assert(solar_os_osc_dispatch_packet(packet,20+n,&result)==ESP_OK && result.applied==2);
    written=sinks[0].writes;
    size_t total=16;
    for(unsigned i=0;i<5;++i) {
        write_u32(packet,total,(uint32_t)n);total+=4;memcpy(packet+total,element,n);total+=n;
    }
    assert(solar_os_osc_dispatch_packet(packet,total,&result)==ESP_ERR_INVALID_SIZE && sinks[0].writes==written);
    float mixed[]={-1,0};size=make_values(packet,"/base/cmd_vel",",iT",mixed);
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==2);
    assert(sinks[0].value==-1 && sinks[1].value==1);
    /* No hidden replay: releasing idle writers does not publish a final zero. */
    written=sinks[0].writes;
    assert(solar_os_osc_receive_release()==ESP_OK && sinks[0].writes==written);
    assert(!receive_info(0).target_claimed && solar_os_osc_binding_count()==2);
    assert(solar_os_stream_open("angular","rival",&rival)==ESP_OK);
    size=make_values(packet,"/base/cmd_vel",",ff",values);
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==0 && sinks[0].writes==written);
    assert(!receive_info(0).target_claimed);
    assert(solar_os_stream_close_ex(&rival)==ESP_OK);
    sinks[0].reenter=true;
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==2);
    sinks[0].reenter=false;
    assert(solar_os_osc_receive_release()==ESP_OK);
    /* Missing streams recover on the next packet; old values are never queued. */
    assert(solar_os_stream_unregister("angular")==ESP_OK);written=sinks[0].writes;
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==0 && sinks[0].writes==written);
    assert(solar_os_stream_register(&b)==ESP_OK);
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==2);
    sinks[1].fail=true;
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==1 && result.rejected_values==1);
    sinks[1].fail=false;
    sinks[1].short_write=true;
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_OK && result.applied==1 && receive_info(1).last_error==ESP_ERR_INVALID_RESPONSE);
    sinks[1].short_write=false;
    sinks[1].close_fail=true;
    assert(solar_os_osc_clear()==ESP_FAIL && solar_os_osc_binding_count()==1);
    assert(receive_info(0).target_claimed);
    sinks[1].close_fail=false;
    assert(solar_os_osc_unbind("angular")==ESP_OK && !solar_os_osc_binding_count());
    assert(solar_os_stream_unregister("linear")==ESP_OK && solar_os_stream_unregister("angular")==ESP_OK);
    /* Bounded total writes, even if one address fans out or repeats in a bundle. */
    for(unsigned i=0;i<9;++i) {
        char name[24];snprintf(name,sizeof(name),"missing%u",i);
        solar_os_osc_binding_config_t c=receiver(name,name,0);assert(solar_os_osc_bind(&c,NULL)==ESP_OK);
    }
    size=make_values(packet,"/base/cmd_vel",",ff",values);
    assert(solar_os_osc_dispatch_packet(packet,size,&result)==ESP_ERR_INVALID_SIZE && !result.applied);
    assert(solar_os_osc_clear()==ESP_OK);
    puts("OSC receive: multi-argument mapping, preflight, bounds, ownership, timestamps, recovery and cleanup retry passed");
}
