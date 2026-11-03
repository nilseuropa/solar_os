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
    "  controller bind <name> <stream>|manual parameter:<path>|pwm:<provider>:<output>\n"
    "  controller feed <name> <measurement>\n"
    "  controller step <name> <measurement> <dt-seconds>\n"
    "  controller manual <name> <output> | auto <name>\n"
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
    solar_os_shell_io_table_printf(io,
        "%s: %s %s mode=%s claimed=%u source=%s target=%s\n"
        "  setpoint=%.6g range=%.6g..%.6g measurement=%.6g output=%.6g error=%.6g P=%.6g I=%.6g D=%.6g saturated=%u\n"
        "  kp=%.6g ki=%.6g kd=%.6g tau=%.6g reverse=%u limits=%.6g..%.6g hysteresis=%.6g\n"
        "  period=%ums timeout=%ums max_dt=%.6gs safe=%.6g frequency=%uHz sample=%llums steps=%u faults=%u last=%s\n",
        c->name, c->algorithm.kind == SOLAR_OS_CONTROLLER_PID ? "pid" : "hysteresis",
        info->faulted ? "fault" : info->enabled ? "running" : "stopped",
        info->state.automatic ? "auto" : "manual", (unsigned)info->claimed,
        c->source[0] ? c->source : "manual", c->target[0] ? c->target : "unbound",
        (double)c->setpoint, (double)c->setpoint_min, (double)c->setpoint_max,
        (double)info->measurement, (double)info->state.output,
        (double)info->state.error, (double)info->state.p, (double)info->state.i, (double)info->state.d,
        (unsigned)info->state.saturated, (double)c->algorithm.kp, (double)c->algorithm.ki,
        (double)c->algorithm.kd, (double)c->algorithm.derivative_tau, (unsigned)c->algorithm.reverse,
        (double)c->algorithm.output_min, (double)c->algorithm.output_max, (double)c->algorithm.hysteresis,
        (unsigned)c->period_ms, (unsigned)c->timeout_ms, (double)c->algorithm.max_dt,
        (double)c->safe_output, (unsigned)c->frequency_hz, (unsigned long long)info->sample_ms,
        (unsigned)info->steps, (unsigned)info->faults, solar_os_shell_error_text(info->last_error));
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
    } else if (argc == 3) {
        if (strcmp(argv[1], "delete") == 0) err = solar_os_controller_delete(argv[2]);
        else if (strcmp(argv[1], "auto") == 0) err = solar_os_controller_auto(argv[2]);
        else if (strcmp(argv[1], "reset") == 0) err = solar_os_controller_reset(argv[2]);
        else if (strcmp(argv[1], "start") == 0) err = solar_os_controller_start(argv[2]);
        else if (strcmp(argv[1], "stop") == 0) err = solar_os_controller_stop(argv[2]);
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "controller: %s\n", solar_os_shell_error_text(err));
        if (err == ESP_ERR_INVALID_ARG) solar_os_shell_io_writeln(io, usage);
    }
}
