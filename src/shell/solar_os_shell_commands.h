#pragma once

#include "solar_os.h"
#include "solar_os_config.h"

void solar_os_shell_cmd_apps(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_adc(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_audio(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_battery(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_ble(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_clear(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_date(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_df(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_gpio(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_humidity(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_i2c(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_job(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_jobs(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_log(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_mem(solar_os_context_t *ctx, int argc, char **argv);
#if SOLAR_OS_PACKAGE_NET
void solar_os_shell_cmd_mqtt(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_netscan(solar_os_context_t *ctx, int argc, char **argv);
#endif
void solar_os_shell_cmd_ntp(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_ota(solar_os_context_t *ctx, int argc, char **argv);
#if SOLAR_OS_PACKAGE_NET
void solar_os_shell_cmd_ping(solar_os_context_t *ctx, int argc, char **argv);
#endif
void solar_os_shell_cmd_pkg(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_port(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_power(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_pwm(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_sd(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_setterm(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_sleep(solar_os_context_t *ctx, int argc, char **argv);
#if SOLAR_OS_PACKAGE_NET
void solar_os_shell_cmd_sshkey(solar_os_context_t *ctx, int argc, char **argv);
#endif
void solar_os_shell_cmd_status(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_temperature(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_time(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_top(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_uart(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_uptime(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_version(solar_os_context_t *ctx, int argc, char **argv);
void solar_os_shell_cmd_wifi(solar_os_context_t *ctx, int argc, char **argv);
