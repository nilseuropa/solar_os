#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include "solar_os_encoder.h"

void solar_os_shell_cmd_encoder(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        solar_os_encoder_info_t info;
        if (!solar_os_encoder_get(0U, &info)) solar_os_shell_io_writeln(io, "no encoders attached");
        for (size_t i = 0; solar_os_encoder_get(i, &info); i++)
            solar_os_shell_io_table_printf(io, "%s driver=%s mode=x%d reset=%u\r\n",
                info.name, info.driver, (int)info.mode, info.can_reset);
        return;
    }
    esp_err_t ret;
    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_encoder_status_t status;
        ret = solar_os_encoder_status(argv[2], &status);
        if (ret == ESP_OK) solar_os_shell_io_printf(io, "%s driver=%s mode=x%d count=%" PRId32 " reset=%u\r\n",
            status.info.name, status.info.driver, (int)status.info.mode, status.count, status.info.can_reset);
        if(ret==ESP_OK) {
            solar_os_shell_io_printf(io,
            "  stream=enc.%s claimed=%u ppr=%.6g ratio=%.6g polarity=%d output=%u\n",
            status.info.name,(unsigned)status.claimed,(double)status.config.ppr,(double)status.config.gear_ratio,
            (int)status.config.polarity,(unsigned)status.config.encoder_on_output);
            solar_os_shell_io_printf(io,
            "  velocity-tau=%.6gs position-zero=%.6g counts-per-rev=%.9g (derived)\n",
            (double)status.config.velocity_tau,(double)status.config.position_zero,
            solar_os_native_encoder_counts_per_rev(&status.config,status.info.mode));
            solar_os_shell_io_printf(io,
            "  velocity=%.6g rad/s position=%.6g rad valid=%u sample-us=%llu\n",
            (double)status.feedback.velocity,(double)status.feedback.position,(unsigned)status.feedback.valid,
            (unsigned long long)status.feedback.timestamp_us);
        }
    } else if (argc == 3 && strcmp(argv[1], "read") == 0) {
        int32_t count;
        ret = solar_os_encoder_read(argv[2], &count);
        if (ret == ESP_OK) solar_os_shell_io_printf(io, "%" PRId32 "\r\n", count);
    } else if (argc == 3 && strcmp(argv[1], "reset") == 0) ret = solar_os_encoder_reset(argv[2]);
    else if(argc==5 && !strcmp(argv[1],"configure")) {
        char *end;errno=0;float value=strtof(argv[4],&end);
        ret=errno || end==argv[4] || *end || !isfinite(value)?ESP_ERR_INVALID_ARG:
            solar_os_encoder_configure(argv[2],argv[3],value);
    }
    else {
        solar_os_shell_io_writeln(io, "usage: encoder [list] | status|read|reset <name> | configure <name> <key> <value>\n"
            "keys: ppr gear_ratio polarity encoder_on_output velocity_tau position_zero");
        return;
    }
    if (ret != ESP_OK) solar_os_shell_io_printf(io, "encoder: %s\r\n", solar_os_shell_error_text(ret));
}
