#include "solar_os_shell_commands.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_controllers.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_time.h"

static const char usage[] =
    "controller list | status <name>\n"
    "  controller create <name> pid|hysteresis | delete <name>\n"
    "  controller configure <name> <key> <value>\n"
    "  controller bind <name> <stream>|manual parameter:<path>|pwm:<provider>:<output>|stream:<sink>\n"
    "  controller feed <name> <measurement>\n"
    "  controller step <name> <measurement> <dt-seconds>\n"
    "  controller manual <name> <output> | auto <name>\n"
    "  controller tune <name> [band [cycles [timeout-ms]]]\n"
    "  controller start|stop|reset <name>\n";

static bool number(const char *text, float *value)
{
    char *end; errno = 0;
    *value = strtof(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(*value);
}

static void print_info(solar_os_shell_io_t *io, const solar_os_controller_info_t *info)
{
    const solar_os_controller_config_t *c = &info->config;
    solar_os_shell_io_printf(io,
        "%s: %s %s mode=%s claimed=%u\n",
        c->name, c->algorithm.kind == SOLAR_OS_CONTROLLER_PID ? "pid" : "hysteresis",
        info->faulted ? "fault" : info->tuning.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING?"tuning":
        info->enabled ? info->waiting?"waiting":"running" : "stopped",
        info->state.automatic ? "auto" : "manual", (unsigned)info->claimed);
    solar_os_shell_io_printf(io,"  source=%s\n",c->source[0] ? c->source : info->owner[0]?"feed":"manual");
    solar_os_shell_io_printf(io,"  target=%s\n",c->target[0] ? c->target : info->owner[0]?"owner":"unbound");
    solar_os_shell_io_printf(io,"  setpoint=%.6g range=%.6g..%.6g measurement=%.6g output=%.6g\n",
        (double)c->setpoint, (double)c->setpoint_min, (double)c->setpoint_max,
        (double)info->measurement, (double)(info->state.output+info->feedforward));
    solar_os_shell_io_printf(io,"  error=%.6g P=%.6g I=%.6g D=%.6g saturated=%u\n",
        (double)info->state.error, (double)info->state.p, (double)info->state.i, (double)info->state.d,
        (unsigned)info->state.saturated);
    solar_os_shell_io_printf(io,"  kp=%.6g ki=%.6g kd=%.6g tau=%.6g reverse=%u limits=%.6g..%.6g hysteresis=%.6g\n",
        (double)c->algorithm.kp, (double)c->algorithm.ki,
        (double)c->algorithm.kd, (double)c->algorithm.derivative_tau, (unsigned)c->algorithm.reverse,
        (double)c->algorithm.output_min, (double)c->algorithm.output_max, (double)c->algorithm.hysteresis);
    solar_os_shell_io_printf(io,"  period=%ums timeout=%ums max_dt=%.6gs safe=%.6g frequency=%uHz\n",
        (unsigned)c->period_ms, (unsigned)c->timeout_ms, (double)c->algorithm.max_dt,
        (double)c->safe_output, (unsigned)c->frequency_hz);
    solar_os_shell_io_printf(io,"  sample=%llums steps=%u faults=%u last=%s\n",(unsigned long long)info->sample_ms,
        (unsigned)info->steps, (unsigned)info->faults, solar_os_shell_error_text(info->last_error));
    if(info->owner[0]) solar_os_shell_io_printf(io,
        "  owner=%s clock=owner feedforward=%.6g\n",
        info->owner,(double)info->feedforward);
    if(c->command_timeout_ms || c->acceleration_limit || c->friction_bias_forward || c->friction_bias_reverse)
        solar_os_shell_io_printf(io,
            "  active-setpoint=%.6g command-valid=%u command-timeout=%ums\n"
            "  acceleration=%.6g deadband=%.6g friction-forward=%.6g reverse=%.6g\n",
            (double)info->active_setpoint,(unsigned)info->command_valid,(unsigned)c->command_timeout_ms,
            (double)c->acceleration_limit,(double)c->velocity_deadband,(double)c->friction_bias_forward,(double)c->friction_bias_reverse);
    if(info->tuning.state!=SOLAR_OS_CONTROLLER_TUNE_IDLE) {
        const solar_os_controller_tune_result_t *t=&info->tuning;
        solar_os_shell_io_printf(io,"  tune=%s reason=%s cycles=%u/%u elapsed=%.3gs\n",
            solar_os_controller_tune_state_name(t->state),solar_os_controller_tune_reason_name(t->reason),
            (unsigned)t->cycles,(unsigned)t->required_cycles,(double)t->elapsed);
        if(t->state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE)
            solar_os_shell_io_printf(io,
                "  candidate kp=%.9g ki=%.9g\n"
                "  kd=%.9g tau=%.9g (existing gains unchanged)\n"
                "  relay amplitude=%.6g period=%.6gs\n"
                "  ultimate-gain=%.6g\n",
                (double)t->kp,(double)t->ki,(double)t->kd,(double)t->derivative_tau,
                (double)t->amplitude,(double)t->period,(double)t->ultimate_gain);
    }
}

void solar_os_shell_cmd_controller(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    float a, b, output;
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        size_t count = solar_os_controller_count();
        if (count == 0) solar_os_shell_io_table_writeln(io, "No controllers configured");
        for (size_t i = 0; i < count; i++) {
            solar_os_controller_info_t info;
            if (solar_os_controller_get_info(i, &info)) print_info(io, &info);
        }
        return;
    }
    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_controller_info_t info;
        err = solar_os_controller_find(argv[2], &info);
        if (err == ESP_OK) print_info(io, &info);
    } else if (argc == 4 && strcmp(argv[1], "create") == 0 &&
        (strcmp(argv[3], "pid") == 0 || strcmp(argv[3], "hysteresis") == 0)) {
        err = solar_os_controller_create(argv[2], strcmp(argv[3], "pid") == 0 ?
            SOLAR_OS_CONTROLLER_PID : SOLAR_OS_CONTROLLER_HYSTERESIS);
    } else if (argc == 5 && strcmp(argv[1], "configure") == 0 && number(argv[4], &a)) {
        err = solar_os_controller_configure(argv[2], argv[3], a);
    } else if (argc == 5 && strcmp(argv[1], "bind") == 0) {
        err = solar_os_controller_bind(argv[2], argv[3], argv[4]);
    } else if (argc == 4 && strcmp(argv[1], "feed") == 0 && number(argv[3], &a)) {
        err = solar_os_controller_feed(argv[2], a, solar_os_time_uptime_ms());
    } else if (argc == 5 && strcmp(argv[1], "step") == 0 && number(argv[3], &a) && number(argv[4], &b)) {
        err = solar_os_controller_step(argv[2], a, b, &output);
        if (err == ESP_OK) solar_os_shell_io_printf(io, "%.6g\n", (double)output);
    } else if (argc == 4 && strcmp(argv[1], "manual") == 0 && number(argv[3], &a)) {
        err = solar_os_controller_manual(argv[2], a);
    } else if(argc>=3 && argc<=6 && !strcmp(argv[1],"tune")) {
        float band=0,cycles=6,timeout=30000;
        if((argc<4 || number(argv[3],&band)) && (argc<5 || number(argv[4],&cycles)) &&
            (argc<6 || number(argv[5],&timeout)) && cycles>=3 && cycles<=12 &&
            timeout>=1000 && timeout<=300000 && (float)(uint32_t)cycles==cycles && (float)(uint32_t)timeout==timeout) {
            err=solar_os_controller_tune(argv[2],band,(uint32_t)cycles,(uint32_t)timeout);
            if(err==ESP_OK) solar_os_shell_io_writeln(io,
                "Tuning started; inspect controller status. controller stop aborts. Existing gains will remain unchanged.");
        }
    } else if (argc == 3) {
        if (strcmp(argv[1], "delete") == 0) err = solar_os_controller_delete(argv[2]);
        else if (strcmp(argv[1], "auto") == 0) err = solar_os_controller_auto(argv[2]);
        else if (strcmp(argv[1], "reset") == 0) err = solar_os_controller_reset(argv[2]);
        else if (strcmp(argv[1], "start") == 0) err = solar_os_controller_start(argv[2]);
        else if (strcmp(argv[1], "stop") == 0) err = solar_os_controller_stop(argv[2]);
    }
    if (err != ESP_OK) {
        if(argc>=3 && !strcmp(argv[1],"tune") && err==ESP_ERR_NOT_SUPPORTED) {
            solar_os_controller_info_t info;
            if(solar_os_controller_find(argv[2],&info)==ESP_OK && info.owner[0]) {
                solar_os_shell_io_printf(io,
                    "controller tune: unavailable for owned controller (owner=%s); stop its owner and use a standalone controller\n",info.owner);
                return;
            }
            solar_os_shell_io_writeln(io,"controller tune: requires a standalone PID controller");
            return;
        }
        solar_os_shell_io_printf(io, "controller: %s\n", solar_os_shell_error_text(err));
        if (err == ESP_ERR_INVALID_ARG) solar_os_shell_io_writeln(io, usage);
    }
}
