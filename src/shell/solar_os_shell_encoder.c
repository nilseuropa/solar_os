#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <inttypes.h>
#include <string.h>
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
    } else if (argc == 3 && strcmp(argv[1], "read") == 0) {
        int32_t count;
        ret = solar_os_encoder_read(argv[2], &count);
        if (ret == ESP_OK) solar_os_shell_io_printf(io, "%" PRId32 "\r\n", count);
    } else if (argc == 3 && strcmp(argv[1], "reset") == 0) ret = solar_os_encoder_reset(argv[2]);
    else {
        solar_os_shell_io_writeln(io, "usage: encoder [list] | status|read|reset <name>");
        return;
    }
    if (ret != ESP_OK) solar_os_shell_io_printf(io, "encoder: %s\r\n", solar_os_shell_error_text(ret));
}
