#include "solar_os_shell_commands.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_robot.h"
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char usage[] =
    "robot start | stop (URDF-declared controllers)\n"
    "robot joint list | status <joint>\n"
    "  robot joint create <joint> velocity|servo\n"
    "  robot joint configure <joint> <key> <value>\n"
    "  robot joint motor <joint> <motor> <encoder>\n"
    "  robot joint servo <joint> <provider> <output> <min-us> <max-us> <min-rad> <max-rad> [freq-hz]\n"
    "  robot joint command <joint> <rad/s-or-rad>\n"
    "  robot joint enable|disable|close <joint>\n";
static const char model_usage[] =
    "robot model load <file.urdf> | unload | info | links | joints | metadata\n"
    "  robot model set <joint> <position> [<joint> <position> ...]\n"
    "  robot model transform <target-frame> <source-frame>\n"
    "  robot model point|vector <target-frame> <source-frame> <x> <y> <z>\n";
static bool number(const char *s, float *value)
{
    char *end; errno=0; *value=strtof(s,&end);
    return errno==0 && end!=s && *end==0 && isfinite(*value);
}
static void show(solar_os_shell_io_t *io,const solar_os_robot_joint_info_v3_t *s)
{
    solar_os_shell_io_printf(io,
        "%s: %s %s claimed=%u steps=%u faults=%u error=%d\n",
        s->config.name,s->config.kind==SOLAR_OS_ROBOT_MOTOR_VELOCITY?"velocity":"servo",
        s->state==SOLAR_OS_ROBOT_ACTIVE?"active":s->state==SOLAR_OS_ROBOT_WAITING?"waiting":s->state==SOLAR_OS_ROBOT_FAULT?"fault":"ready",
        (unsigned)s->claimed,(unsigned)s->steps,(unsigned)s->faults,(int)s->last_error);
    solar_os_shell_io_printf(io,
        "  motor=%s encoder=%s ppr=%.6g decoding=x%u ratio=%.6g motor-polarity=%d encoder-polarity=%d encoder-on-output=%u\n",
        s->config.motor,s->config.encoder,(double)s->config.ppr,(unsigned)s->decoding,(double)s->config.gear_ratio,
        (int)s->config.motor_polarity,(int)s->config.encoder_polarity,(unsigned)s->config.encoder_on_output);
    if (s->decoding && s->encoder_counts_per_rev>0)
        solar_os_shell_io_printf(io,"  encoder-counts-per-joint-rev=%.6g (derived)\n",(double)s->encoder_counts_per_rev);
    else
        solar_os_shell_io_writeln(io,"  encoder-counts-per-joint-rev=pending calibration/decoder sample");
    solar_os_shell_io_printf(io,
        "  target=%.6g setpoint=%.6g velocity=%.6g valid=%u position=%.6g valid=%u referenced=%u commanded-position=%.6g\n",
        (double)s->target,(double)s->setpoint,(double)s->velocity,(unsigned)s->velocity_valid,(double)s->position,
        (unsigned)s->position_valid,(unsigned)s->referenced,(double)s->commanded_position);
    solar_os_shell_io_printf(io,
        "  output=%.6g P=%.6g I=%.6g D=%.6g saturated=%u count=%ld sample-us=%llu expiry-us=%llu\n",
        (double)s->output,(double)s->p,(double)s->i,(double)s->d,(unsigned)s->saturated,(long)s->raw_count,
        (unsigned long long)s->sample_us,(unsigned long long)s->command_expires_us);
    solar_os_shell_io_printf(io,
        "  command-valid=%u command-timeout-ms=%u feedback-timeout-ms=%u max-gap-ms=%u\n",
        (unsigned)s->command_valid,(unsigned)s->config.command_timeout_ms,
        (unsigned)s->config.feedback_timeout_ms,(unsigned)s->config.max_gap_ms);
    if (s->config.kind==SOLAR_OS_ROBOT_MOTOR_VELOCITY && (s->config.ppr==0 || s->config.gear_ratio==0))
        solar_os_shell_io_writeln(io,"  calibration incomplete: configure ppr and gear_ratio explicitly before enable");
    if (s->state==SOLAR_OS_ROBOT_WAITING)
        solar_os_shell_io_writeln(io,s->last_error==SOLAR_OS_NATIVE_ERROR_TIMEOUT?
            "  command expired; output stopped; send a fresh command to resume":
            "  waiting for first command; output stopped; command watchdog not armed");
    if (s->state==SOLAR_OS_ROBOT_FAULT)
        solar_os_shell_io_printf(io,"  recovery: robot joint disable %s before configuring or enabling again\n",s->config.name);
    solar_os_shell_io_printf(io,
        "  friction-bias-forward=%.6g friction-bias-reverse=%.6g velocity-deadband=%.6g feedforward=%.6g\n",
        (double)s->config.friction_bias_forward,(double)s->config.friction_bias_reverse,
        (double)s->config.velocity_deadband,(double)s->feedforward);
    solar_os_shell_io_printf(io,
        "  kp=%.6g ki=%.6g kd=%.6g tau=%.6g velocity-tau=%.6g max-drive=%.6g max-velocity=%.6g acceleration=%.6g\n",
        (double)s->config.kp,(double)s->config.ki,(double)s->config.kd,(double)s->config.derivative_tau,
        (double)s->config.velocity_tau,(double)s->config.max_drive,(double)s->config.velocity_limit,(double)s->config.acceleration_limit);
}
static void joint_command(solar_os_context_t *ctx,int argc,char **argv)
{
    solar_os_shell_io_t *io=solar_os_shell_command_io(ctx);
    esp_err_t err=ESP_ERR_INVALID_ARG;
    float v;
    if (argc==1) { solar_os_shell_io_writeln(io,usage); return; }
    if (argc==2 && !strcmp(argv[1],"list")) {
        size_t n=solar_os_robot_joint_count();
        if (!n) solar_os_shell_io_writeln(io,"No joints open; robot library is loaded on create");
        for (size_t i=0;i<n;i++) { solar_os_robot_joint_info_v3_t s; if (solar_os_robot_joint_get_info(i,&s)) show(io,&s); }
        return;
    }
    if (argc==4 && !strcmp(argv[1],"create") && (!strcmp(argv[3],"velocity") || !strcmp(argv[3],"servo")))
        err=solar_os_robot_joint_create(argv[2],!strcmp(argv[3],"velocity")?SOLAR_OS_ROBOT_MOTOR_VELOCITY:SOLAR_OS_ROBOT_SERVO_POSITION,"shell");
    else if (argc==5 && !strcmp(argv[1],"configure") && number(argv[4],&v)) err=solar_os_robot_joint_configure(argv[2],argv[3],v);
    else if (argc==5 && !strcmp(argv[1],"motor")) err=solar_os_robot_joint_bind_motor(argv[2],argv[3],argv[4]);
    else if (argc==4 && !strcmp(argv[1],"command") && number(argv[3],&v)) err=solar_os_robot_joint_command(argv[2],v);
    else if (argc==3 && !strcmp(argv[1],"status")) {
        solar_os_robot_joint_info_v3_t s; err=solar_os_robot_joint_status(argv[2],&s); if (err==ESP_OK) show(io,&s);
    } else if (argc==3 && !strcmp(argv[1],"enable")) err=solar_os_robot_joint_enable(argv[2]);
    else if (argc==3 && !strcmp(argv[1],"disable")) err=solar_os_robot_joint_disable(argv[2]);
    else if (argc==3 && !strcmp(argv[1],"close")) err=solar_os_robot_joint_close(argv[2]);
    else if ((argc==9 || argc==10) && !strcmp(argv[1],"servo") && strlen(argv[3])<20) {
        solar_os_native_servo_config_v1_t c={.frequency_hz=50}; size_t output,min,max,freq=50;
        if (solar_os_shell_parse_size_arg(argv[4],0,UINT16_MAX,&output) &&
            solar_os_shell_parse_size_arg(argv[5],1,1000000,&min) && solar_os_shell_parse_size_arg(argv[6],1,1000000,&max) &&
            number(argv[7],&c.angle_min) && number(argv[8],&c.angle_max) &&
            (argc==9 || solar_os_shell_parse_size_arg(argv[9],1,1000,&freq))) {
            c.output=output;c.pulse_min_us=min;c.pulse_max_us=max;c.frequency_hz=freq;
            strlcpy(c.provider,argv[3],sizeof(c.provider)); err=solar_os_robot_joint_bind_servo(argv[2],&c);
        }
    }
    if (err!=ESP_OK) {
        solar_os_shell_io_printf(io,"robot joint: %s\n",solar_os_shell_error_text(err));
        solar_os_robot_joint_info_v3_t s;
        if (err==ESP_ERR_INVALID_STATE && argc>=3 && solar_os_robot_joint_status(argv[2],&s)==ESP_OK) {
            if (s.state==SOLAR_OS_ROBOT_FAULT)
                solar_os_shell_io_printf(io,"Joint is faulted (error=%d); run 'robot joint disable %s' before configuring, enabling or commanding again\n",
                    (int)s.last_error,s.config.name);
            else if (!strcmp(argv[1],"enable") && s.config.kind==SOLAR_OS_ROBOT_MOTOR_VELOCITY &&
                     (s.config.ppr==0 || s.config.gear_ratio==0))
                solar_os_shell_io_writeln(io,"Missing calibration: configure ppr and gear_ratio explicitly before enable");
            else if (s.claimed)
                solar_os_shell_io_printf(io,"Joint owns hardware; run 'robot joint disable %s' before changing configuration or bindings\n",s.config.name);
        }
        if (err==ESP_ERR_NOT_FOUND) solar_os_shell_io_writeln(io,"Check joint names and installed robot library");
        if (err==ESP_ERR_INVALID_ARG) solar_os_shell_io_writeln(io,usage);
    }
}
static void model_command(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    char error[128] = {0};
    if (argc == 1) { solar_os_shell_io_writeln(io, model_usage); return; }
    if (argc == 3 && !strcmp(argv[1], "load")) {
        char path[256];
        err = solar_os_shell_resolve_path(ctx, argv[2], path, sizeof(path));
        if (err == ESP_OK) err = solar_os_robot_model_load_file(path, error, sizeof(error));
        if (err == ESP_OK) solar_os_shell_io_writeln(io, "Robot model loaded");
    } else if (argc == 2 && !strcmp(argv[1], "unload")) err = solar_os_robot_model_unload();
    else if (argc == 2 && (!strcmp(argv[1], "info") || !strcmp(argv[1], "links") ||
        !strcmp(argv[1], "joints") || !strcmp(argv[1], "metadata"))) {
        solar_os_robot_model_info_t info;
        err = solar_os_robot_model_get_info(&info);
        if (err == ESP_OK && !strcmp(argv[1], "info"))
            solar_os_shell_io_printf(io, "%s: root=%s links=%u joints=%u known=%u solar_os-bytes=%u\n",
                info.name, info.root, (unsigned)info.links, (unsigned)info.joints,
                (unsigned)info.known_joints, (unsigned)info.extension_bytes);
        else if (err == ESP_OK && !strcmp(argv[1], "links")) {
            for (uint32_t i = 0; i < info.links && err == ESP_OK; ++i) {
                char name[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
                err = solar_os_robot_model_get_link(i, name, sizeof(name));
                if (err == ESP_OK) solar_os_shell_io_writeln(io, name);
            }
        } else if (err == ESP_OK && !strcmp(argv[1], "joints")) {
            static const char *kinds[] = {"fixed", "revolute", "continuous", "prismatic"};
            for (uint32_t i = 0; i < info.joints && err == ESP_OK; ++i) {
                solar_os_robot_model_joint_info_t joint;
                err = solar_os_robot_model_get_joint(i, &joint);
                if (err == ESP_OK) solar_os_shell_io_printf(io, "%s: %s %s -> %s position=%.9g known=%u\n",
                    joint.name, joint.kind < 4 ? kinds[joint.kind] : "unknown", joint.parent, joint.child,
                    (double)joint.position, (unsigned)joint.position_known);
            }
        } else if (err == ESP_OK) {
            char *xml = solar_os_memory_alloc(info.extension_bytes + 1, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "robot.metadata");
            if (!xml) err = ESP_ERR_NO_MEM;
            else {
                err = solar_os_robot_model_get_extension(xml, info.extension_bytes + 1);
                if (err == ESP_OK) solar_os_shell_io_writeln(io, xml);
                solar_os_memory_free(xml);
            }
        }
    } else if (argc >= 4 && !(argc % 2) && !strcmp(argv[1], "set")) {
        uint32_t count = (uint32_t)(argc - 2)/2;
        if (count <= SOLAR_OS_ROBOT_MODEL_JOINT_MAX) {
            const char *names[SOLAR_OS_ROBOT_MODEL_JOINT_MAX];
            float values[SOLAR_OS_ROBOT_MODEL_JOINT_MAX];
            bool valid = true;
            for (uint32_t i = 0; i < count; ++i) {
                names[i] = argv[2+2*i];
                if (!number(argv[3+2*i], &values[i])) valid = false;
            }
            if (valid) err = solar_os_robot_model_update(names, values, count);
        }
    } else if (argc == 4 && !strcmp(argv[1], "transform")) {
        solar_os_robot_transform_t t;
        err = solar_os_robot_model_transform(argv[2], argv[3], &t);
        if (err == ESP_OK) solar_os_shell_io_printf(io,
            "%s <- %s: xyz=[%.9g %.9g %.9g] quaternion-xyzw=[%.9g %.9g %.9g %.9g]\n",
            argv[2], argv[3], (double)t.translation[0], (double)t.translation[1], (double)t.translation[2],
            (double)t.rotation[0], (double)t.rotation[1], (double)t.rotation[2], (double)t.rotation[3]);
    } else if (argc == 7 && (!strcmp(argv[1], "point") || !strcmp(argv[1], "vector"))) {
        float v[3], out[3];
        if (number(argv[4], &v[0]) && number(argv[5], &v[1]) && number(argv[6], &v[2])) {
            err = solar_os_robot_model_apply(argv[2], argv[3], v, !strcmp(argv[1], "vector"), out);
            if (err == ESP_OK) solar_os_shell_io_printf(io, "[%.9g %.9g %.9g]\n", (double)out[0], (double)out[1], (double)out[2]);
        }
    }
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE && argc >= 4 &&
            (!strcmp(argv[1], "transform") || !strcmp(argv[1], "point") || !strcmp(argv[1], "vector")))
            solar_os_robot_model_transform_error(argv[2], argv[3], error, sizeof(error));
        solar_os_shell_io_printf(io, "robot model: %s%s%s\n", solar_os_shell_error_text(err), *error ? ": " : "", error);
        if (err == ESP_ERR_INVALID_STATE && (!strcmp(argv[1], "load") || !strcmp(argv[1], "unload")))
            solar_os_shell_io_writeln(io, "Run robot stop before replacing or unloading the model");
        if (err == ESP_ERR_INVALID_STATE && !strcmp(argv[1], "set"))
            solar_os_shell_io_writeln(io, "Bound joints use live feedback; stop the robot before supplying manual model state");
        if (err == ESP_ERR_INVALID_ARG) solar_os_shell_io_writeln(io, model_usage);
    }
}

void solar_os_shell_cmd_robot(solar_os_context_t *ctx,int argc,char **argv)
{
    if (argc==2 && (!strcmp(argv[1],"start") || !strcmp(argv[1],"stop"))) {
        bool start=!strcmp(argv[1],"start");
        esp_err_t err=start?solar_os_robot_start():solar_os_robot_stop();
        solar_os_shell_io_t *io=solar_os_shell_command_io(ctx);
        if(err==ESP_OK) solar_os_shell_io_writeln(io,start?"robot: controllers started; waiting for commands":"robot: controllers stopped");
        else {
            solar_os_shell_io_printf(io,"robot %s: %s\n",argv[1],solar_os_shell_error_text(err));
            solar_os_shell_io_writeln(io,"Inspect robot joint list; robot stop retries incomplete cleanup");
        }
    }
    else if (argc >= 2 && !strcmp(argv[1], "joint")) joint_command(ctx, argc-1, argv+1);
    else if (argc >= 2 && !strcmp(argv[1], "model")) model_command(ctx, argc-1, argv+1);
    else solar_os_shell_io_writeln(solar_os_shell_command_io(ctx), "usage: robot start|stop | joint|model <operation> [arguments]");
}
