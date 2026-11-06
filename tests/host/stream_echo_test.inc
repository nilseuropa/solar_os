#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_stream.h"
#include "freertos/task.h"
#define SOLAR_OS_KEY_APP_EXIT 0x1d
#define SOLAR_OS_SHELL_IO_KIND_PORT 1
typedef struct {int port,kind;char output[8192];size_t used;} solar_os_shell_io_t;
typedef struct {bool exit_requested,sleep_requested,deep_sleep_requested;solar_os_shell_io_t io;} solar_os_context_t;
static uint64_t now=100;
static uint64_t cancel_at;
static bool cancelled,cached,fail_close,future,observer;
static unsigned opens,closes,reads;
static unsigned writes;
static const char *reject_open;
static unsigned reject_write,short_write;
static uint64_t disconnect_at;
static bool check_group;
static struct {uint64_t time;float value;bool second;} trace[256];
static unsigned trace_count;
static uint32_t write_delay_ms;
static float last_written;
static float measurement=1.25f;
static esp_err_t sample_error;
uint64_t solar_os_time_uptime_ms(void) {return now;}
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *v) {(void)v;return ESP_ERR_NOT_SUPPORTED;}
void vTaskDelay(TickType_t ticks) {now+=ticks;}
size_t strlcpy(char *dst,const char *src,size_t n) {size_t len=strlen(src);if(n){size_t copy=len<n-1?len:n-1;memcpy(dst,src,copy);dst[copy]=0;}return len;}
static solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *c) {return &c->io;}
static int solar_os_shell_io_kind(solar_os_shell_io_t *io) {return io->kind;}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...) {
    char text[192];va_list args;va_start(args,fmt);int n=vsnprintf(text,sizeof(text),fmt,args);va_end(args);
    assert(n>=0 && n<(int)sizeof(text) && io->used+(size_t)n<sizeof(io->output));
    memcpy(io->output+io->used,text,(size_t)n+1);io->used+=(size_t)n;
}
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *s) {solar_os_shell_io_printf(io,"%s\n",s);}
static const char *solar_os_shell_error_text(esp_err_t e) {
    if(e==ESP_ERR_TIMEOUT)return "timeout";
    if(e==ESP_ERR_INVALID_RESPONSE)return "invalid response";
    if(e==ESP_ERR_NOT_SUPPORTED)return "unsupported";
    return "error";
}
static size_t solar_os_input_read_chars(char *out,size_t n) {
    (void)n;if(cancel_at && now>=cancel_at && !cancelled) {cancelled=true;out[0]=3;return 1;}return 0;
}
static esp_err_t solar_os_port_read(int *p,void *out,size_t n,uint32_t timeout,size_t *count) {
    (void)p;(void)timeout;if(disconnect_at && now>=disconnect_at){*count=0;return ESP_FAIL;}
    *count=solar_os_input_read_chars(out,n);return *count?ESP_OK:ESP_ERR_TIMEOUT;
}
/* COMMAND */
static esp_err_t open_source(void *u,const char *o,const solar_os_stream_open_options_t *p,solar_os_stream_handle_t *h)
{(void)u;(void)o;(void)p;opens++;return reject_open && !strcmp(reject_open,h->id)?ESP_ERR_INVALID_STATE:ESP_OK;}
static esp_err_t close_source(void *u,solar_os_stream_handle_t *h)
{(void)u;(void)h;closes++;return fail_close?ESP_FAIL:ESP_OK;}
static esp_err_t read_source(void *u,const solar_os_stream_read_options_t *p,solar_os_stream_scalar_sample_t *s)
{(void)u;(void)p;reads++;*s=(solar_os_stream_scalar_sample_t){measurement,future?now+1:cached?99:now};return sample_error;}
static esp_err_t read_plain(void *u,const solar_os_stream_read_options_t *p,float *v)
{(void)u;(void)p;reads++;*v=measurement;return ESP_OK;}
static esp_err_t observe_source(void *u,const solar_os_stream_read_options_t *p,solar_os_stream_scalar_sample_t *s)
{return observer?read_source(u,p,s):ESP_ERR_NOT_SUPPORTED;}
static esp_err_t write_scalar(void *u,solar_os_stream_handle_t *h,const void *data,size_t bytes,uint32_t timeout,size_t *written)
{
    (void)u;(void)timeout;assert(bytes==sizeof(float));memcpy(&last_written,data,bytes);writes++;
    assert(trace_count<256);trace[trace_count].time=now;trace[trace_count].value=last_written;
    trace[trace_count++].second=!strcmp(h->id,"sensor2");
    if(check_group) {
        solar_os_stream_info_t info;
        assert(solar_os_stream_get_info("sensor",&info)==ESP_OK && info.active_handles==1);
        assert(solar_os_stream_get_info("sensor2",&info)==ESP_OK && info.active_handles==1);
    }
    now+=write_delay_ms;
    *written=writes==short_write?bytes-1:bytes;
    return writes==reject_write?ESP_FAIL:ESP_OK;
}
static solar_os_stream_driver_t driver={.info={.id="sensor",.provider="test",.format="f32",.unit="rad/s",
    .type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_DUPLEX,
    .sharing=SOLAR_OS_STREAM_SHARING_EXCLUSIVE},.open=open_source,.close_checked=close_source,
    .read_scalar_sample=read_source,.observe_scalar_sample=observe_source,.read_scalar=read_plain,.write=write_scalar};
static const char *run(const char *line,int port) {
    static solar_os_context_t ctx;memset(&ctx,0,sizeof(ctx));ctx.io.kind=port;
    char input[256],*argv[32],*save;int argc=0;strlcpy(input,line,sizeof(input));
    for(char *s=strtok_r(input," ",&save);s;s=strtok_r(NULL," ",&save)) {assert(argc<32);argv[argc++]=s;}
    if(argc>=2 && !strcmp(argv[1],"write")) solar_os_shell_cmd_stream_write(&ctx,argc,argv);
    else solar_os_shell_cmd_stream_echo(&ctx,argc,argv);
    return ctx.io.output;
}
static unsigned lines(const char *s,const char *needle) {unsigned n=0;while((s=strstr(s,needle))) {++n;s+=strlen(needle);}return n;}
static void released(void) {solar_os_stream_info_t i;assert(solar_os_stream_get_info("sensor",&i)==ESP_OK && !i.active_handles);}
static void group_released(void) {
    released();solar_os_stream_info_t info;assert(solar_os_stream_get_info("sensor2",&info)==ESP_OK && !info.active_handles);
}
static void multi_write_tests(void) {
    driver.info.type=SOLAR_OS_STREAM_TYPE_SCALAR;driver.info.direction=SOLAR_OS_STREAM_DIRECTION_DUPLEX;
    solar_os_stream_driver_t second=driver;strcpy(second.info.id,"sensor2");
    second.info.has_scalar_range=true;second.info.scalar_minimum=-1;second.info.scalar_maximum=1;
    assert(solar_os_stream_register(&driver)==ESP_OK && solar_os_stream_register(&second)==ESP_OK);
    check_group=true;trace_count=0;
    assert(!*run("stream write sensor .2 sensor2 -.3",1));
    assert(trace_count==2 && !trace[0].second && trace[1].second && trace[0].value==.2f && trace[1].value==-.3f);group_released();
    unsigned before=opens,before_writes=writes;
    assert(strstr(run("stream write sensor .2 sensor2 nan",1),"sensor2: 'nan'"));
    assert(strstr(run("stream write sensor .2 missing 0",1),"missing:"));
    assert(strstr(run("stream write sensor .2 sensor2 2",1),"sensor2: value 2 outside allowed range"));
    assert(strstr(run("stream write sensor .2 sensor .3",1),"duplicate stream 'sensor'"));
    const char *invalid[]={"--rate 0","--rate 101","--rate 1.5","--rate 20 --rate 20",
        "--duration 1", "--rate 20 --duration 0", "--rate 20 --duration nan", "--rate 20 --duration 86401",
        "--rate 20 --duration 1 --duration 1", "--unknown", "--rate", "--rate 20 --duration"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        char input[256];snprintf(input,sizeof(input),"stream write sensor .2 sensor2 -.3 %s",invalid[i]);
        assert(strstr(run(input,1),"stream write:"));
    }
    assert(opens==before && writes==before_writes);group_released();
    reject_open="sensor2";
    assert(strstr(run("stream write sensor .2 sensor2 -.3",1),"sensor2: open failed"));
    assert(writes==before_writes);group_released();reject_open=NULL;
    for(int port=0;port<=1;++port) {
        trace_count=0;uint64_t start=now;before=opens;
        assert(strstr(run("stream write sensor .2 sensor2 -.3 --rate 20 --duration .12",port),"Publishing 2 streams at 20 Hz"));
        assert(now-start==120 && trace_count==6 && opens==before+2);
        for(unsigned i=0;i<6;++i)assert(trace[i].time==start+(i/2)*50 && trace[i].second==(i%2!=0));
        assert(trace[5].value==-.3f);group_released(); /* No injected zero on exit. */
        trace_count=0;cancelled=false;cancel_at=now+35;start=now;
        run("stream write --rate 1 sensor .2 --duration 10 sensor2 -.3",port);
        assert(cancelled && now-start<50 && trace_count==2);group_released();cancel_at=0;
    }
    trace_count=0;uint64_t start=now;
    run("stream write sensor .2 sensor2 -.3 --rate 60 --duration .1",1);
    assert(now-start==100 && trace_count==12);
    for(unsigned i=2;i<trace_count;i+=2)assert(trace[i].time>trace[i-2].time);
    group_released();
    trace_count=0;disconnect_at=now+35;start=now;
    run("stream write sensor .2 sensor2 -.3 --rate 1",1);
    assert(now-start<50 && trace_count==2);group_released();disconnect_at=0;
    trace_count=0;reject_write=writes+2;
    assert(strstr(run("stream write sensor .2 sensor2 -.3 --rate 20 --duration 1",1),"sensor2: write failed"));
    assert(trace_count==2);group_released();reject_write=0;
    short_write=writes+1;
    assert(strstr(run("stream write sensor .2 sensor2 -.3",1),"sensor: write failed: invalid response"));
    group_released();short_write=0;
    trace_count=0;write_delay_ms=40;start=now;
    run("stream write sensor .2 sensor2 -.3 --rate 100 --duration .1",1);
    assert(trace_count==3 && trace[2].time-start==90);group_released();write_delay_ms=0;
    fail_close=true;assert(strstr(run("stream write sensor .2 sensor2 -.3",1),"close failed"));
    fail_close=false;assert(!*run("stream write sensor .2 sensor2 -.3",1));group_released();
    check_group=false;
    solar_os_stream_driver_t extra=driver;
    for(unsigned i=0;i<8;++i) {
        snprintf(extra.info.id,sizeof(extra.info.id),"s%u",i);
        assert(solar_os_stream_register(&extra)==ESP_OK);
    }
    trace_count=0;
    assert(!*run("stream write s0 1 s1 1 s2 1 s3 1 s4 1 s5 1 s6 1 s7 1",1) && trace_count==8);
    before=opens;
    assert(strstr(run("stream write s0 1 s1 1 s2 1 s3 1 s4 1 s5 1 s6 1 s7 1 s8 1",1),"at most 8 streams"));
    assert(opens==before);
    for(unsigned i=0;i<8;++i) {
        snprintf(extra.info.id,sizeof(extra.info.id),"s%u",i);
        assert(solar_os_stream_unregister(extra.info.id)==ESP_OK);
    }
    assert(solar_os_stream_unregister("sensor")==ESP_OK && solar_os_stream_unregister("sensor2")==ESP_OK);
    puts("stream write: grouped validation/open, periodic timing, cancellation/disconnect, failures and cleanup retry passed");
}
int main(void) {
    assert(solar_os_stream_register(&driver)==ESP_OK);
    assert(!*run("stream write sensor .5",1) && writes==1 && last_written==.5f);released();
    assert(strstr(run("stream write sensor nan",1),"finite number") && writes==1);
    assert(strstr(run("stream write sensor inf",1),"finite number") && writes==1);
    assert(strstr(run("stream write sensor .5 extra",1),"usage:"));
    assert(strstr(run("stream write missing .5",1),"missing:"));
    assert(solar_os_stream_unregister("sensor")==ESP_OK);
    driver.info.has_scalar_range=true;driver.info.scalar_minimum=-.058f;driver.info.scalar_maximum=.058f;
    driver.info.unit[0]=0;strcpy(driver.info.unit,"m/s");
    assert(solar_os_stream_register(&driver)==ESP_OK);
    unsigned writes_before=writes,opens_before=opens;
    assert(strstr(run("stream write sensor .5",1),"sensor: value 0.5 outside allowed range [-0.058, 0.058] m/s"));
    assert(strstr(run("stream write sensor -.5",1),"value -0.5 outside allowed range"));
    assert(writes==writes_before && opens==opens_before);released();
    assert(!*run("stream write sensor .058",1) && last_written==.058f);released();
    assert(!*run("stream write sensor -.058",1) && last_written==-.058f);released();
    assert(solar_os_stream_unregister("sensor")==ESP_OK);
    driver.info.has_scalar_range=false;strcpy(driver.info.unit,"rad/s");
    driver.info.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE;
    assert(solar_os_stream_register(&driver)==ESP_OK);
    assert(strstr(run("stream write sensor 0",1),"sensor is read-only"));released();
    assert(solar_os_stream_unregister("sensor")==ESP_OK);
    driver.info.direction=SOLAR_OS_STREAM_DIRECTION_DUPLEX;
    assert(solar_os_stream_register(&driver)==ESP_OK);
    const char *s=run("stream echo sensor --once",0);
    assert(strstr(s,"sample-ms=100 age-ms=0 value=1.25 rad/s") && !strstr(s,"stream echo:"));released();
    for(int port=0;port<=1;++port) {
        cancelled=false;cancel_at=now+35;
        s=run("stream echo sensor --rate 1",port);
        assert(cancelled && now<cancel_at+15 && lines(s,"value=")==1);released();cancel_at=0;
    }
    unsigned before=opens;
    assert(strstr(run("stream echo sensor --rate 0",0),"usage:"));
    assert(strstr(run("stream echo sensor --rate 1.5",0),"usage:"));
    assert(strstr(run("stream echo sensor --once --once",0),"usage:"));assert(opens==before);
    solar_os_stream_handle_t owner=SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open("sensor","controller",&owner)==ESP_OK);
    observer=true;cached=true;before=opens;unsigned before_close=closes;
    s=run("stream echo sensor --once",0);assert(strstr(s,"sample-ms=99") && opens==before && closes==before_close);
    solar_os_stream_info_t info;assert(solar_os_stream_get_info("sensor",&info)==ESP_OK && info.active_handles==1 && !strcmp(info.owner,"controller"));
    assert(solar_os_stream_close_ex(&owner)==ESP_OK);observer=false;cached=false;
    future=true;assert(strstr(run("stream echo sensor --once",0),"invalid response"));released();future=false;
    measurement=NAN;assert(strstr(run("stream echo sensor --once",0),"invalid response"));released();measurement=1.25f;
    sample_error=ESP_ERR_TIMEOUT;uint64_t start=now;
    assert(strstr(run("stream echo sensor --once",0),"timeout") && now-start>=1000);released();sample_error=ESP_OK;
    fail_close=true;assert(strstr(run("stream echo sensor --once",0),"close failed"));
    assert(solar_os_stream_get_info("sensor",&info)==ESP_OK && info.active_handles==1);
    fail_close=false;assert(!strstr(run("stream echo sensor --once",0),"failed"));released();
    assert(solar_os_stream_unregister("sensor")==ESP_OK);
    driver.read_scalar_sample=NULL;assert(solar_os_stream_register(&driver)==ESP_OK);
    assert(strstr(run("stream echo sensor --once",0),"sample-ms=unavailable value=1.25"));released();
    assert(solar_os_stream_unregister("sensor")==ESP_OK);
    driver.info.type=SOLAR_OS_STREAM_TYPE_EVENT;driver.read_scalar_sample=read_source;
    assert(solar_os_stream_register(&driver)==ESP_OK);before=opens;
    assert(strstr(run("stream echo sensor --once",0),"unsupported") && opens==before);
    assert(solar_os_stream_unregister("sensor")==ESP_OK);
    multi_write_tests();
    puts("stream echo: scalar/timestamps, observer ownership, cancellation, validation, timeout and cleanup retry passed");
}
