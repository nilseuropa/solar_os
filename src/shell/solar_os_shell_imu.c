#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "solar_os_imu.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static const char *default_name(void)
{
    static EXT_RAM_BSS_ATTR solar_os_imu_info_t info;
    return solar_os_imu_get(0U, &info) ? info.name : NULL;
}

static void print_capabilities(solar_os_shell_io_t *term,
                               solar_os_imu_capabilities_t capabilities)
{
    if ((capabilities & SOLAR_OS_IMU_CAP_ACCELERATION) != 0U) {
        solar_os_shell_io_write(term, " accel");
    }
    if ((capabilities & SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY) != 0U) {
        solar_os_shell_io_write(term, " gyro");
    }
    if ((capabilities & SOLAR_OS_IMU_CAP_ORIENTATION) != 0U) {
        solar_os_shell_io_write(term, " orientation");
    }
}

void solar_os_shell_cmd_imu(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        solar_os_imu_info_t info;
        for (size_t i = 0; solar_os_imu_get(i, &info); i++) {
            solar_os_shell_io_printf(term, "%s  %s", info.name, info.driver);
            print_capabilities(term, info.capabilities);
            solar_os_shell_io_write(term, "\r\n");
        }
        return;
    }
    if (argc < 2 || argc > 4 || strcmp(argv[1], "sample") != 0) {
        solar_os_shell_io_writeln(
            term, "usage: imu [list] | imu sample [name] [timeout-ms]");
        return;
    }
    const char *name = argc >= 3 ? argv[2] : default_name();
    const uint32_t timeout_ms = argc == 4 ?
        (uint32_t)strtoul(argv[3], NULL, 0) : 1000U;
    if (name == NULL || timeout_ms == 0U) {
        solar_os_shell_io_writeln(term, "imu: no sensor or invalid timeout");
        return;
    }

    solar_os_imu_sample_t sample;
    const esp_err_t ret = solar_os_imu_read_sample(name, timeout_ms, &sample);
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term, "imu: %s\r\n", esp_err_to_name(ret));
        return;
    }
    solar_os_shell_io_printf(term,
                             "%s timestamp-us=%" PRIu64 "\r\n",
                             name,
                             sample.timestamp_us);
    if ((sample.valid & SOLAR_OS_IMU_CAP_ACCELERATION) != 0U) {
        solar_os_shell_io_printf(term,
                                 "accel-m/s2 x=%.6f y=%.6f z=%.6f\r\n",
                                 sample.acceleration_m_s2[0],
                                 sample.acceleration_m_s2[1],
                                 sample.acceleration_m_s2[2]);
    }
    if ((sample.valid & SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY) != 0U) {
        solar_os_shell_io_printf(term,
                                 "gyro-rad/s x=%.6f y=%.6f z=%.6f\r\n",
                                 sample.angular_velocity_rad_s[0],
                                 sample.angular_velocity_rad_s[1],
                                 sample.angular_velocity_rad_s[2]);
    }
    if ((sample.valid & SOLAR_OS_IMU_CAP_ORIENTATION) != 0U) {
        solar_os_shell_io_printf(term,
                                 "orientation w=%.6f x=%.6f y=%.6f z=%.6f\r\n",
                                 sample.orientation[0],
                                 sample.orientation[1],
                                 sample.orientation[2],
                                 sample.orientation[3]);
    }
}
