#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell.h"
#include "solar_os_servo.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool parse_uint(const char *text, uint32_t maximum, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-') return false;
    char *end;
    errno = 0;
    const unsigned long result = strtoul(text, &end, 0);
    if (errno != 0 || *end != '\0' || result > maximum) return false;
    *value = (uint32_t)result;
    return true;
}

static bool parse_angle(const char *text, float *value)
{
    if (text == NULL || text[0] == '\0') return false;
    char *end;
    errno = 0;
    *value = strtof(text, &end);
    return errno == 0 && *end == '\0' && isfinite(*value);
}

static void usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "servo list | status [name]");
    solar_os_shell_io_writeln(term, "servo open <name> <output> <min-us> <max-us> <min-deg> <max-deg> [freq-hz [inverted:0|1]]");
    solar_os_shell_io_writeln(term, "servo set <name> <angle-deg> | pulse <name> <pulse-us>");
    solar_os_shell_io_writeln(term, "servo disable <name> | close <name>");
}

static void print_info(solar_os_shell_io_t *term, const solar_os_servo_info_t *info,
                       bool detailed)
{
    const solar_os_servo_config_t *c = &info->config;
    char output[SOLAR_OS_PWM_ENDPOINT_MAX];
    solar_os_pwm_format_output(&c->output, output, sizeof(output));
    if (!detailed) {
        solar_os_shell_io_table_printf(term, "%s %s %s owner=%s\r\n",
            info->name, output, info->enabled ? "enabled" : "disabled", info->owner);
        return;
    }
    solar_os_shell_io_table_printf(term,
        "%s %s owner=%s %s %" PRIu32 " Hz pulses=%" PRIu32 "..%" PRIu32
        " us angles=%.3f..%.3f deg%s\r\n",
        info->name, output, info->owner, info->enabled ? "enabled" : "disabled",
        c->freq_hz, c->pulse_min_us, c->pulse_max_us,
        c->angle_min_deg, c->angle_max_deg, c->inverted ? " inverted" : "");
    if (info->has_command) {
        solar_os_shell_io_table_printf(term, "  commanded %.3f deg / %" PRIu32 " us\r\n",
            info->commanded_angle_deg, info->commanded_pulse_us);
    }
}

void solar_os_shell_cmd_servo(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = solar_os_shell_command_io(ctx);
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (argc == 1 || (argc == 2 &&
        (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "status") == 0))) {
        solar_os_servo_info_t info;
        const bool detailed = argc == 2 && strcmp(argv[1], "status") == 0;
        if (solar_os_servo_count() == 0U) solar_os_shell_io_writeln(term, "no servos open");
        for (size_t i = 0; solar_os_servo_get(i, &info); i++) print_info(term, &info, detailed);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_servo_info_t info;
        ret = solar_os_servo_status(argv[2], &info);
        if (ret == ESP_OK) { print_info(term, &info, true); return; }
    } else if (argc >= 8 && argc <= 10 && strcmp(argv[1], "open") == 0) {
        uint32_t inverted = 0;
        solar_os_servo_config_t config = {.freq_hz = SOLAR_OS_SERVO_FREQ_DEFAULT_HZ};
        if (!solar_os_pwm_parse_output(argv[3], &config.output) ||
            !parse_uint(argv[4], UINT32_MAX, &config.pulse_min_us) ||
            !parse_uint(argv[5], UINT32_MAX, &config.pulse_max_us) ||
            !parse_angle(argv[6], &config.angle_min_deg) ||
            !parse_angle(argv[7], &config.angle_max_deg) ||
            (argc >= 9 && !parse_uint(argv[8], SOLAR_OS_SERVO_FREQ_MAX_HZ, &config.freq_hz)) ||
            (argc == 10 && !parse_uint(argv[9], 1U, &inverted))) {
            usage(term);
            return;
        }
        config.inverted = inverted != 0U;
        ret = solar_os_servo_open(argv[2], "shell", &config);
        if (ret == ESP_OK) {
            solar_os_shell_io_printf(term, "%s opened disabled\r\n", argv[2]);
            return;
        }
    } else if (argc == 4 && strcmp(argv[1], "set") == 0) {
        float angle;
        if (parse_angle(argv[3], &angle)) ret = solar_os_servo_set(argv[2], "shell", angle);
    } else if (argc == 4 && strcmp(argv[1], "pulse") == 0) {
        uint32_t pulse;
        if (parse_uint(argv[3], UINT32_MAX, &pulse)) ret = solar_os_servo_pulse(argv[2], "shell", pulse);
    } else if (argc == 3 && strcmp(argv[1], "disable") == 0) {
        ret = solar_os_servo_disable(argv[2], "shell");
    } else if (argc == 3 && strcmp(argv[1], "close") == 0) {
        ret = solar_os_servo_close(argv[2], "shell");
    } else {
        usage(term);
        return;
    }
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term, "servo: %s\r\n", solar_os_shell_error_text(ret));
        if (ret == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "output already owned or shared PWM timer has another frequency");
        }
    }
}
