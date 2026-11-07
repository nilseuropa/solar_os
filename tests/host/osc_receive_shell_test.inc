#define main osc_service_tests
#include "osc_test.c"
#undef main
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
typedef struct {char text[8192];size_t used;} solar_os_shell_io_t;
typedef struct {solar_os_shell_io_t io;} solar_os_context_t;
static solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx){return &ctx->io;}
static const char *solar_os_shell_error_text(esp_err_t err){return err==ESP_OK?"no error":"error";}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *format,...)
{
    va_list args;va_start(args,format);int bytes=vsnprintf(io->text+io->used,sizeof(io->text)-io->used,format,args);va_end(args);
    assert(bytes>=0 && (size_t)bytes<sizeof(io->text)-io->used);io->used+=(size_t)bytes;
}
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *text){solar_os_shell_io_printf(io,"%s\n",text);}
#define solar_os_shell_io_table_printf solar_os_shell_io_printf
#define solar_os_shell_io_table_writeln solar_os_shell_io_writeln
static void solar_os_shell_diag_problem(solar_os_shell_io_t *io,const char *domain,const char *problem,...)
{solar_os_shell_io_printf(io,"%s: %s\n",domain,problem);}
static void solar_os_shell_diag_invalid(solar_os_shell_io_t *io,const char *domain,const char *field,...)
{solar_os_shell_io_printf(io,"%s: invalid %s\n",domain,field);}
static void solar_os_shell_diag_unexpected(solar_os_shell_io_t *io,const char *domain,const char *value,...)
{solar_os_shell_io_printf(io,"%s: unexpected %s\n",domain,value);}
/* COMMAND */
static const char *run(const char *line)
{
    static solar_os_context_t ctx;memset(&ctx,0,sizeof(ctx));
    char input[256],*argv[20],*save;int argc=0;strlcpy(input,line,sizeof(input));
    for(char *s=strtok_r(input," ",&save);s;s=strtok_r(NULL," ",&save)){assert(argc<20);argv[argc++]=s;}
    solar_os_shell_cmd_osc(&ctx,argc,argv);return ctx.io.text;
}
int main(void)
{
    osc_service_tests();memset(sinks,0,sizeof(sinks));
    solar_os_stream_driver_t a=sink_driver(0),b=sink_driver(1);
    assert(solar_os_stream_register(&a)==ESP_OK && solar_os_stream_register(&b)==ESP_OK);
    assert(strstr(run("osc bind linear receive linear /base/cmd_vel"),"binding linear created"));
    assert(strstr(run("osc bind angular receive angular /base/cmd_vel arg=1"),"binding angular created"));
    assert(!receive_info(0).target_claimed);
    const char *bad[]={"arg=8","arg=-1","arg=1 arg=0","rate=20","edge=both","arg=1.5"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        char line[256];snprintf(line,sizeof(line),"osc bind bad receive missing /cmd %s",bad[i]);
        assert(strstr(run(line),"invalid receive option"));
    }
    assert(solar_os_osc_binding_count()==2);
    uint8_t packet[128];float values[]={.2f,-.3f};solar_os_osc_dispatch_result_t result;
    size_t bytes=make_values(packet,"/base/cmd_vel",",ff",values);
    assert(solar_os_osc_dispatch_packet(packet,bytes,&result)==ESP_OK && result.applied==2);
    const char *out=run("osc bindings");
    assert(strstr(out,"linear: receive /base/cmd_vel arg=0 -> linear"));
    assert(strstr(out,"angular: receive /base/cmd_vel arg=1 -> angular"));
    assert(strstr(out,"claimed=1 received=1"));
    sinks[1].close_fail=true;
    assert(strstr(run("osc clear"),"clear; close failed or binding busy; retry failed"));
    assert(solar_os_osc_binding_count()==1 && receive_info(0).target_claimed);
    sinks[1].close_fail=false;assert(strstr(run("osc unbind angular"),"binding angular removed"));
    assert(strstr(run("osc bind telemetry stream missing /telemetry rate=2"),"binding telemetry created"));
    assert(receive_info(0).config.direction==SOLAR_OS_OSC_SEND && receive_info(0).config.interval_ms==500);
    assert(strstr(run("osc clear"),"bindings cleared"));
    assert(solar_os_stream_unregister("linear")==ESP_OK && solar_os_stream_unregister("angular")==ESP_OK);
    puts("OSC shell: receive syntax, indices, listing, cleanup errors and outbound compatibility passed");
}
