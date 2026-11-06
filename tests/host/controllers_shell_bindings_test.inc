#define main controllers_registry_suite
#include "controllers_test.c"
#undef main
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include "solar_os_encoder.h"

typedef struct { char output[4096]; size_t used; } solar_os_shell_io_t;
typedef struct { solar_os_shell_io_t io; } solar_os_context_t;
static solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx) { return &ctx->io; }
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...)
{
    va_list args; va_start(args,fmt);
    char formatted[192]; /* Match the real shell's per-call format budget. */
    int n=vsnprintf(formatted,sizeof(formatted),fmt,args);
    assert(n>=0 && (size_t)n<sizeof(formatted));
    assert(io->used+(size_t)n<sizeof(io->output));
    memcpy(io->output+io->used,formatted,(size_t)n+1);
    va_end(args); assert(n>=0 && (size_t)n<sizeof(io->output)-io->used); io->used+=(size_t)n;
}
#define solar_os_shell_io_table_printf solar_os_shell_io_printf
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *text)
{
    size_t n=strlen(text); assert(io->used+n+1<sizeof(io->output));
    memcpy(io->output+io->used,text,n); io->used+=n;
    io->output[io->used++]='\n'; io->output[io->used]='\0';
}
#define solar_os_shell_io_table_writeln solar_os_shell_io_writeln
static const char *solar_os_shell_error_text(esp_err_t error)
{ (void)error; return "device error"; }
enum { SOLAR_OS_JOB_STOPPED,SOLAR_OS_JOB_RUNNING };
typedef struct { int state; } solar_os_job_status_t;
static bool worker_running;
static unsigned worker_starts;
static bool solar_os_jobs_get_by_name(const char *name,solar_os_job_status_t *status)
{ assert(!strcmp(name,"controllers"));status->state=worker_running?SOLAR_OS_JOB_RUNNING:SOLAR_OS_JOB_STOPPED;return true; }
static esp_err_t solar_os_jobs_start(solar_os_context_t *ctx,const char *name,int argc,char **argv)
{ (void)ctx;assert(!strcmp(name,"controllers") && argc==1 && !strcmp(argv[0],name));worker_running=true;worker_starts++;return ESP_OK; }
/* SHELL_COMMAND */

static const char *run(const char *line)
{
    static solar_os_context_t ctx;
    char input_line[256],*argv[16],*save; int argc=0;
    strlcpy(input_line,line,sizeof(input_line));
    for(char *t=strtok_r(input_line," ",&save);t;t=strtok_r(NULL," ",&save)) {
        assert(argc<16); argv[argc++]=t;
    }
    memset(&ctx.io,0,sizeof(ctx.io)); solar_os_shell_cmd_controller(&ctx,argc,argv);
    return ctx.io.output;
}
int main(void)
{
    assert(!*run("controller create loop pid"));
    assert(strstr(run("controller tune loop .01 3 2000"),"Automatic tuning started"));
    assert(strstr(run("controller status loop"),"phase=baseline point=0/3"));
    assert(info().tuning.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING);
    assert(!*run("controller stop loop"));
    assert(strstr(run("controller status loop"),"tune=aborted"));
    assert(!*run("controller reset loop"));
    assert(strstr(run("controller tune loop .01 3 120000"),"Automatic tuning started"));
    float delay[18]={0},y=0;unsigned cursor=0;
    for(unsigned i=0;i<6000 && info().tuning.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i) {
        float output;
        assert(solar_os_controller_step("loop",y,.02f,&output)==ESP_OK);
        y+=(delay[cursor]-y)*(.02f/.25f);delay[cursor]=output;cursor=(cursor+1)%18;
    }
    assert(info().tuning.state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE);
    const char *result=run("controller status loop");
    assert(strstr(result,"candidate kp=") && strstr(result," ki=") && strstr(result,"\n  kd=") && strstr(result," tau="));
    assert(strstr(result,"existing gains unchanged") && strstr(result,"tune=complete"));
    assert(!*run("controller reset loop"));
    assert(strstr(run("controller tune loop nan"),"controller tune <name>"));
    assert(strstr(run("controller tune loop .01 3.5"),"controller tune <name>"));
    assert(strstr(run("controller tune loop .01 3 999"),"controller tune <name>"));
    assert(strstr(run("controller tune loop .01 3 30000 extra"),"controller tune <name>"));
    assert(info().tuning.state==SOLAR_OS_CONTROLLER_TUNE_IDLE);
    assert(strstr(run("controller tune loop"),"Automatic tuning started"));
    assert(!*run("controller stop loop"));
    assert(!worker_starts);
    register_output();
    assert(!*run("controller bind loop manual parameter:plant.drive"));
    assert(strstr(run("controller tune loop"),"Automatic tuning started"));
    assert(worker_running && worker_starts==1);
    assert(!*run("controller stop loop"));
    assert(!*run("controller delete loop"));
    assert(solar_os_parameters_unregister(&registration)==ESP_OK);
    assert(!*run("controller create relay hysteresis"));
    assert(strstr(run("controller tune relay"),"requires a standalone PID"));
    assert(!*run("controller delete relay"));
    solar_os_native_controller_config_v1_t c={.kp=.5f,.derivative_tau=.05f,.max_dt=.2f,
        .output_min=-.2f,.output_max=.2f,.setpoint_min=-3,.setpoint_max=3,.period_ms=20,.timeout_ms=100};
    uint32_t id;
    assert(solar_os_controller_open_owned("wheel","robot",&c,&id)==ESP_OK);
    assert(strstr(run("controller tune wheel"),"unavailable for owned controller (owner=robot)"));
    solar_os_controller_info_t s;
    assert(solar_os_controller_find("wheel",&s)==ESP_OK && s.enabled && s.waiting && !s.tuning.state);
    assert(solar_os_controller_close_owned(id)==ESP_OK);
    puts("Controller shell: tuning arguments, status, abort and ownership rejection passed");
}
