#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_stream.h"
#include "solar_os_time.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Keep failed closes reachable for a later invocation to retry. A slot is
 * held by a foreground reader, not by a background sampling task. */
typedef struct { bool busy;solar_os_stream_handle_t handle; } echo_reader_t;
#define STREAM_CLI_HANDLES 8
static echo_reader_t readers[STREAM_CLI_HANDLES]={
    {.handle=SOLAR_OS_STREAM_HANDLE_INIT},{.handle=SOLAR_OS_STREAM_HANDLE_INIT},
    {.handle=SOLAR_OS_STREAM_HANDLE_INIT},{.handle=SOLAR_OS_STREAM_HANDLE_INIT},
    {.handle=SOLAR_OS_STREAM_HANDLE_INIT},{.handle=SOLAR_OS_STREAM_HANDLE_INIT},
    {.handle=SOLAR_OS_STREAM_HANDLE_INIT},{.handle=SOLAR_OS_STREAM_HANDLE_INIT}};
static portMUX_TYPE reader_lock=portMUX_INITIALIZER_UNLOCKED;
static echo_reader_t *echo_reader_acquire(const char *id)
{
    echo_reader_t *chosen=NULL;
    portENTER_CRITICAL(&reader_lock);
    for(size_t i=0;i<STREAM_CLI_HANDLES;++i) if(!readers[i].busy && readers[i].handle.slot>=0 && !strcmp(readers[i].handle.id,id)) {
        chosen=&readers[i];break;
    }
    if(!chosen) for(size_t i=0;i<STREAM_CLI_HANDLES;++i) if(!readers[i].busy && readers[i].handle.slot<0) { chosen=&readers[i];break; }
    if(chosen) chosen->busy=true;
    portEXIT_CRITICAL(&reader_lock);return chosen;
}
static bool echo_cancelled(solar_os_context_t *ctx,solar_os_shell_io_t *io)
{
    if(ctx->exit_requested || ctx->sleep_requested || ctx->deep_sleep_requested) return true;
    char chars[16];size_t count=0;
    if(solar_os_shell_io_kind(io)==SOLAR_OS_SHELL_IO_KIND_PORT) {
        esp_err_t err=solar_os_port_read(&io->port,(uint8_t *)chars,sizeof(chars),0,&count);
        if(err!=ESP_OK && err!=ESP_ERR_TIMEOUT) return true;
    } else count=solar_os_input_read_chars(chars,sizeof(chars));
    for(size_t i=0;i<count;++i)
        if((uint8_t)chars[i]==SOLAR_OS_KEY_APP_EXIT || chars[i]==0x1d || chars[i]==0x03 || chars[i]==0x1b) return true;
    return false;
}

void solar_os_shell_cmd_stream_echo(solar_os_context_t *ctx,int argc,char **argv)
{
    solar_os_shell_io_t *io=solar_os_shell_command_io(ctx);
    const char *usage="usage: stream echo <source> [--once] [--rate 1..100]";
    bool once=false,rate_set=false;unsigned rate=10;
    if(argc<3) { solar_os_shell_io_writeln(io,usage);return; }
    for(int i=3;i<argc;++i) {
        if(!strcmp(argv[i],"--once") && !once) once=true;
        else if(!strcmp(argv[i],"--rate") && !rate_set && i+1<argc) {
            char *end;errno=0;unsigned long value=strtoul(argv[++i],&end,10);
            if(errno || end==argv[i] || *end || value<1 || value>100) { solar_os_shell_io_writeln(io,usage);return; }
            rate=(unsigned)value;rate_set=true;
        } else { solar_os_shell_io_writeln(io,usage);return; }
    }
    solar_os_stream_info_t info;
    esp_err_t err=solar_os_stream_get_info(argv[2],&info);
    if(err==ESP_OK && (info.type==SOLAR_OS_STREAM_TYPE_AUDIO || info.type==SOLAR_OS_STREAM_TYPE_VIDEO || info.direction==SOLAR_OS_STREAM_DIRECTION_SINK))
        err=ESP_ERR_NOT_SUPPORTED;
    if(err!=ESP_OK) { solar_os_shell_io_printf(io,"stream echo: %s\n",solar_os_shell_error_text(err));return; }
    echo_reader_t *reader=echo_reader_acquire(info.id);
    if(!reader) { solar_os_shell_io_writeln(io,"stream echo: readers busy; retry after another echo stops");return; }
    solar_os_stream_handle_t *handle=&reader->handle;
    if(solar_os_stream_handle_valid(handle)) err=solar_os_stream_close_ex(handle);
    solar_os_stream_read_options_t options={.window_ms=10,.timeout_ms=100};
    const bool scalar=info.type==SOLAR_OS_STREAM_TYPE_SCALAR;
    if(err==ESP_OK && !scalar) {
        err=solar_os_stream_open(info.id,"stream-echo",handle);
        char header[SOLAR_OS_STREAM_CSV_HEADER_MAX];
        if(err==ESP_OK) err=solar_os_stream_csv_header(&info,header,sizeof(header));
        if(err==ESP_OK) solar_os_shell_io_writeln(io,header);
    }
    bool printed=false,cancelled=false;uint64_t last_sample=0,last_success=solar_os_time_uptime_ms();
    if(err==ESP_OK && !once) solar_os_shell_io_printf(io,"%s (%s); Ctrl+C stops\n",info.id,info.unit[0]?info.unit:"unitless");
    while(err==ESP_OK && !cancelled && !(cancelled=echo_cancelled(ctx,io))) {
        if(!scalar) {
            solar_os_stream_csv_record_t record;
            err=solar_os_stream_read_csv(handle,&options,&record);
            uint64_t now=solar_os_time_uptime_ms();
            if(err==ESP_OK && record.has_data) {
                solar_os_shell_io_writeln(io,record.line);last_success=now;
                if(once) break;
            } else if(err!=ESP_OK && err!=ESP_ERR_TIMEOUT) break;
            else if(now-last_success>=1000) { err=ESP_ERR_TIMEOUT;break; }
            err=ESP_OK;
        } else {
        solar_os_stream_scalar_sample_t sample={0};bool timestamped=true;
        if(solar_os_stream_handle_valid(handle)) err=solar_os_stream_read_scalar_sample(handle,&options,&sample);
        else {
            err=solar_os_stream_observe_scalar_sample(info.id,&options,&sample);
            if(err==ESP_ERR_NOT_SUPPORTED) {
                solar_os_stream_open_options_t open={.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE,.timeout_ms=100};
                err=solar_os_stream_open_ex(info.id,"stream-echo",&open,handle);
                if(err==ESP_OK) err=solar_os_stream_read_scalar_sample(handle,&options,&sample);
            }
        }
        if(err==ESP_ERR_NOT_SUPPORTED && solar_os_stream_handle_valid(handle)) {
            timestamped=false;err=solar_os_stream_read_scalar(handle,&options,&sample.value);
        }
        uint64_t now=solar_os_time_uptime_ms();
        if(err==ESP_OK && (!isfinite(sample.value) || (timestamped && sample.uptime_ms>now))) err=ESP_ERR_INVALID_RESPONSE;
        if(err==ESP_OK) {
            last_success=now;
            if(!timestamped || !printed || sample.uptime_ms!=last_sample) {
                if(timestamped) solar_os_shell_io_printf(io,"sample-ms=%" PRIu64 " age-ms=%" PRIu64 " value=%.9g %s\n",
                    sample.uptime_ms,now-sample.uptime_ms,(double)sample.value,info.unit);
                else solar_os_shell_io_printf(io,"sample-ms=unavailable value=%.9g %s\n",(double)sample.value,info.unit);
                printed=true;last_sample=sample.uptime_ms;
                if(once) break;
            }
        } else if(err!=ESP_ERR_TIMEOUT || now-last_success>=1000) break;
        else err=ESP_OK;
        }
        uint32_t delay=once?10:1000/rate;
        /* Poll cancellation independently of the output rate. */
        while(delay && !(cancelled=echo_cancelled(ctx,io))) {
            uint32_t chunk=delay>10?10:delay;TickType_t ticks=pdMS_TO_TICKS(chunk);
            vTaskDelay(ticks?ticks:1);delay-=chunk;
        }
    }
    if(solar_os_stream_handle_valid(handle)) {
        esp_err_t close=solar_os_stream_close_ex(handle);
        if(close!=ESP_OK) solar_os_shell_io_printf(io,"stream echo: close failed: %s; repeat stream echo %s to retry\n",solar_os_shell_error_text(close),info.id);
    }
    portENTER_CRITICAL(&reader_lock);reader->busy=false;portEXIT_CRITICAL(&reader_lock);
    if(err!=ESP_OK) solar_os_shell_io_printf(io,"stream echo: %s\n",solar_os_shell_error_text(err));
}

void solar_os_shell_cmd_stream_write(solar_os_context_t *ctx,int argc,char **argv)
{
    solar_os_shell_io_t *io=solar_os_shell_command_io(ctx);
    const char *usage="usage: stream write <scalar-sink> <value> [<scalar-sink> <value> ...] [--rate 1..100] [--duration seconds]";
    struct { const char *id;float value;echo_reader_t *reader; } items[STREAM_CLI_HANDLES]={0};
    unsigned count=0,rate=0;uint64_t duration_ms=0;
    if(argc<4) { solar_os_shell_io_writeln(io,usage);return; }
    for(int i=2;i<argc;++i) {
        if(!strcmp(argv[i],"--rate")) {
            if(rate || i+1>=argc) { solar_os_shell_io_writeln(io,"stream write: --rate requires one integer from 1 to 100");return; }
            char *end;errno=0;unsigned long value=strtoul(argv[++i],&end,10);
            if(errno || end==argv[i] || *end || value<1 || value>100) {
                solar_os_shell_io_printf(io,"stream write: invalid rate '%s'; expected an integer from 1 to 100 Hz\n",argv[i]);return;
            }
            rate=(unsigned)value;continue;
        }
        if(!strcmp(argv[i],"--duration")) {
            if(duration_ms || i+1>=argc) { solar_os_shell_io_writeln(io,"stream write: --duration requires one value in seconds");return; }
            char *end;errno=0;double seconds=strtod(argv[++i],&end);
            if(errno || end==argv[i] || *end || !isfinite(seconds) || seconds<.001 || seconds>86400) {
                solar_os_shell_io_printf(io,"stream write: invalid duration '%s'; expected 0.001..86400 seconds\n",argv[i]);return;
            }
            duration_ms=(uint64_t)(seconds*1000+.5);continue;
        }
        if(!strncmp(argv[i],"--",2)) { solar_os_shell_io_printf(io,"stream write: unknown option '%s'\n",argv[i]);return; }
        if(count==STREAM_CLI_HANDLES) { solar_os_shell_io_printf(io,"stream write: at most %u streams per command\n",(unsigned)STREAM_CLI_HANDLES);return; }
        const char *id=argv[i];
        for(unsigned k=0;k<count;++k) if(!strcmp(id,items[k].id)) {
            solar_os_shell_io_printf(io,"stream write: duplicate stream '%s'\n",id);return;
        }
        if(i+1>=argc) { solar_os_shell_io_printf(io,"stream write: missing value for %s\n",id);solar_os_shell_io_writeln(io,usage);return; }
        char *end;errno=0;float value=strtof(argv[++i],&end);
        if(errno || end==argv[i] || *end || !isfinite(value)) {
            solar_os_shell_io_printf(io,"stream write: %s: '%s' is not a finite number\n",id,argv[i]);return;
        }
        items[count].id=id;items[count++].value=value;
    }
    if(!count) { solar_os_shell_io_writeln(io,usage);return; }
    if(duration_ms && !rate) { solar_os_shell_io_writeln(io,"stream write: --duration requires --rate");return; }
    /* Validate the whole group before claiming any endpoint. */
    for(unsigned i=0;i<count;++i) {
        solar_os_stream_info_t info;esp_err_t err=solar_os_stream_get_info(items[i].id,&info);
        if(err!=ESP_OK) { solar_os_shell_io_printf(io,"stream write: %s: %s\n",items[i].id,solar_os_shell_error_text(err));return; }
        if(info.type!=SOLAR_OS_STREAM_TYPE_SCALAR || strcmp(info.format,"f32")) {
            solar_os_shell_io_printf(io,"stream write: %s: expected a float32 scalar stream\n",info.id);return;
        }
        if(info.direction==SOLAR_OS_STREAM_DIRECTION_SOURCE) {
            solar_os_shell_io_printf(io,"stream write: %s is read-only\n",info.id);return;
        }
        if(info.has_scalar_range && (items[i].value<info.scalar_minimum || items[i].value>info.scalar_maximum)) {
            solar_os_shell_io_printf(io,"stream write: %s: value %.6g outside allowed range [%.6g, %.6g]%s%s\n",
                info.id,(double)items[i].value,(double)info.scalar_minimum,(double)info.scalar_maximum,
                info.unit[0]?" ":"",info.unit);return;
        }
    }
    esp_err_t err=ESP_OK;unsigned failed=0;
    const char *operation="open";
    for(unsigned i=0;i<count;++i) {
        failed=i;items[i].reader=echo_reader_acquire(items[i].id);
        if(!items[i].reader) { solar_os_shell_io_printf(io,"stream write: %s: stream handles busy; retry later\n",items[i].id);goto cleanup; }
        solar_os_stream_handle_t *handle=&items[i].reader->handle;
        if(solar_os_stream_handle_valid(handle))err=solar_os_stream_close_ex(handle);
        if(err==ESP_OK) {
            const solar_os_stream_open_options_t options={.direction=SOLAR_OS_STREAM_DIRECTION_SINK,.timeout_ms=100};
            err=solar_os_stream_open_ex(items[i].id,"stream-write",&options,handle);
        }
        if(err!=ESP_OK)goto cleanup;
    }
    if(rate)solar_os_shell_io_printf(io,"Publishing %u streams at %u Hz; Ctrl+C stops\n",count,rate);
    uint64_t started=solar_os_time_uptime_ms();bool cancelled=false;
    operation="write";
    do {
        if(rate && (echo_cancelled(ctx,io) || (duration_ms && solar_os_time_uptime_ms()-started>=duration_ms)))break;
        for(unsigned i=0;i<count;++i) {
            if(rate && (echo_cancelled(ctx,io) || (duration_ms && solar_os_time_uptime_ms()-started>=duration_ms)))goto cleanup;
            failed=i;
            size_t written=0;err=solar_os_stream_write(&items[i].reader->handle,&items[i].value,sizeof(float),100,&written);
            if(err==ESP_OK && written!=sizeof(float))err=ESP_ERR_INVALID_RESPONSE;
            if(err!=ESP_OK)goto cleanup;
        }
        if(!rate)break;
        uint64_t elapsed=solar_os_time_uptime_ms()-started;
        /* Skip missed periods instead of issuing a burst of old commands. */
        uint64_t due=started+(((elapsed*rate/1000+1)*1000+rate-1)/rate);
        if(duration_ms && due>started+duration_ms)due=started+duration_ms;
        uint64_t current;
        while((current=solar_os_time_uptime_ms())<due && !(cancelled=echo_cancelled(ctx,io))) {
            uint64_t remaining=due-current;
            uint32_t chunk=(uint32_t)(remaining>10?10:remaining);
            TickType_t ticks=pdMS_TO_TICKS(chunk);vTaskDelay(ticks?ticks:1);
        }
    } while(!cancelled);
cleanup:
    if(err!=ESP_OK)solar_os_shell_io_printf(io,"stream write: %s: %s failed: %s\n",items[failed].id,operation,solar_os_shell_error_text(err));
    for(unsigned i=0;i<count;++i) if(items[i].reader) {
        solar_os_stream_handle_t *handle=&items[i].reader->handle;
        if(solar_os_stream_handle_valid(handle)) {
            esp_err_t close=solar_os_stream_close_ex(handle);
            if(close!=ESP_OK)solar_os_shell_io_printf(io,"stream write: %s: close failed: %s; repeat stream write %s %.9g to retry\n",
                items[i].id,solar_os_shell_error_text(close),items[i].id,(double)items[i].value);
        }
        portENTER_CRITICAL(&reader_lock);items[i].reader->busy=false;portEXIT_CRITICAL(&reader_lock);
    }
}
