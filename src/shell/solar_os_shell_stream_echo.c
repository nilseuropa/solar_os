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
static echo_reader_t readers[4]={
    {.handle=SOLAR_OS_STREAM_HANDLE_INIT},{.handle=SOLAR_OS_STREAM_HANDLE_INIT},
    {.handle=SOLAR_OS_STREAM_HANDLE_INIT},{.handle=SOLAR_OS_STREAM_HANDLE_INIT}};
static portMUX_TYPE reader_lock=portMUX_INITIALIZER_UNLOCKED;
static echo_reader_t *echo_reader_acquire(const char *id)
{
    echo_reader_t *chosen=NULL;
    portENTER_CRITICAL(&reader_lock);
    for(size_t i=0;i<4;++i) if(!readers[i].busy && readers[i].handle.slot>=0 && !strcmp(readers[i].handle.id,id)) {
        chosen=&readers[i];break;
    }
    if(!chosen) for(size_t i=0;i<4;++i) if(!readers[i].busy && readers[i].handle.slot<0) { chosen=&readers[i];break; }
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
    const char *usage="usage: stream echo <scalar-source> [--once] [--rate 1..100]";
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
    if(err==ESP_OK && (info.type!=SOLAR_OS_STREAM_TYPE_SCALAR || info.direction==SOLAR_OS_STREAM_DIRECTION_SINK))
        err=ESP_ERR_NOT_SUPPORTED;
    if(err!=ESP_OK) { solar_os_shell_io_printf(io,"stream echo: %s\n",solar_os_shell_error_text(err));return; }
    echo_reader_t *reader=echo_reader_acquire(info.id);
    if(!reader) { solar_os_shell_io_writeln(io,"stream echo: readers busy; retry after another echo stops");return; }
    solar_os_stream_handle_t *handle=&reader->handle;
    if(solar_os_stream_handle_valid(handle)) err=solar_os_stream_close_ex(handle);
    solar_os_stream_read_options_t options={.window_ms=10,.timeout_ms=100};
    bool printed=false,cancelled=false;uint64_t last_sample=0,last_success=solar_os_time_uptime_ms();
    if(err==ESP_OK && !once) solar_os_shell_io_printf(io,"%s (%s); Ctrl+C stops\n",info.id,info.unit[0]?info.unit:"unitless");
    while(err==ESP_OK && !cancelled && !(cancelled=echo_cancelled(ctx,io))) {
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
    if(argc!=4) { solar_os_shell_io_writeln(io,"usage: stream write <scalar-sink> <value>");return; }
    char *end;errno=0;float value=strtof(argv[3],&end);
    if(errno || end==argv[3] || *end || !isfinite(value)) {
        solar_os_shell_io_writeln(io,"stream write: expected a finite number");return;
    }
    solar_os_stream_info_t info;esp_err_t err=solar_os_stream_get_info(argv[2],&info);
    if(err==ESP_OK && (info.type!=SOLAR_OS_STREAM_TYPE_SCALAR ||
        info.direction==SOLAR_OS_STREAM_DIRECTION_SOURCE || strcmp(info.format,"f32"))) err=ESP_ERR_NOT_SUPPORTED;
    if(err!=ESP_OK) { solar_os_shell_io_printf(io,"stream write: %s\n",solar_os_shell_error_text(err));return; }
    echo_reader_t *reader=echo_reader_acquire(info.id);
    if(!reader) { solar_os_shell_io_writeln(io,"stream write: readers busy; retry later");return; }
    solar_os_stream_handle_t *handle=&reader->handle;
    if(solar_os_stream_handle_valid(handle)) err=solar_os_stream_close_ex(handle);
    if(err==ESP_OK) {
        const solar_os_stream_open_options_t options={.direction=SOLAR_OS_STREAM_DIRECTION_SINK,.timeout_ms=100};
        err=solar_os_stream_open_ex(info.id,"stream-write",&options,handle);
        if(err==ESP_OK) {
            size_t written=0;err=solar_os_stream_write(handle,&value,sizeof(value),100,&written);
            if(err==ESP_OK && written!=sizeof(value)) err=ESP_ERR_INVALID_RESPONSE;
        }
    }
    if(solar_os_stream_handle_valid(handle)) {
        esp_err_t close=solar_os_stream_close_ex(handle);
        if(close!=ESP_OK) solar_os_shell_io_printf(io,"stream write: close failed: %s; repeat stream write %s to retry\n",solar_os_shell_error_text(close),info.id);
    }
    portENTER_CRITICAL(&reader_lock);reader->busy=false;portEXIT_CRITICAL(&reader_lock);
    if(err!=ESP_OK) solar_os_shell_io_printf(io,"stream write: %s\n",solar_os_shell_error_text(err));
}
