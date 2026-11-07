/* Run the production shell dispatcher through the real frontend and engine. */
#define main robot_registry_suite
#include "robot_integration_test.c"
#undef main
#include <errno.h>
#include <stdarg.h>

typedef struct { char output[4096]; size_t used; } solar_os_shell_io_t;
typedef struct { solar_os_shell_io_t io; } solar_os_context_t;
static solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx) { return &ctx->io; }
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...)
{
    va_list args; va_start(args,fmt);
    int n=vsnprintf(io->output+io->used,sizeof(io->output)-io->used,fmt,args);
    va_end(args); assert(n>=0 && (size_t)n<sizeof(io->output)-io->used); io->used+=(size_t)n;
}
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *text)
{ solar_os_shell_io_printf(io,"%s\n",text); }
static const char *solar_os_shell_error_text(esp_err_t error)
{ (void)error; return "device error"; }
static esp_err_t solar_os_shell_resolve_path(solar_os_context_t *ctx, const char *path, char *out, size_t bytes)
{ (void)ctx;return strlcpy(out,path,bytes)<bytes?ESP_OK:ESP_ERR_INVALID_ARG; }

/* SHELL_COMMAND */

static solar_os_context_t ctx;
static const char *run(const char *line)
{
    char input[256],*argv[16],*save; int argc=0;
    strlcpy(input,line,sizeof(input));
    for (char *t=strtok_r(input," ",&save);t;t=strtok_r(NULL," ",&save)) {
        assert(argc<16); argv[argc++]=t;
    }
    memset(&ctx.io,0,sizeof(ctx.io)); solar_os_shell_cmd_robot(&ctx,argc,argv);
    return ctx.io.output;
}
static void drive_shell_check(void)
{
    assert(strstr(run("robot diff_drive status base"),"usage: robot start|stop | joint|model"));
    assert(strstr(run("robot diff_drive command base 0 0"),"usage: robot start|stop | joint|model"));
}
int main(void)
{
    setup();
    assert(strstr(run("robot"),"robot start|stop"));
    assert(strstr(run("robot model create wheel"),"robot model load"));
    assert(strstr(run("robot axis list"),"usage:"));
    assert(strstr(run("robot create wheel velocity"),"usage:"));
    assert(strstr(run("robot model load " ROBOT_MODEL_FIXTURE),"Robot model loaded"));
    assert(strstr(run("robot model info"),"solbot: root=base_footprint links=5 joints=4 known=2"));
    assert(strstr(run("robot model links"),"left_wheel_link"));
    assert(strstr(run("robot model joints"),"position=0 known=0"));
    assert(strstr(run("robot model transform base_link left_wheel_link"),"Supply positions"));
    assert(!*run("robot model set left_wheel_joint 0 right_wheel_joint 0"));
    assert(strstr(run("robot model transform base_link left_wheel_link"),"base_link <- left_wheel_link"));
    assert(strstr(run("robot model point base_link left_wheel_link 0 0 0"),"0.035064"));
    assert(strstr(run("robot model vector base_link left_wheel_link 1 0 0"),"[1 0 0]"));
    assert(strstr(run("robot model load /missing.urdf"),"robot model:"));
    assert(strstr(run("robot model info"),"solbot:"));
    assert(!*run("robot model unload"));
    assert(!solar_os_robot_joint_count());
    assert(strstr(run("robot joint"),"robot joint configure"));
    assert(strstr(run("robot joint list"),"No joints open"));
    assert(!*run("robot joint create wheel velocity"));
    assert(strstr(run("robot joint status wheel"),"calibration incomplete"));
    assert(strstr(run("robot joint enable wheel"),"Missing calibration"));
    assert(!*run("robot joint configure wheel ppr 7"));
    assert(strstr(run("robot joint enable wheel"),"Missing calibration"));
    assert(!*run("robot joint configure wheel gear_ratio 30"));
    assert(strstr(run("robot joint configure wheel encoder_counts_per_rev 2098"),"robot joint:"));
    assert(!*run("robot joint configure wheel friction_bias_forward 0.04"));
    assert(!*run("robot joint configure wheel friction_bias_reverse 0.03"));
    assert(!*run("robot joint motor wheel left left_enc"));
    solar_os_robot_joint_info_v3_t s;
    assert(solar_os_robot_joint_status("wheel",&s)==ESP_OK);
    assert(s.config.ppr==7 && s.config.gear_ratio==30 && !strcmp(s.config.encoder,"left_enc"));
    assert(fabsf(s.config.friction_bias_forward-.04f)<1e-6f && fabsf(s.config.friction_bias_reverse-.03f)<1e-6f);
    assert(strstr(run("robot joint configure wheel bad_key 1"),"robot joint:"));
    assert(strstr(run("robot joint configure wheel kp"),"robot joint configure"));
    assert(!*run("robot joint enable wheel"));
    assert(strstr(run("robot joint status wheel"),"encoder-counts-per-joint-rev=210 (derived)"));
    for (int i=0;i<50;i++) { time_value+=20000; lock();cycle(find(1));unlock(); }
    assert(strstr(run("robot joint status wheel"),"waiting for first command"));
    assert(solar_os_robot_joint_status("wheel",&s)==ESP_OK && s.output==0 && !s.command_valid);
    assert(!*run("robot joint command wheel 1.5"));
    assert(solar_os_robot_joint_status("wheel",&s)==ESP_OK && s.target==1.5f && s.claimed);
    assert(strstr(run("robot joint status wheel"),"friction-bias-forward="));
    assert(strstr(run("robot joint list"),"wheel: velocity active"));
    for (int i=0;i<15;i++) { time_value+=20000; lock();cycle(find(1));unlock(); }
    assert(strstr(run("robot joint status wheel"),"send a fresh command to resume"));
    assert(!*run("robot joint command wheel 1"));
    assert(solar_os_robot_joint_status("wheel",&s)==ESP_OK && s.state==SOLAR_OS_ROBOT_ACTIVE);
    assert(strstr(run("robot joint configure wheel kp 1"),"Joint owns hardware"));
    assert(!*run("robot joint disable wheel"));
    assert(!*run("robot joint close wheel"));
    assert(!*run("robot joint create pan servo"));
    assert(!*run("robot joint servo pan native 2 1000 2000 -1 1 60"));
    assert(solar_os_robot_joint_status("pan",&s)==ESP_OK && s.config.servo.output==2 && s.config.servo.frequency_hz==60);
    assert(s.config.servo.pulse_min_us==1000 && s.config.servo.pulse_max_us==2000 && s.config.servo.angle_min==-1);
    assert(!*run("robot joint command pan 0.5"));
    assert(!*run("robot joint close pan"));
    assert(solar_os_robot_model_load_xml(model_velocity_fixture,strlen(model_velocity_fixture),NULL,0)==ESP_OK);
    assert(strstr(run("robot start"),"controllers started"));
    assert(strstr(run("robot joint list"),"model_wheel: velocity waiting"));
    assert(strstr(run("robot model unload"),"Run robot stop"));
    assert(strstr(run("robot stop"),"controllers stopped"));
    assert(!*run("robot model unload"));
    drive_binding_check=drive_shell_check;diff_drive_integration_test();
    teardown(); puts("Robot shell: joint namespace, argument routing, calibration and lifecycle passed");
    return 0;
}
